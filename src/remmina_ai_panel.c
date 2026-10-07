/*
 * Remmina - The GTK+ Remote Desktop Client
 *
 * AI side panel: chat with an OpenAI-compatible LLM endpoint (default:
 * the local mlx-serve server) and let it "see" the active remote
 * connection (RDP/VNC screen capture, SSH terminal text) and type text
 * back into it.
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "remmina_ai_panel.h"

#include "remmina_public.h"
#include "remmina_pref.h"
#include "remmina_protocol_widget.h"
#include "remmina_ssh_plugin.h"
#include "remmina/remmina_trace_calls.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <curl/curl.h>
#include <string.h>

#define AI_DEFAULT_URL   "http://localhost:11434/v1"
#define AI_MAX_IMG_DIM   2048    /* downscale screenshots only past this size;
                                  * keeping native resolution preserves the
                                  * model's click-aim precision */
#define AI_MAX_TERM_CHARS 14000   /* max terminal characters sent to the model */

typedef enum {
	AI_JOB_CHAT = 0,
	AI_JOB_MODELS = 1
} AiJobKind;

/* forward declarations */
static gchar *ai_message_json(const gchar *role, const gchar *text);
static void ai_continue_after_limit(RemminaAiPanel *panel);
static void ai_on_model_changed(GtkComboBox *combo, gpointer data);
static void ai_refresh_models(RemminaAiPanel *panel);
static void ai_refresh_models_from_body(RemminaAiPanel *panel, const gchar *body);
static void ai_chat_append(RemminaAiPanel *panel, const gchar *who,
			   const gchar *tag, const gchar *text);
static void ai_panel_save_settings(RemminaAiPanel *panel);
static void ai_registry_bind(GtkWidget *proto, RemminaAiPanel *panel);
static void ai_registry_unbind(GtkWidget *proto);
static gboolean ai_on_chat_view_click(GtkWidget *view, GdkEventButton *ev,
				      gpointer data);
static void ai_on_scroll_changed(GtkAdjustment *adj, gpointer data);
static void ai_scroll_bottom(RemminaAiPanel *panel);
static gboolean ai_scroll_recheck(gpointer data);
static void ai_on_approval_allow(GtkButton *btn, gpointer data);
static void ai_on_approval_deny(GtkButton *btn, gpointer data);
static void ai_on_tools_toggled(GtkToggleButton *btn, gpointer data);
static gchar *ai_endpoint_url(RemminaAiPanel *panel, const gchar *suffix);
static void ai_send_chat_request(RemminaAiPanel *panel);
static void ai_history_trim(RemminaAiPanel *panel);
static void ai_stream_begin(RemminaAiPanel *panel);
static void ai_stream_end(RemminaAiPanel *panel);
static void ai_toggle_turn(RemminaAiPanel *panel, gint turn_no, gboolean collapse);
static void ai_add_tools(JsonObject *root);

/* Reference-counted, mutex-guarded bridge between the curl worker
 * thread (producer) and the GTK main thread (consumer) carrying
 * streamed assistant tokens. */
typedef struct _AiSink {
	GMutex   mutex;
	GString *pending;    /* text not yet drained into the UI */
	gint     refs;
	gboolean alive;      /* FALSE once the panel is destroyed */
} AiSink;

static AiSink *ai_sink_new(void)
{
	AiSink *s = g_new0(AiSink, 1);

	g_mutex_init(&s->mutex);
	s->pending = g_string_new(NULL);
	s->refs = 1;
	s->alive = TRUE;
	return s;
}

static AiSink *ai_sink_ref(AiSink *s)
{
	g_return_val_if_fail(s != NULL, NULL);
	g_atomic_int_inc(&s->refs);
	return s;
}

static void ai_sink_unref(AiSink *s)
{
	if (g_atomic_int_dec_and_test(&s->refs)) {
		g_string_free(s->pending, TRUE);
		g_mutex_clear(&s->mutex);
		g_free(s);
	}
}

/* Producer side: called from the worker thread */
static void ai_sink_append(AiSink *s, const gchar *text, gssize len)
{
	g_mutex_lock(&s->mutex);
	if (s->alive)
		g_string_append_len(s->pending, text, len);
	g_mutex_unlock(&s->mutex);
}

/* One tool call accumulated from streamed deltas (or parsed whole from a
 * non-streaming response) */
typedef struct _AiToolCall {
	gchar   *id;
	gchar   *name;
	GString *args;
} AiToolCall;

/* used by the approval handlers, defined with the tool loop below */
static void ai_run_toolcall(RemminaAiPanel *panel, AiToolCall *tc);
static void ai_tool_round_tail(RemminaAiPanel *panel);

static AiToolCall *ai_tool_call_new(void)
{
	AiToolCall *tc = g_new0(AiToolCall, 1);

	tc->args = g_string_new(NULL);
	return tc;
}

static void ai_tool_call_free(gpointer p)
{
	AiToolCall *tc = p;

	g_free(tc->id);
	g_free(tc->name);
	g_string_free(tc->args, TRUE);
	g_free(tc);
}

typedef struct _AiHttpJob {
	RemminaAiPanel *panel;      /* only dereferenced through weak_ref */
	GWeakRef        weak_ref;
	AiJobKind       kind;
	gboolean        is_get;
	gchar          *url;        /* full endpoint url */
	gchar          *api_key;
	gchar          *payload;
	GString        *response;
	gboolean        is_stream;
	GString        *sse_line;   /* partial SSE line buffer (worker only) */
	GString        *content;    /* accumulated streamed text (worker only) */
	GPtrArray      *tool_calls; /* AiToolCall*, worker only, indexed by delta index */
	gboolean        finish_tool_calls; /* finish_reason was "tool_calls" */
	AiSink         *sink;       /* owned ref, NULL for non-stream jobs */
	volatile gboolean cancelled; /* set by Stop from the main thread; read
	                              * by the worker's write/progress cbs to
	                              * abort the transfer */
	char            errbuf[CURL_ERROR_SIZE];
	CURL           *easy;
} AiHttpJob;

struct _RemminaAiPanel {
	RemminaConnectionWindow *cnnwin;

	/* The session this panel belongs to (per-tab, not "visible tab") */
	GWeakRef         proto_ref;     /* RemminaProtocolWidget of my session */
	gboolean         connected;     /* kept fresh by update_context */

	/* UI */
	GtkWidget       *box;           /* top level */
	GtkWidget       *context_label;
	GtkWidget       *chat_view;     /* GtkTextView */
	GtkTextBuffer   *chat_buf;
	GtkWidget       *scrolled;      /* scrolled window around the view */
	GtkWidget       *input;         /* GtkEntry */
	GtkWidget       *send_btn;
	GtkWidget       *stop_btn;      /* cancel the running turn */
	GtkWidget       *type_btn;
	GtkWidget       *attach_check;  /* attach screenshot */
	GtkWidget       *enter_check;   /* press Enter after typing */
	GtkWidget       *model_combo;   /* combo with entry */
	GtkWidget       *url_entry;
	GtkWidget       *key_entry;
	GtkWidget       *rounds_spin;   /* max tool actions per turn */
	GtkWidget       *status_label;
	GtkWidget       *tools_check;   /* allow AI to click/type */

	/* State */
	GPtrArray       *history;       /* array of serialized message JSON strings */
	gboolean         busy;
	AiHttpJob       *active_job;    /* in-flight request, or NULL; main
	                                 * thread owns the pointer, the worker
	                                 * only reads its cancelled flag */
	gchar           *last_reply;    /* plain text of last assistant message */

	/* Streaming state (main thread only) */
	AiSink           *sink;         /* ref of the live stream sink, NULL when idle */
	guint            drain_timer;   /* g_timeout id, 0 when not streaming */
	gboolean         placeholder;   /* "Thinking…" placeholder is live */
	gint             drained;       /* chars already inserted into the buffer */
	gboolean         stick_bottom;  /* auto-scroll follows new text */
	gboolean         auto_scrolling;/* scroll() in progress: not user input */
	guint            scroll_idle;   /* deferred bottom-scroll timeout id, 0 = none */
	guint            recheck_timer; /* settle re-scroll timeout id */
	gboolean         scroll_recheck;/* a settle re-scroll is pending */
	gint64           scroll_deadline;/* swallow events until (monotonic ns) */
	gdouble          adj_value;     /* last seen scroll position */
	gdouble          adj_upper;     /* last seen scroll range, for growth */
	gint             tool_rounds;   /* tool-loop iterations this turn */
	gboolean         continue_pending; /* "continue" offer is live in chat */
	GPtrArray       *pending_tcs;   /* tool calls held for user approval */
	GtkWidget       *approval_bar;  /* inline approval prompt (hidden) */
	GtkWidget       *approval_label;
	GtkWidget       *confirm_check; /* "confirm risky actions" checkbox */
	gboolean         confirm_risky; /* gate risky typed commands */
	gint             turn_seq;      /* block counter for collapsible turns */
	gchar           *turn_tag;      /* body tag of the live turn, e.g. "turn3" */
	gchar           *turn_hdr_tag;  /* header marker tag, e.g. "h3" */

	/* Geometry of the last attached screenshot (for click mapping) */
	gint             img_w, img_h;      /* image pixels actually sent */
	gint             remote_w, remote_h; /* remote framebuffer size */

	/* Settings (owned strings, persisted in remmina.pref) */
	gchar           *api_url;
	gchar           *model;
	gchar           *api_key;
	gboolean         tools_enabled;
};

static void remmina_ai_panel_free(RemminaAiPanel *panel)
{
	guint i;

	g_return_if_fail(panel != NULL);

	if (panel->drain_timer)
		g_source_remove(panel->drain_timer);
	if (panel->scroll_idle)
		g_source_remove(panel->scroll_idle);
	if (panel->recheck_timer)
		g_source_remove(panel->recheck_timer);
	if (panel->pending_tcs)
		g_ptr_array_free(panel->pending_tcs, TRUE);
	if (panel->sink) {
		g_mutex_lock(&panel->sink->mutex);
		panel->sink->alive = FALSE;
		g_mutex_unlock(&panel->sink->mutex);
		ai_sink_unref(panel->sink);
	}
	{
		GObject *proto = g_weak_ref_get(&panel->proto_ref);

		if (proto) {
			ai_registry_unbind(GTK_WIDGET(proto));
			g_object_unref(proto);
		}
	}

	for (i = 0; i < panel->history->len; i++)
		g_free(g_ptr_array_index(panel->history, i));
	g_ptr_array_free(panel->history, TRUE);

	g_free(panel->api_url);
	g_free(panel->model);
	g_free(panel->api_key);
	g_free(panel->last_reply);
	g_free(panel->turn_tag);
	g_free(panel->turn_hdr_tag);
	g_weak_ref_clear(&panel->proto_ref);
	g_free(panel);
}

/* Screenshot images in tool results are megabytes of base64; once a newer
 * image exists, older ones only burn context. Replace their content arrays
 * with the text part plus a note. */
static gchar *ai_serialize_message(JsonObject *msg);

static void ai_history_strip_old_images(RemminaAiPanel *panel)
{
	guint i;

	for (i = 1; i + 1 < panel->history->len; i++) {
		gchar *m = g_ptr_array_index(panel->history, i);
		JsonParser *p;
		JsonNode *root;
		JsonObject *o;
		JsonArray *content, *kept;
		guint j;

		if (!strstr(m, "\"image_url\""))
			continue;
		p = json_parser_new();
		if (!json_parser_load_from_data(p, m, -1, NULL)) {
			g_object_unref(p);
			continue;
		}
		root = json_parser_get_root(p);
		if (json_node_get_node_type(root) != JSON_NODE_OBJECT) {
			g_object_unref(p);
			continue;
		}
		o = json_node_get_object(root);
		if (!json_object_has_member(o, "content") ||
		    json_node_get_node_type(json_object_get_member(o, "content"))
			!= JSON_NODE_ARRAY) {
			g_object_unref(p);
			continue;
		}
		content = json_object_get_array_member(o, "content");
		kept = json_array_new();
		for (j = 0; j < json_array_get_length(content); j++) {
			JsonNode *part = json_array_get_element(content, j);

			if (json_node_get_node_type(part) == JSON_NODE_OBJECT &&
			    json_object_has_member(json_node_get_object(part), "text"))
				json_array_add_element(kept, json_node_copy(part));
		}
		{
			JsonObject *note = json_object_new();

			json_object_set_string_member(note, "type", "text");
			json_object_set_string_member(note, "text",
				"[screenshot omitted: a newer image supersedes it]");
			json_array_add_object_element(kept, note);
		}
		json_object_set_array_member(o, "content", kept);
		{
			gchar *stripped = ai_serialize_message(o);

			g_free(m);
			g_ptr_array_index(panel->history, i) = stripped;
		}
		g_object_unref(p);
	}
}

/* Append a serialized chat message (JSON object text) to the history */
static void ai_history_append(RemminaAiPanel *panel, const gchar *message_json)
{
	g_ptr_array_add(panel->history, g_strdup(message_json));
	if (strstr(message_json, "\"image_url\""))
		ai_history_strip_old_images(panel);
}

static gchar *ai_serialize_message(JsonObject *msg)
{
	JsonGenerator *gen = json_generator_new();
	JsonNode *root = json_node_init_object(json_node_alloc(), msg);
	gchar *out;

	json_generator_set_root(gen, root);
	out = json_generator_to_data(gen, NULL);
	g_object_unref(gen);
	json_node_free(root);
	return out;
}

/* ------------------------------------------------------------------ */
/* Chat transcript view helpers                                        */
/* ------------------------------------------------------------------ */

static void ai_chat_ensure_tags(RemminaAiPanel *panel)
{
	GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(panel->chat_buf);

	if (gtk_text_tag_table_lookup(tt, "who") == NULL) {
		gtk_text_buffer_create_tag(panel->chat_buf, "who",
					   "weight", PANGO_WEIGHT_BOLD, NULL);
		gtk_text_buffer_create_tag(panel->chat_buf, "system",
					   "style", PANGO_STYLE_ITALIC,
					   "foreground", "#888a85", NULL);
		gtk_text_buffer_create_tag(panel->chat_buf, "error",
					   "foreground", "#c01c28", NULL);
		gtk_text_buffer_create_tag(panel->chat_buf, "tool",
					   "foreground", "#26a269", NULL);
		/* clickable "continue" link shown when the tool-round cap
		 * is hit (see ai_on_chat_view_click) */
		gtk_text_buffer_create_tag(panel->chat_buf, "contbtn",
					   "foreground", "#3584e4",
					   "underline", PANGO_UNDERLINE_SINGLE,
					   "left-margin", 14, NULL);
		/* marks the user's own messages so a box can be drawn
		 * around them (see ai_on_chat_draw); the margins keep the
		 * wrapped text inside the box's 5px inset */
		gtk_text_buffer_create_tag(panel->chat_buf, "userblock",
					   "left-margin", 13,
					   "right-margin", 13, NULL);
		/* per-message timestamps: grey, one flush left (AI
		 * replies, bottom-left), one flush right (the user's
		 * own messages, bottom-right inside their box) */
		gtk_text_buffer_create_tag(panel->chat_buf, "tsleft",
					   "foreground", "#888a85",
					   "left-margin", 14, NULL);
		gtk_text_buffer_create_tag(panel->chat_buf, "tsright",
					   "foreground", "#888a85",
					   "justification", GTK_JUSTIFY_RIGHT,
					   "right-margin", 13,
					   "pixels-above-lines", 5, NULL);
	}
}

/* "HH:MM" in the machine's local time; caller g_free()s. */
static gchar *ai_timestamp(void)
{
	GDateTime *now = g_date_time_new_now_local();
	gchar *s = g_strdup_printf("%02d:%02d",
			g_date_time_get_hour(now), g_date_time_get_minute(now));

	g_date_time_unref(now);
	return s;
}

static void ai_chat_append(RemminaAiPanel *panel, const gchar *who,
			   const gchar *tag, const gchar *text)
{
	GtkTextIter iter;
	gboolean is_user = g_strcmp0(tag, "userblock") == 0;

	ai_chat_ensure_tags(panel);
	gtk_text_buffer_get_end_iter(panel->chat_buf, &iter);
	if (who && *who) {
		if (is_user) {
			gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf,
					&iter, who, -1, "who", "userblock", NULL);
			gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf,
					&iter, "\n", -1, "userblock", NULL);
		} else {
			gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf,
					&iter, who, -1, "who", NULL);
			gtk_text_buffer_insert(panel->chat_buf, &iter, "\n", -1);
		}
	}
	gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf, &iter,
			text, -1, (tag && *tag) ? tag : NULL, NULL);
	/* the user's send time on its own line just BELOW the box, flush
	 * right: only tsright tags it (not userblock), so ai_on_chat_draw
	 * stops the box at the message text itself */
	if (is_user) {
		gchar *ts = ai_timestamp();

		gtk_text_buffer_insert(panel->chat_buf, &iter, "\n", -1);
		gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf,
				&iter, ts, -1, "tsright", NULL);
		g_free(ts);
	}
	gtk_text_buffer_insert(panel->chat_buf, &iter, "\n\n", -1);

	/* follow the bottom only while already following it; a tool or
	 * status line must not yank a user who is reading history */
	if (panel->stick_bottom)
		ai_scroll_bottom(panel);
}

/* True when the display line starting at `at` carries `tag` on its first
 * character. get_tags() returns {tags-before-pos, tags-after-pos}; after
 * advancing one char, set 0 holds the line's first character — checking
 * that exact character avoids tagging the blank separator lines. */
static gboolean ai_line_tagged(GtkTextIter *at, GtkTextTag *tag)
{
	GtkTextIter ch = *at;

	/* gtk_text_iter_get_tags() segfaults on Arch's gtk3 3.24.52 (verified
	 * with a standalone repro); gtk_text_iter_has_tag() works fine. */
	if (!tag || !gtk_text_iter_forward_char(&ch))
		return FALSE;
	return gtk_text_iter_has_tag(&ch, tag);
}

/* Rounded rectangle path (fixed radius) */
static void ai_cairo_rrect(cairo_t *cr, double x, double y,
			   double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
	cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
	cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
	cairo_close_path(cr);
}

/* Draw a subtle rounded box behind each of the user's own messages, a
 * shade lighter than the panel background so they read as distinct.
 * Runs of consecutive tagged lines (wrapped messages) get one box.
 * Nothing is drawn for AI/system/tool lines. */
static gboolean ai_on_chat_draw(GtkWidget *view, cairo_t *cr, gpointer data)
{
	RemminaAiPanel *panel = data;
	GtkTextView *tv = GTK_TEXT_VIEW(view);
	GtkTextBuffer *buf = panel->chat_buf;
	GtkTextTag *t_user;
	GdkRectangle vis;
	GdkWindow *textwin, *mainwin;
	GtkStyleContext *style;
	GdkRGBA base;
	gint ox = 0, oy = 0, wx = 0, tw;

	if (!buf || gtk_text_buffer_get_char_count(buf) == 0)
		return FALSE;
	textwin = gtk_text_view_get_window(tv, GTK_TEXT_WINDOW_TEXT);
	mainwin = gtk_widget_get_window(view);
	if (!textwin || !mainwin)
		return FALSE;

	t_user = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(buf),
					   "userblock");
	if (!t_user)
		return FALSE;

	gtk_text_view_get_visible_rect(tv, &vis);
	gdk_window_get_position(textwin, &ox, &oy);
	tw = gdk_window_get_width(textwin);

	/* fill = theme text color mixed slightly into the background:
	 * one lookup, so dark and light themes both get a "lighter" box */
	style = gtk_widget_get_style_context(panel->chat_view);
	gtk_style_context_get_color(style, GTK_STATE_FLAG_NORMAL, &base);

	cairo_save(cr);
	cairo_translate(cr, ox, oy);

	{
		gint line_no, nlines;

		nlines = gtk_text_buffer_get_line_count(buf);
		for (line_no = 0; line_no < nlines; line_no++) {
			GtkTextIter l;
			gint ly = 0, lh = 0, end, top_buf, bot_buf;
			gint top_w = 0, bot_w = 0;

			gtk_text_buffer_get_iter_at_line(buf, &l, line_no);
			if (!ai_line_tagged(&l, t_user))
				continue;
			gtk_text_view_get_line_yrange(tv, &l, &ly, &lh);
			if (lh <= 0)
				continue;

			/* extend over consecutive user lines (a wrapped
			 * message is one box, not one per display line) */
			top_buf = ly;
			bot_buf = ly + lh;
			end = line_no + 1;
			while (end < nlines) {
				GtkTextIter n;
				gint ny = 0, nh = 0;

				gtk_text_buffer_get_iter_at_line(buf, &n, end);
				if (!ai_line_tagged(&n, t_user))
					break;
				gtk_text_view_get_line_yrange(tv, &n, &ny, &nh);
				if (nh <= 0 || ny != bot_buf)
					break;
				bot_buf = ny + nh;
				end++;
			}

			gtk_text_view_buffer_to_window_coords(
				tv, GTK_TEXT_WINDOW_TEXT, 0, top_buf, &wx, &top_w);
			gtk_text_view_buffer_to_window_coords(
				tv, GTK_TEXT_WINDOW_TEXT, 0, bot_buf, &wx, &bot_w);
			if (bot_w > 0 && top_w < vis.height) {
				/* clearly distinct from the panel background:
				 * dark mode → noticeably lighter grey box;
				 * light mode → grey box on white */
				gboolean light = gtk_style_context_has_class(
					gtk_widget_get_style_context(panel->box),
					"remmina-ai-light");
				gdouble fill = light ? 0.14 : 0.24;
				gdouble edge = light ? 0.35 : 0.50;

				/* The font keeps its leading at the top of
				 * each line box, so a box that hugs the raw
				 * line geometry looks top-heavy. Inset the
				 * top edge into that leading. The bottom
				 * edge stops exactly at the last line's
				 * boundary: the timestamp line sits directly
				 * below the box and its glyphs would
				 * otherwise overlap a padded border. */
				ai_cairo_rrect(cr, 5, MAX(top_w, 0) + 2,
					       tw - 10,
					       MAX((bot_w - top_w) - 2, 4), 6);
				cairo_set_source_rgba(cr, base.red, base.green,
						      base.blue, fill);
				cairo_fill_preserve(cr);
				cairo_set_source_rgba(cr, base.red, base.green,
						      base.blue, edge);
				cairo_set_line_width(cr, 1.0);
				cairo_stroke(cr);
			}
			line_no = end - 1;
		}
	}
	cairo_restore(cr);
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Settings persistence (remmina.pref generic key/value store)         */
/* ------------------------------------------------------------------ */

static void ai_panel_load_settings(RemminaAiPanel *panel)
{
	gchar *v;

	v = remmina_pref_get_value("ai_api_url");
	panel->api_url = g_strdup((v && *v) ? v : AI_DEFAULT_URL);
	g_free(v);

	v = remmina_pref_get_value("ai_model");
	panel->model = (v && *v) ? v : NULL;

	v = remmina_pref_get_value("ai_api_key");
	panel->api_key = (v && *v) ? v : NULL;

	{
		gchar *t = remmina_pref_get_value("ai_tools_enabled");

		panel->tools_enabled = (t && g_strcmp0(t, "true") == 0);
		g_free(t);
	}
	{
		gchar *t = remmina_pref_get_value("ai_confirm_risky");

		/* on by default: only an explicit "false" disables it */
		panel->confirm_risky = (!t || g_strcmp0(t, "true") == 0);
		g_free(t);
	}
}

static void ai_panel_save_settings(RemminaAiPanel *panel)
{
	remmina_pref_set_value("ai_api_url", panel->api_url);
	remmina_pref_set_value("ai_model", panel->model ? panel->model : "");
	remmina_pref_set_value("ai_api_key", panel->api_key ? panel->api_key : "");
	remmina_pref_set_value("ai_tools_enabled",
			       panel->tools_enabled ? "true" : "false");
	remmina_pref_set_value("ai_confirm_risky",
			       panel->confirm_risky ? "true" : "false");
}

static void ai_on_tools_toggled(GtkToggleButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;

	panel->tools_enabled = gtk_toggle_button_get_active(btn);
	ai_panel_save_settings(panel);
}

/* Persist the actions-per-turn spinner to the ai_max_tool_rounds pref.
 * ai_max_tool_rounds() re-reads the pref each tool round, so the change
 * applies to the next turn without a restart. Shared across all panels
 * (one global pref). */
static void ai_on_rounds_changed(GtkSpinButton *spin, gpointer data)
{
	gchar buf[8];

	(void)data;
	g_snprintf(buf, sizeof(buf), "%d",
		   (gint)gtk_spin_button_get_value(spin));
	remmina_pref_set_value("ai_max_tool_rounds", buf);
}

static void ai_on_confirm_toggled(GtkToggleButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;

	panel->confirm_risky = gtk_toggle_button_get_active(btn);
	ai_panel_save_settings(panel);
}

/* ------------------------------------------------------------------ */
/* Remote context capture                                              */
/* ------------------------------------------------------------------ */

/* The protocol widget of MY session (per-tab), or NULL if it's gone or
 * no longer connected. */
/* Returns a BORROWED reference (not owned by the caller): every caller
 * uses the widget synchronously within its own function, and the panel's
 * container keeps the widget alive while it exists. Returning the fresh
 * reference g_weak_ref_get() produces leaked one strong ref per call —
 * enough to pin a whole connection's object graph after its window
 * closes once a few tool calls have run. */
static RemminaProtocolWidget *ai_active_gp(RemminaAiPanel *panel)
{
	GObject *gp = g_weak_ref_get(&panel->proto_ref);
	RemminaProtocolWidget *ret;

	if (!gp)
		return NULL;
	if (!panel->connected || !REMMINA_IS_PROTOCOL_WIDGET(gp)) {
		g_object_unref(gp);
		return NULL;
	}
	ret = REMMINA_PROTOCOL_WIDGET(gp);
	g_object_unref(gp);
	return ret;
}

/* Active connection info: protocol display name + server host.
 * Either out-parameter may be NULL. */
static void ai_active_info(RemminaAiPanel *panel, const gchar **protocol,
			   const gchar **server, const gchar **name)
{
	RemminaProtocolWidget *gp = ai_active_gp(panel);
	RemminaFile *f;

	if (protocol)
		*protocol = gp ? remmina_protocol_widget_get_name(gp) : NULL;
	if (server)
		*server = NULL;
	if (name)
		*name = NULL;
	if (gp && (f = remmina_protocol_widget_get_file(gp)) != NULL) {
		if (server)
			*server = remmina_file_get_string(f, "server");
		if (name)
			*name = remmina_file_get_string(f, "name");
	}
}

/* Encode a pixbuf as a base64 data URI, downscaling huge images so the
 * request stays within the model's vision budget. Returns NULL on failure. */
static gchar *ai_pixbuf_to_data_uri(GdkPixbuf *src)
{
	GdkPixbuf *scaled = NULL, *use = src;
	guint8 *buf = NULL;
	gsize bufsize = 0;
	gchar *b64, *uri = NULL;
	gint w, h, max;

	w = gdk_pixbuf_get_width(src);
	h = gdk_pixbuf_get_height(src);
	max = MAX(w, h);
	if (max > AI_MAX_IMG_DIM) {
		scaled = gdk_pixbuf_scale_simple(src,
				(gint)((gdouble)w * AI_MAX_IMG_DIM / max + 0.5),
				(gint)((gdouble)h * AI_MAX_IMG_DIM / max + 0.5),
				GDK_INTERP_BILINEAR);
		if (scaled)
			use = scaled;
	}

	if (!gdk_pixbuf_get_has_alpha(use)) {
		GdkPixbuf *rgba = gdk_pixbuf_add_alpha(use, FALSE, 0, 0, 0);
		if (scaled)
			g_object_unref(scaled);
		scaled = rgba;
		use = rgba;
	}

	if (gdk_pixbuf_save_to_buffer(use, (gchar **)&buf, &bufsize, "png", NULL, NULL)) {
		b64 = g_base64_encode(buf, bufsize);
		uri = g_strconcat("data:image/png;base64,", b64, NULL);
		g_free(b64);
		g_free(buf);
	}
	if (scaled)
		g_object_unref(scaled);
	return uri;
}
/* Grab the remote screen of gp.
 * Returns a GdkPixbuf (caller unrefs) or NULL. Tries, in order:
 *  1. the plugin's own framebuffer hook (RDP, VNC: true remote pixels)
 *  2. the GTK window of the protocol widget (last resort; X11 only) */
static GdkPixbuf *ai_capture_remote(RemminaProtocolWidget *gp, gchar **err)
{
	RemminaPluginScreenshotData rpsd;
	GdkPixbuf *pix = NULL;

	if (!GTK_IS_WIDGET(gp)) {
		if (err)
			*err = g_strdup(_("No active connection in this window"));
		return NULL;
	}

	if (remmina_protocol_widget_plugin_screenshot(gp, &rpsd)) {
		cairo_format_t fmt;
		cairo_surface_t *surf, *out;
		cairo_t *cr;
		gint stride;

		if (rpsd.bitsPerPixel == 32)
			fmt = CAIRO_FORMAT_ARGB32;
		else if (rpsd.bitsPerPixel == 24)
			fmt = CAIRO_FORMAT_RGB24;
		else
			fmt = CAIRO_FORMAT_RGB16_565;
		stride = cairo_format_stride_for_width(fmt, rpsd.width);
		surf = cairo_image_surface_create_for_data(rpsd.buffer, fmt,
				rpsd.width, rpsd.height, stride);
		out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, rpsd.width, rpsd.height);
		cr = cairo_create(out);
		cairo_set_source_surface(cr, surf, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		cairo_destroy(cr);
		pix = gdk_pixbuf_get_from_surface(out, 0, 0, rpsd.width, rpsd.height);
		cairo_surface_destroy(surf);
		cairo_surface_destroy(out);
		free(rpsd.buffer);
		return pix;
	}

	/* Fallback: grab the on-screen window of the protocol widget.
	 * Works under X11; on Wayland this usually fails. */
	{
		GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(gp));
		if (win) {
			pix = gdk_pixbuf_get_from_window(win, 0, 0,
				 gdk_window_get_width(win),
				 gdk_window_get_height(win));
		}
		if (!pix && err)
			*err = g_strdup(_("This connection type cannot provide a screenshot"));
	}
	return pix;
}

/* Grab the visible text of an SSH (VTE) terminal. Caller g_free()s. */
static gchar *ai_capture_terminal_text(RemminaProtocolWidget *gp)
{
	GtkWidget *vte = remmina_ssh_plugin_get_vte(gp);
	GOutputStream *stream;
	GString *out = NULL;
	GError *error = NULL;

	if (!vte || !VTE_IS_TERMINAL(vte))
		return NULL;

	stream = g_memory_output_stream_new(NULL, 0, g_realloc, NULL);
	if (vte_terminal_write_contents_sync(VTE_TERMINAL(vte), stream,
					    VTE_WRITE_DEFAULT, NULL, &error)) {
		gchar *data = (gchar *)g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(stream));
		gsize len = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(stream));
		if (data && len > 0) {
			out = g_string_new_len(data, len);
			/* strip the bulk of the scrollback, keep the tail */
			if (out->len > AI_MAX_TERM_CHARS)
				g_string_erase(out, 0, out->len - AI_MAX_TERM_CHARS);
		}
	} else {
		g_clear_error(&error);
	}
	g_object_unref(stream);
	return out ? g_string_free(out, FALSE) : NULL;
}

/* ------------------------------------------------------------------ */
/* OpenAI-compatible HTTP client (libcurl, worker thread)              */
/* ------------------------------------------------------------------ */

/* Consume one complete SSE line from the worker thread */
static void ai_sse_worker_line(AiHttpJob *job, const gchar *line, gsize len)
{
	JsonParser *p;
	JsonNode *root, *ch0, *delta;
	JsonObject *chunk;
	const gchar *payload;

	/* skip blanks and event:/id: fields */
	if (len == 0)
		return;
	if (g_str_has_prefix(line, "data:") == FALSE)
		return;
	payload = line + 5;
	while (*payload == ' ')
		payload++;
	if (g_strcmp0(payload, "[DONE]") == 0)
		return;

	p = json_parser_new();
	if (!json_parser_load_from_data(p, payload, -1, NULL)) {
		g_object_unref(p);
		return;
	}
	root = json_parser_get_root(p);
	if (!JSON_NODE_HOLDS_OBJECT(root))
		goto out;
	chunk = json_node_get_object(root);
	if (!json_object_has_member(chunk, "choices"))
		goto out;
	{
		JsonArray *choices = json_object_get_array_member(chunk, "choices");

		if (json_array_get_length(choices) == 0)
			goto out;
		ch0 = json_array_get_element(choices, 0);
	}
	if (!JSON_NODE_HOLDS_OBJECT(ch0))
		goto out;
	{
		JsonObject *c0 = json_node_get_object(ch0);

		if (json_object_has_member(c0, "finish_reason")) {
			const gchar *fr = json_object_get_string_member(c0, "finish_reason");

			if (fr && g_strcmp0(fr, "tool_calls") == 0)
				job->finish_tool_calls = TRUE;
		}
		if (!json_object_has_member(c0, "delta"))
			goto out;
		delta = json_object_get_member(c0, "delta");
	}
	if (!JSON_NODE_HOLDS_OBJECT(delta))
		goto out;
	{
		JsonObject *d = json_node_get_object(delta);

		/* streamed content text */
		if (json_object_has_member(d, "content")) {
			const gchar *piece = json_object_get_string_member(d, "content");

			if (piece && *piece) {
				g_string_append(job->content, piece);
				ai_sink_append(job->sink, piece, -1);
			}
		}
		/* streamed tool-call fragments, keyed by "index" */
		if (json_object_has_member(d, "tool_calls")) {
			JsonArray *tcs = json_object_get_array_member(d, "tool_calls");
			guint i;

			for (i = 0; i < json_array_get_length(tcs); i++) {
				JsonNode *tcn = json_array_get_element(tcs, i);
				JsonObject *tco;
				AiToolCall *tc;
				guint idx = i;

				if (!JSON_NODE_HOLDS_OBJECT(tcn))
					continue;
				tco = json_node_get_object(tcn);
				if (json_object_has_member(tco, "index"))
					idx = json_object_get_int_member(tco, "index");
				while (job->tool_calls->len <= idx)
					g_ptr_array_add(job->tool_calls, ai_tool_call_new());
				tc = g_ptr_array_index(job->tool_calls, idx);

				if (json_object_has_member(tco, "id")) {
					const gchar *id = json_object_get_string_member(tco, "id");

					if (id && *id) {
						g_free(tc->id);
						tc->id = g_strdup(id);
					}
				}
				if (json_object_has_member(tco, "function")) {
					JsonObject *fn = json_object_get_object_member(tco, "function");

					if (json_object_has_member(fn, "name")) {
						const gchar *nm = json_object_get_string_member(fn, "name");

						if (nm && *nm) {
							/* name may also arrive fragmented */
							if (!tc->name)
								tc->name = g_strdup(nm);
							else if (g_strcmp0(tc->name, nm) != 0
								 && g_str_has_prefix(nm, tc->name)) {
								g_free(tc->name);
								tc->name = g_strdup(nm);
							}
						}
					}
					if (json_object_has_member(fn, "arguments"))
						g_string_append(tc->args,
							json_object_get_string_member(fn, "arguments"));
				}
			}
		}
	}
out:
	g_object_unref(p);
}

static size_t ai_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	AiHttpJob *job = (AiHttpJob *)userdata;
	gsize total = size * nmemb;

	/* user pressed Stop: return 0 to abort the transfer */
	if (job->cancelled)
		return 0;

	/* guard against runaway responses (~32 MB) */
	if (job->response->len + total > 32u << 20)
		return 0;

	if (!job->is_stream) {
		g_string_append_len(job->response, ptr, total);
		return total;
	}

	/* Server-Sent Events: buffer partial lines, consume complete ones */
	for (gsize i = 0; i < total; i++) {
		if (ptr[i] == '\n') {
			if (job->sse_line->len > 0 && job->sse_line->str[job->sse_line->len - 1] == '\r')
				g_string_truncate(job->sse_line, job->sse_line->len - 1);
			ai_sse_worker_line(job, job->sse_line->str, job->sse_line->len);
			g_string_set_size(job->sse_line, 0);
		} else {
			g_string_append_c(job->sse_line, ptr[i]);
		}
	}
	return total;
}

static gboolean remmina_ai_panel_http_done(gpointer userdata);

/* Fired on a timer while the transfer is open even when no bytes are
 * flowing, so Stop is responsive while the model is "thinking". Return
 * non-zero to abort the transfer. */
static int
ai_curl_progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t ultotal, curl_off_t ulnow)
{
	AiHttpJob *job = (AiHttpJob *)userdata;

	(void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
	return job->cancelled ? 1 : 0;
}

static gpointer ai_http_thread(gpointer userdata)
{
	AiHttpJob *job = (AiHttpJob *)userdata;
	struct curl_slist *hdrs = NULL;
	char auth[512];

	job->easy = curl_easy_init();
	if (!job->easy) {
		g_strlcpy(job->errbuf, "curl_easy_init failed", sizeof(job->errbuf));
		g_idle_add(remmina_ai_panel_http_done, job);
		return NULL;
	}
	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	if (job->api_key && *job->api_key) {
		g_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", job->api_key);
		hdrs = curl_slist_append(hdrs, auth);
	}

	curl_easy_setopt(job->easy, CURLOPT_URL, job->url);
	if (!job->is_get) {
		curl_easy_setopt(job->easy, CURLOPT_POST, 1L);
		curl_easy_setopt(job->easy, CURLOPT_POSTFIELDS, job->payload);
	}
	curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(job->easy, CURLOPT_WRITEFUNCTION, ai_curl_write_cb);
	curl_easy_setopt(job->easy, CURLOPT_WRITEDATA, job);
	curl_easy_setopt(job->easy, CURLOPT_ERRORBUFFER, job->errbuf);
	curl_easy_setopt(job->easy, CURLOPT_TIMEOUT, 300L);
	curl_easy_setopt(job->easy, CURLOPT_CONNECTTIMEOUT, 10L);
	/* progress callback so Stop can cancel even with no data flowing */
	curl_easy_setopt(job->easy, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(job->easy, CURLOPT_XFERINFOFUNCTION, ai_curl_progress_cb);
	curl_easy_setopt(job->easy, CURLOPT_XFERINFODATA, job);

	job->errbuf[0] = '\0';
	{
		CURLcode res = curl_easy_perform(job->easy);
		if (res != CURLE_OK && job->errbuf[0] == '\0')
			g_strlcpy(job->errbuf, curl_easy_strerror(res),
				  sizeof(job->errbuf));
	}

	curl_slist_free_all(hdrs);
	curl_easy_cleanup(job->easy);
	job->easy = NULL;

	g_idle_add(remmina_ai_panel_http_done, job);
	return NULL;
}

static AiHttpJob *ai_job_new(RemminaAiPanel *panel, AiJobKind kind,
			     const gchar *endpoint, const gchar *payload,
			     gboolean stream)
{
	AiHttpJob *job = g_new0(AiHttpJob, 1);

	job->panel = panel;
	/* weak ref on the panel's top widget: resolves to NULL after destroy */
	g_weak_ref_init(&job->weak_ref, panel->box);
	job->kind = kind;
	job->url = g_strdup(endpoint);
	job->api_key = g_strdup(panel->api_key ? panel->api_key : "");
	job->payload = g_strdup(payload);
	job->response = g_string_new(NULL);
	job->errbuf[0] = '\0';
	job->is_stream = stream;
	if (stream) {
		job->sse_line = g_string_new(NULL);
		job->content = g_string_new(NULL);
		job->tool_calls = g_ptr_array_new_with_free_func(ai_tool_call_free);
		/* the sink is shared with the panel; create it on first use */
		if (!panel->sink)
			panel->sink = ai_sink_new();
		job->sink = ai_sink_ref(panel->sink);
	}
	return job;
}

static void ai_job_free(AiHttpJob *job)
{
	g_weak_ref_clear(&job->weak_ref);
	g_free(job->url);
	g_free(job->api_key);
	g_free(job->payload);
	g_string_free(job->response, TRUE);
	if (job->sse_line)
		g_string_free(job->sse_line, TRUE);
	if (job->content)
		g_string_free(job->content, TRUE);
	if (job->tool_calls)
		g_ptr_array_free(job->tool_calls, TRUE);
	if (job->sink)
		ai_sink_unref(job->sink);
	g_free(job);
}

static void ai_launch(AiHttpJob *job)
{
	GError *error = NULL;
	GThread *th = g_thread_try_new("remmina-ai", ai_http_thread, job, &error);

	if (!th) {
		GtkWidget *alive = g_weak_ref_get(&job->weak_ref);
		if (alive) {
			RemminaAiPanel *panel = g_object_get_data(G_OBJECT(alive), "ai-panel");
			ai_chat_append(panel, NULL, "error",
				       _("Failed to start AI request thread"));
			g_object_unref(alive);
		}
		ai_job_free(job);
	}
}

/* ------------------------------------------------------------------ */
/* Live streaming display ("thinking box")                             */
/* ------------------------------------------------------------------ */

static void ai_marks_clear(RemminaAiPanel *panel)
{
	gtk_text_buffer_delete_mark_by_name(panel->chat_buf, "ph_beg");
	gtk_text_buffer_delete_mark_by_name(panel->chat_buf, "ph_end");
}

static void ai_stream_begin(RemminaAiPanel *panel);
static gboolean ai_drain_stream(gpointer data);
static void ai_stream_end(RemminaAiPanel *panel);

/* Short pretty name for the current model: strips the "owner/" prefix
 * and quantization/serving suffixes, caps at 24 chars. Caller g_free()s. */
static gchar *ai_model_display_name(RemminaAiPanel *panel)
{
	static const gchar *tails[] = {
		"-MLX-Serve", "_MLX_Serve", "-mlx-serve",
		"-MLX", "-mlx", "-Serve", "-serve", NULL };
	const gchar *m;
	gchar *cut;
	guint t;

	if (!panel->model || !*panel->model)
		return g_strdup(_("AI"));

	m = strrchr(panel->model, '/');
	m = m ? m + 1 : panel->model;
	cut = g_strdup(m);
	for (t = 0; tails[t]; t++) {
		gchar *pos = strstr(cut, tails[t]);

		if (pos)
			*pos = '\0';
	}
	/* drop a trailing quantization tag like "-4bit" / "_8bit" */
	{
		gchar *q = strstr(cut, "-4bit");

		if (!q)
			q = strstr(cut, "-8bit");
		if (q)
			*q = '\0';
	}
	/* strip any separators left by the cuts above */
	{
		gsize n = strlen(cut);

		while (n > 0 && (cut[n - 1] == '-' || cut[n - 1] == '_'))
			cut[--n] = '\0';
	}
	if (!*cut) {
		g_free(cut);
		cut = g_strdup(m);   /* everything was suffix: use raw name */
	}
	if (g_utf8_strlen(cut, -1) > 24) {
		gchar *short_ = g_utf8_substring(cut, 0, 23);
		gchar *out = g_strconcat(short_, "…", NULL);

		g_free(short_);
		g_free(cut);
		return out;
	}
	return cut;
}

/* Start a live assistant message: a collapsible block delimited by a
 * header line ("▸ <model>") and a rule at the end, with the reply's
 * timestamp at the bottom-left. The body carries a per-turn tag so it
 * can be hidden independently. */
static void ai_stream_begin(RemminaAiPanel *panel)
{
	GtkTextIter iter;
	GtkTextTagTable *tt;
	gchar *tagname;
	gchar hdr[64];

	ai_chat_ensure_tags(panel);
	tt = gtk_text_buffer_get_tag_table(panel->chat_buf);
	if (!gtk_text_tag_table_lookup(tt, "thinking"))
		gtk_text_buffer_create_tag(panel->chat_buf, "thinking",
					   "style", PANGO_STYLE_ITALIC,
					   "foreground", "#888a85",
					   "left-margin", 14, NULL);
	if (!gtk_text_tag_table_lookup(tt, "rule"))
		gtk_text_buffer_create_tag(panel->chat_buf, "rule",
					   "foreground", "#5e5c64",
					   "left-margin", 14, NULL);
	if (!gtk_text_tag_table_lookup(tt, "hdr"))
		gtk_text_buffer_create_tag(panel->chat_buf, "hdr",
					   "weight", PANGO_WEIGHT_BOLD,
					   "foreground", "#e5a50a",
					   "left-margin", 14, NULL);

	panel->stick_bottom = TRUE;
	panel->drained = 0;
	panel->placeholder = TRUE;
	if (!panel->sink)
		panel->sink = ai_sink_new();

	/* fresh sink state for this turn */
	g_mutex_lock(&panel->sink->mutex);
	g_string_set_size(panel->sink->pending, 0);
	panel->sink->alive = TRUE;
	g_mutex_unlock(&panel->sink->mutex);

	/* per-turn tags */
	panel->turn_seq++;
	tagname = g_strdup_printf("turn%d", panel->turn_seq);
	if (!gtk_text_tag_table_lookup(tt, tagname))
		gtk_text_buffer_create_tag(panel->chat_buf, tagname,
					   "invisible", FALSE, NULL);
	{
		gchar *hname = g_strdup_printf("h%d", panel->turn_seq);

		if (!gtk_text_tag_table_lookup(tt, hname))
			gtk_text_buffer_create_tag(panel->chat_buf, hname, NULL);
		panel->turn_hdr_tag = hname;
	}
	g_free(panel->turn_tag);
	panel->turn_tag = tagname;

	/* header line (NOT tagged with the turn tag, so it stays visible) */
	{
		gchar *mname = ai_model_display_name(panel);

		g_snprintf(hdr, sizeof(hdr), "▸ %s\n", mname);
		g_free(mname);
	}
	gtk_text_buffer_get_end_iter(panel->chat_buf, &iter);
	gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf, &iter,
			hdr, -1, "hdr", panel->turn_hdr_tag, NULL);

	/* body placeholder between two marks, tagged for collapsing */
	gtk_text_buffer_get_end_iter(panel->chat_buf, &iter);
	gtk_text_buffer_create_mark(panel->chat_buf, "ph_beg", &iter, TRUE);
	gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf, &iter,
			_("Thinking…"), -1, "thinking", panel->turn_tag, NULL);
	gtk_text_buffer_create_mark(panel->chat_buf, "ph_end", &iter, FALSE);

	if (panel->drain_timer == 0)
		panel->drain_timer = g_timeout_add(120, ai_drain_stream, panel);

	/* a turn is running: offer Stop */
	gtk_widget_set_sensitive(panel->stop_btn, TRUE);
	gtk_widget_show(panel->stop_btn);
}

/* Move any text the worker produced into the buffer; runs on a timer */
static gboolean ai_drain_stream(gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	GtkTextIter iter;
	gchar *chunk = NULL;
	gsize len = 0;

	g_mutex_lock(&panel->sink->mutex);
	if (panel->sink->pending->len > 0) {
		len = panel->sink->pending->len;
		chunk = g_string_free(panel->sink->pending, FALSE);
		panel->sink->pending = g_string_new(NULL);
	}
	g_mutex_unlock(&panel->sink->mutex);

	if (chunk) {
		GtkTextMark *mbeg, *mend;

		if (panel->placeholder) {
			/* first token: wipe the "Thinking…" placeholder */
			GtkTextIter b, e;

			mbeg = gtk_text_buffer_get_mark(panel->chat_buf, "ph_beg");
			mend = gtk_text_buffer_get_mark(panel->chat_buf, "ph_end");
			gtk_text_buffer_get_iter_at_mark(panel->chat_buf, &b, mbeg);
			gtk_text_buffer_get_iter_at_mark(panel->chat_buf, &e, mend);
			gtk_text_buffer_delete(panel->chat_buf, &b, &e);
			panel->placeholder = FALSE;
		}
		mend = gtk_text_buffer_get_mark(panel->chat_buf, "ph_end");
		gtk_text_buffer_get_iter_at_mark(panel->chat_buf, &iter, mend);
		gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf, &iter,
				chunk, len, panel->turn_tag, NULL);
		panel->drained += (gint)len;
		g_free(chunk);

		if (panel->stick_bottom)
			ai_scroll_bottom(panel);
	}
	return G_SOURCE_CONTINUE;
}

/* End the live message: drop a leftover placeholder, close it out */
static void ai_stream_end(RemminaAiPanel *panel)
{
	if (panel->drain_timer) {
		g_source_remove(panel->drain_timer);
		panel->drain_timer = 0;
	}
	/* final drain of anything still in the sink */
	ai_drain_stream(panel);
	if (panel->placeholder) {
		GtkTextIter b, e;

		gtk_text_buffer_get_iter_at_mark(panel->chat_buf, &b,
			gtk_text_buffer_get_mark(panel->chat_buf, "ph_beg"));
		gtk_text_buffer_get_iter_at_mark(panel->chat_buf, &e,
			gtk_text_buffer_get_mark(panel->chat_buf, "ph_end"));
		gtk_text_buffer_delete(panel->chat_buf, &b, &e);
		panel->placeholder = FALSE;
	}
	ai_marks_clear(panel);
	{
		GtkTextIter iter;

		gtk_text_buffer_get_end_iter(panel->chat_buf, &iter);
		{
			GtkTextIter prev = iter;

			/* streamed text may not end in a newline */
			if (gtk_text_iter_backward_char(&prev) &&
			    gtk_text_iter_get_char(&prev) != '\n')
				gtk_text_buffer_insert(panel->chat_buf,
						&iter, "\n", -1);
		}
		/* closing rule, tagged so it collapses with the body */
		gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf, &iter,
				"──────────────────────────\n", -1,
				"rule", panel->turn_tag, NULL);
		/* the reply's time below the rule, flush left, tagged with
		 * the turn tag so it collapses with the body */
		{
			gchar *ts = ai_timestamp();

			gtk_text_buffer_insert_with_tags_by_name(panel->chat_buf,
					&iter, ts, -1, "tsleft",
					panel->turn_tag, NULL);
			gtk_text_buffer_insert(panel->chat_buf, &iter, "\n", -1);
			g_free(ts);
		}
		/* one untagged blank line separates blocks */
		gtk_text_buffer_insert(panel->chat_buf, &iter, "\n", -1);
		if (panel->stick_bottom)
			ai_scroll_bottom(panel);
	}
	if (panel->sink) {
		ai_sink_unref(panel->sink);
		panel->sink = NULL;
	}
}

/* Toggle the collapsed state of the turn whose header line contains it. */
static void ai_toggle_turn(RemminaAiPanel *panel, gint turn_no, gboolean collapse)
{
	GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(panel->chat_buf);
	gchar *tagname = g_strdup_printf("turn%d", turn_no);
	GtkTextTag *tag = gtk_text_tag_table_lookup(tt, tagname);

	g_free(tagname);
	if (!tag)
		return;
	g_object_set(tag, "invisible", collapse, NULL);

	/* swap the ▸ / ▾ glyph at the start of the header line */
	{
		gchar *hname = g_strdup_printf("h%d", turn_no);
		GtkTextTag *htag = gtk_text_tag_table_lookup(tt, hname);
		GtkTextTag *hdr = gtk_text_tag_table_lookup(tt, "hdr");
		GtkTextIter iter;
		gboolean found = FALSE;

		g_free(hname);
		if (!htag)
			return;

		/* linearly scan for the first char carrying htag (its header) */
		gtk_text_buffer_get_start_iter(panel->chat_buf, &iter);
		{
			do {
				if (gtk_text_iter_has_tag(&iter, htag)) {
					found = TRUE;
					break;
				}
			} while (gtk_text_iter_forward_char(&iter));
		}
		if (found) {
			GtkTextIter e = iter;

			gtk_text_iter_forward_char(&e);
			gtk_text_buffer_delete(panel->chat_buf, &iter, &e);
			/* re-insert the glyph with BOTH hdr and htag so it
			 * stays the scan anchor on the next toggle (hdr
			 * alone made the following toggle delete a body char
			 * and produce a double ▾▸ glyph) */
			gtk_text_buffer_insert_with_tags(panel->chat_buf,
					&iter, collapse ? "▸" : "▾", -1,
					hdr, htag, NULL);
		}
	}
}

/* Click handler: clicking a header line collapses/expands that turn */
static gboolean ai_on_chat_view_click(GtkWidget *view, GdkEventButton *ev,
				      gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	GtkTextIter iter, line_start, line_end;
	gint bx, by;
	GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(panel->chat_buf);
	GSList *tl, *l;
	const gchar *gname;
	gint turn_no = -1;
	gboolean collapses;

	if (ev->button != 1)
		return FALSE;

	gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view),
			GTK_TEXT_WINDOW_TEXT, (gint)ev->x, (gint)ev->y, &bx, &by);
	gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(view), &iter, bx, by);

	line_start = iter;
	gtk_text_iter_set_line_offset(&line_start, 0);
	line_end = line_start;
	if (!gtk_text_iter_forward_line(&line_end))
		gtk_text_buffer_get_end_iter(panel->chat_buf, &line_end);

	/* clicking the "» Continue" link resumes the tool loop for one
	 * more cap-sized budget; a retired (or superseded) link is still
	 * consumed so the click never lands on a turn header */
	{
		GtkTextTag *cb = gtk_text_tag_table_lookup(tt, "contbtn");

		if (cb && ai_line_tagged(&line_start, cb)) {
			if (panel->continue_pending && !panel->busy)
				ai_continue_after_limit(panel);
			return TRUE;
		}
	}

	/* find an "hN" header marker tag on this line */
	tl = gtk_text_iter_get_tags(&line_start);
	for (l = tl; l; l = l->next) {
		g_object_get(l->data, "name", &gname, NULL);
		if (gname && gname[0] == 'h' && g_ascii_isdigit(gname[1])) {
			turn_no = atoi(gname + 1);
			break;
		}
	}
	g_slist_free(tl);
	if (turn_no < 0)
		return FALSE;

	{
		gchar *tn = g_strdup_printf("turn%d", turn_no);
		GtkTextTag *tag = gtk_text_tag_table_lookup(tt, tn);
		gboolean invis = FALSE;

		g_free(tn);
		if (!tag)
			return FALSE;
		g_object_get(tag, "invisible", &invis, NULL);
		collapses = !invis;
	}
	ai_toggle_turn(panel, turn_no, collapses);
	return TRUE; /* header click handled, don't move the cursor */
}

/* Scroll the transcript to the very bottom by setting the vertical
 * adjustment directly to (upper - page_size).
 *
 * This deliberately avoids gtk_text_view_scroll_to_iter(): when the
 * text view's line layout is not fully validated yet, that call
 * defers its own scroll to an internal idle and lands short — one of
 * the "sometimes doesn't scroll" races. Setting the adjustment is
 * immediate and exact; the trailing 350ms recheck (below) catches
 * layout that grows after we scroll.
 *
 * The scroll is deferred ~40ms by coalesce: GtkTextView rebuilds its
 * line layout lazily after inserts, so scrolling in the same callback
 * that inserted the text measures the OLD layout. A short timeout
 * lets GTK's validate pass finish first, and coalescing keeps one
 * scroll per burst during fast streaming. It is a timeout, not an
 * idle: g_idle_add sources starve under event traffic (mouse motion,
 * exposes) and the scroll would silently never run. */
static void ai_scroll_now(RemminaAiPanel *panel)
{
	GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(panel->scrolled));

	if (!adj)
		return;

	panel->auto_scrolling = TRUE;
	gtk_adjustment_set_value(adj,
			gtk_adjustment_get_upper(adj)
			- gtk_adjustment_get_page_size(adj));
	panel->auto_scrolling = FALSE;

	panel->adj_upper = gtk_adjustment_get_upper(adj);
	panel->adj_value = gtk_adjustment_get_value(adj);

	/* layout may still grow (wrapped lines settle late); swallow
	 * GTK's own downward catch-up moves briefly and re-scroll once
	 * the dust settles so we finish flush with the bottom */
	panel->scroll_deadline = g_get_monotonic_time() + G_TIME_SPAN_MILLISECOND * 600;
	if (!panel->scroll_recheck) {
		panel->scroll_recheck = TRUE;
		panel->recheck_timer = g_timeout_add(350, ai_scroll_recheck,
						     panel);
	}
}

static gboolean ai_scroll_bottom_idle(gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;

	panel->scroll_idle = 0;
	if (panel->stick_bottom)
		ai_scroll_now(panel);
	return G_SOURCE_REMOVE;
}

/* One-shot settle re-scroll (see ai_scroll_now) */
static gboolean ai_scroll_recheck(gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;

	panel->recheck_timer = 0;
	panel->scroll_recheck = FALSE;
	if (panel->stick_bottom)
		ai_scroll_now(panel);
	return G_SOURCE_REMOVE;
}

static void ai_scroll_bottom(RemminaAiPanel *panel)
{
	if (panel->scroll_idle == 0)
		panel->scroll_idle = g_timeout_add(40, ai_scroll_bottom_idle,
						   panel);
}

/* Stop following the bottom when the user scrolls up to read history */
static void ai_on_scroll_changed(GtkAdjustment *adj, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	gdouble upper = gtk_adjustment_get_upper(adj);
	gdouble page = gtk_adjustment_get_page_size(adj);
	gdouble value = gtk_adjustment_get_value(adj);
	gboolean moved_down = value > panel->adj_value + 0.5;

	panel->adj_value = value;

	/* our own synchronous ai_scroll_bottom() must not look like a
	 * user scroll */
	if (panel->auto_scrolling)
		return;

	/* GtkTextView chains a catch-up scroll after ours while its
	 * layout settles; that move is downward and arrives within a few
	 * hundred ms. Never user intent: a user leaving the bottom
	 * always moves UP. */
	if (moved_down && g_get_monotonic_time() < panel->scroll_deadline)
		return;

	/* When new text is inserted, the range grows and GTK fires this
	 * signal with the OLD value before the auto-scroll can run. That
	 * is not a user scroll: without this guard the stale "above the
	 * bottom" reading silently turns auto-scroll off. (This must not
	 * depend on adj_upper being previously set, or the very first
	 * overflow past the page break — when adj_upper is still 0 —
	 * reads as "user left the bottom" and follow never engages.) */
	if (!moved_down && upper > panel->adj_upper + 0.5
	    && panel->stick_bottom) {
		panel->adj_upper = upper;
		return;
	}
	panel->adj_upper = upper;

	panel->stick_bottom = (value >= upper - page - 4.0);
}

/* ------------------------------------------------------------------ */
/* Remote-control tools (click / type / keys / scroll)                 */
/* ------------------------------------------------------------------ */

/* Default cap on tool-loop rounds per user turn (each round is one
 * assistant message that may batch several tool calls). Override at
 * runtime by putting ai_max_tool_rounds=<n> in remmina.pref; it is
 * re-read every round, so an edit applies to the turn in progress. */
#define AI_MAX_TOOL_ROUNDS 8

static gint ai_max_tool_rounds(void)
{
	gchar *v = remmina_pref_get_value("ai_max_tool_rounds");
	gint n = (v && *v) ? atoi(v) : 0;

	g_free(v);
	if (n < 1)
		n = AI_MAX_TOOL_ROUNDS;
	else if (n > 100)
		n = 100;
	return n;
}

/* The widget that receives user input for this connection:
 * the VTE terminal (SSH) or the protocol's drawing area (RDP/VNC/GVNC). */
static GtkWidget *ai_find_input_widget(RemminaProtocolWidget *gp)
{
	GtkWidget *w, *stack[32];
	guint sp = 0;

	w = remmina_ssh_plugin_get_vte(gp);
	if (w)
		return w;

	/* depth-first search for the first GtkDrawingArea descendant */
	stack[sp++] = GTK_WIDGET(gp);
	while (sp > 0) {
		GtkWidget *cur = stack[--sp];
		GList *kids, *l;

		if (GTK_IS_DRAWING_AREA(cur) && gtk_widget_get_realized(cur))
			return cur;
		kids = gtk_container_get_children(GTK_CONTAINER(cur));
		for (l = kids; l; l = l->next) {
			if (sp < G_N_ELEMENTS(stack))
				stack[sp++] = GTK_WIDGET(l->data);
		}
		g_list_free(kids);
	}
	return NULL;
}

/* Map a point from screenshot-image space into the input widget's local
 * coordinates. RDP/VNC translate widget coords -> remote pixels themselves:
 * 1:1 when unscaled, proportional only in "scaled" mode. Our screenshot is
 * always the full remote framebuffer, so unscaled views must map to remote
 * pixel coordinates (which may exceed the on-screen allocation). */
static void ai_map_point(RemminaAiPanel *panel, GtkWidget *w,
			 gint ix, gint iy, gdouble *wx, gdouble *wy)
{
	gint aw = MAX(1, gtk_widget_get_allocated_width(w));
	gint ah = MAX(1, gtk_widget_get_allocated_height(w));
	RemminaProtocolWidget *gp = ai_active_gp(panel);
	RemminaScaleMode scale = gp ?
		remmina_protocol_widget_get_current_scale_mode(gp)
		: REMMINA_PROTOCOL_WIDGET_SCALE_MODE_NONE;
	gdouble fx, fy;

	if (panel->img_w > 0 && panel->img_h > 0) {
		fx = (gdouble)ix / panel->img_w;
		fy = (gdouble)iy / panel->img_h;
	} else {
		fx = (gdouble)ix / aw;   /* assume 1:1 pixel space */
		fy = (gdouble)iy / ah;
	}
	fx = CLAMP(fx, 0.0, 1.0);
	fy = CLAMP(fy, 0.0, 1.0);

	if (scale == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_SCALED) {
		/* widget scales proportionally: fraction of the allocation */
		*wx = fx * (aw - 1);
		*wy = fy * (ah - 1);
	} else {
		/* 1:1 widget space: fraction of the full remote framebuffer */
		*wx = fx * MAX(1, panel->remote_w > 0 ? panel->remote_w : aw);
		*wy = fy * MAX(1, panel->remote_h > 0 ? panel->remote_h : ah);
	}
}

static GdkDevice *ai_pointer(void)
{
	return gdk_seat_get_pointer(gdk_display_get_default_seat(
			   gdk_display_get_default()));
}

static void ai_emit_motion(GtkWidget *w, gdouble x, gdouble y)
{
	GdkEventMotion ev;
	gboolean ret;

	if (!gtk_widget_get_window(w))
		return;
	memset(&ev, 0, sizeof(ev));
	ev.type = GDK_MOTION_NOTIFY;
	ev.window = gtk_widget_get_window(w);
	ev.send_event = TRUE;
	ev.time = GDK_CURRENT_TIME;
	ev.x = x; ev.y = y;
	ev.x_root = x; ev.y_root = y;
	ev.state = 0;
	ev.is_hint = 0;
	ev.device = ai_pointer();
	g_signal_emit_by_name(w, "motion-notify-event", &ev, &ret);
}

static void ai_emit_button(GtkWidget *w, guint button, gdouble x, gdouble y,
			   gboolean press)
{
	GdkEventButton ev;
	gboolean ret;

	if (!gtk_widget_get_window(w))
		return;
	memset(&ev, 0, sizeof(ev));
	ev.type = press ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE;
	ev.window = gtk_widget_get_window(w);
	ev.send_event = TRUE;
	ev.time = GDK_CURRENT_TIME;
	ev.x = x; ev.y = y;
	ev.x_root = x; ev.y_root = y;
	ev.state = 0;
	ev.button = button;
	ev.device = ai_pointer();
	g_signal_emit_by_name(w, press ? "button-press-event"
			 : "button-release-event", &ev, &ret);
}

static void ai_emit_scroll(GtkWidget *w, gdouble x, gdouble y, gboolean up)
{
	GdkEventScroll ev;
	gboolean ret;

	if (!gtk_widget_get_window(w))
		return;
	memset(&ev, 0, sizeof(ev));
	ev.type = GDK_SCROLL;
	ev.window = gtk_widget_get_window(w);
	ev.send_event = TRUE;
	ev.time = GDK_CURRENT_TIME;
	ev.x = x; ev.y = y;
	ev.x_root = x; ev.y_root = y;
	ev.state = 0;
	ev.direction = up ? GDK_SCROLL_UP : GDK_SCROLL_DOWN;
	ev.device = ai_pointer();
	g_signal_emit_by_name(w, "scroll-event", &ev, &ret);
}

/* Type text through the plugin's own keystroke path so scaling and
 * keyboard mapping are handled by the protocol plugin itself */
static gchar *ai_type_text(RemminaAiPanel *panel, const gchar *text,
			   gboolean press_enter)
{
	RemminaProtocolWidget *gp = ai_active_gp(panel);
	GtkWidget *vte;

	if (!gp)
		return g_strdup(_("no active connection"));

	vte = remmina_ssh_plugin_get_vte(gp);
	if (vte) {
		vte_terminal_feed_child(VTE_TERMINAL(vte), text, strlen(text));
		if (press_enter)
			vte_terminal_feed_child(VTE_TERMINAL(vte), "\r", 1);
		return g_strdup_printf("typed %d characters into the SSH session",
				       (int)strlen(text));
	}

	if (!remmina_protocol_widget_plugin_receives_keystrokes(gp))
		return g_strdup(_("this connection type cannot receive keystrokes"));

	/* remmina_protocol_widget_send_keystrokes() consumes a "keystrokes"
	 * string off a menu item, converting literal "\n" / "\t" sequences
	 * into Return / Tab (and freeing the string on the way out) */
	{
		GString *esc = g_string_new(NULL);
		GtkWidget *mi;
		const gchar *it;

		for (it = text; *it; it++) {
			switch (*it) {
			case '\n': g_string_append(esc, "\\n"); break;
			case '\t': g_string_append(esc, "\\t"); break;
			default:   g_string_append_c(esc, *it); break;
			}
		}
		if (press_enter)
			g_string_append(esc, "\\n");

		mi = gtk_menu_item_new();
		g_object_set_data(G_OBJECT(mi), "keystrokes", esc->str);
		g_string_free(esc, FALSE);  /* ownership moved to the menu item */
		remmina_protocol_widget_send_keystrokes(gp, GTK_MENU_ITEM(mi));
		gtk_widget_destroy(mi);
	}
	return g_strdup_printf("typed %d characters into the session",
			       (int)strlen(text));
}

static guint ai_keyname_to_keyval(const gchar *name)
{
	if (g_ascii_strcasecmp(name, "ctrl") == 0 || g_ascii_strcasecmp(name, "control") == 0)
		return GDK_KEY_Control_L;
	if (g_ascii_strcasecmp(name, "shift") == 0)
		return GDK_KEY_Shift_L;
	if (g_ascii_strcasecmp(name, "alt") == 0)
		return GDK_KEY_Alt_L;
	if (g_ascii_strcasecmp(name, "super") == 0 || g_ascii_strcasecmp(name, "win") == 0
			|| g_ascii_strcasecmp(name, "cmd") == 0 || g_ascii_strcasecmp(name, "meta") == 0)
		return GDK_KEY_Super_L;
	if (g_ascii_strcasecmp(name, "enter") == 0 || g_ascii_strcasecmp(name, "return") == 0)
		return GDK_KEY_Return;
	if (g_ascii_strcasecmp(name, "esc") == 0 || g_ascii_strcasecmp(name, "escape") == 0)
		return GDK_KEY_Escape;
	if (g_ascii_strcasecmp(name, "del") == 0 || g_ascii_strcasecmp(name, "delete") == 0)
		return GDK_KEY_Delete;
	if (g_ascii_strcasecmp(name, "backspace") == 0)
		return GDK_KEY_BackSpace;
	if (g_ascii_strcasecmp(name, "tab") == 0)
		return GDK_KEY_Tab;
	if (g_ascii_strcasecmp(name, "space") == 0)
		return GDK_KEY_space;
	if (g_ascii_strcasecmp(name, "up") == 0)
		return GDK_KEY_Up;
	if (g_ascii_strcasecmp(name, "down") == 0)
		return GDK_KEY_Down;
	if (g_ascii_strcasecmp(name, "left") == 0)
		return GDK_KEY_Left;
	if (g_ascii_strcasecmp(name, "right") == 0)
		return GDK_KEY_Right;
	if (g_ascii_strcasecmp(name, "home") == 0)
		return GDK_KEY_Home;
	if (g_ascii_strcasecmp(name, "end") == 0)
		return GDK_KEY_End;
	if (g_ascii_strcasecmp(name, "pgup") == 0 || g_ascii_strcasecmp(name, "pageup") == 0)
		return GDK_KEY_Page_Up;
	if (g_ascii_strcasecmp(name, "pgdn") == 0 || g_ascii_strcasecmp(name, "pagedown") == 0)
		return GDK_KEY_Page_Down;
	return gdk_keyval_from_name(name);
}

static gchar *ai_press_keys(RemminaAiPanel *panel, const gchar *combo)
{
	RemminaProtocolWidget *gp = ai_active_gp(panel);
	GtkWidget *w;
	guint keyvals[6];
	gint n = 0;
	gchar **parts, **p;

	if (!gp)
		return g_strdup(_("no active connection"));
	w = ai_find_input_widget(gp);
	if (!w)
		return g_strdup(_("no input widget for this connection"));

	parts = g_strsplit(combo, "+", -1);
	for (p = parts; *p && n < (gint)G_N_ELEMENTS(keyvals); p++) {
		guint kv = ai_keyname_to_keyval(g_strstrip(*p));

		if (kv == 0 || kv == GDK_KEY_VoidSymbol)
			continue;
		keyvals[n++] = kv;
	}
	g_strfreev(parts);

	if (n == 0)
		return g_strdup_printf("unrecognized key combination \"%s\"", combo);

	remmina_protocol_widget_send_keys_signals(w, keyvals, n,
			(GdkEventType)(GDK_KEY_PRESS | GDK_KEY_RELEASE));
	return g_strdup_printf("pressed %s", combo);
}

/* Execute one tool call; always returns a human-readable result string */
/* Execute one tool call; always returns a human-readable result string.
 * If the tool produced an image (remote_screenshot), *out_image_uri is
 * set to a data: URI the caller must send as a multimodal tool result
 * and g_free(). */
static gchar *ai_execute_tool(RemminaAiPanel *panel, const gchar *name,
			      const gchar *args_json, gchar **out_image_uri)
{
	RemminaProtocolWidget *gp = ai_active_gp(panel);
	JsonParser *parser = json_parser_new();
	JsonObject *args = NULL;
	gchar *result;

	if (out_image_uri)
		*out_image_uri = NULL;

	if (json_parser_load_from_data(parser, args_json && *args_json ? args_json : "{}",
				       -1, NULL)) {
		JsonNode *root = json_parser_get_root(parser);

		if (JSON_NODE_HOLDS_OBJECT(root))
			args = json_node_get_object(root);
	}

	if (!gp) {
		g_object_unref(parser);
		return g_strdup(_("no active connection in this window"));
	}

	/* Some models echo the tool name with the "remote_" prefix dropped
	 * (e.g. "click" instead of "remote_click"). Normalize to the verb. */
	{
		const gchar *verb = name;

		if (g_str_has_prefix(verb, "remote_"))
			verb += strlen("remote_");

	if (g_strcmp0(verb, "click") == 0) {
		gint x = args ? (gint)json_object_get_int_member(args, "x") : 0;
		gint y = args ? (gint)json_object_get_int_member(args, "y") : 0;
		const gchar *button = "left";
		gboolean dbl = FALSE;
		guint btn = 1;
		gdouble wx, wy;
		GtkWidget *w;

		if (args && json_object_has_member(args, "button"))
			button = json_object_get_string_member(args, "button");
		if (args && json_object_has_member(args, "double_click"))
			dbl = json_object_get_boolean_member(args, "double_click");
		if (g_strcmp0(button, "right") == 0)
			btn = 3;
		else if (g_strcmp0(button, "middle") == 0)
			btn = 2;

		w = ai_find_input_widget(gp);
		if (!w) {
			g_object_unref(parser);
			return g_strdup(_("no clickable surface in this connection"));
		}
		if (remmina_ssh_plugin_get_vte(gp)) {
			g_object_unref(parser);
			return g_strdup(_("clicking is not available in an SSH terminal session"));
		}
		ai_map_point(panel, w, x, y, &wx, &wy);
		ai_emit_motion(w, wx, wy);
		ai_emit_button(w, btn, wx, wy, TRUE);
		ai_emit_button(w, btn, wx, wy, FALSE);
		if (dbl) {
			ai_emit_button(w, btn, wx, wy, TRUE);
			ai_emit_button(w, btn, wx, wy, FALSE);
		}
		result = g_strdup_printf("%s-clicked at screenshot pixel (%d, %d)",
					 button, x, y);
	} else if (g_strcmp0(verb, "type") == 0) {
		const gchar *text = args ? json_object_get_string_member(args, "text") : NULL;
		gboolean enter = TRUE;

		if (args && json_object_has_member(args, "press_enter"))
			enter = json_object_get_boolean_member(args, "press_enter");
		if (!text || !*text)
			result = g_strdup(_("empty text"));
		else
			result = ai_type_text(panel, text, enter);
	} else if (g_strcmp0(verb, "key") == 0) {
		const gchar *keys = args ? json_object_get_string_member(args, "keys") : NULL;

		if (!keys || !*keys)
			result = g_strdup(_("empty key combination"));
		else
			result = ai_press_keys(panel, keys);
	} else if (g_strcmp0(verb, "scroll") == 0) {
		const gchar *dir = args ? json_object_get_string_member(args, "direction") : "down";
		gint amount = 3, i;
		gboolean up;
		GtkWidget *w;

		if (args && json_object_has_member(args, "amount"))
			amount = (gint)json_object_get_int_member(args, "amount");
		amount = CLAMP(amount, 1, 15);
		up = (g_strcmp0(dir, "up") == 0);
		w = ai_find_input_widget(gp);
		if (!w || remmina_ssh_plugin_get_vte(gp)) {
			g_object_unref(parser);
			return g_strdup(_("scrolling is not available for this connection"));
		}
		for (i = 0; i < amount; i++)
			ai_emit_scroll(w,
				gtk_widget_get_allocated_width(w) / 2.0,
				gtk_widget_get_allocated_height(w) / 2.0, up);
		result = g_strdup_printf("scrolled %s %d clicks", up ? "up" : "down", amount);
	} else if (g_strcmp0(verb, "screenshot") == 0) {
		GdkPixbuf *pix = NULL;
		gchar *err = NULL;

		if (remmina_ssh_plugin_get_vte(gp)) {
			gchar *txt = ai_capture_terminal_text(gp);

			if (!txt) {
				g_object_unref(parser);
				return g_strdup(_("could not read the terminal buffer"));
			}
			result = g_strdup(txt);
			g_free(txt);
		} else {
			pix = ai_capture_remote(gp, &err);
			if (!pix) {
				g_object_unref(parser);
				return err ? err : g_strdup(_("screenshot failed"));
			}
			g_free(err);
			/* refresh click-mapping geometry from the new frame */
			panel->remote_w = gdk_pixbuf_get_width(pix);
			panel->remote_h = gdk_pixbuf_get_height(pix);
			{
				gint mw = MAX(panel->remote_w, panel->remote_h);
				gdouble f = mw > AI_MAX_IMG_DIM ?
					(gdouble)AI_MAX_IMG_DIM / mw : 1.0;

				panel->img_w = (gint)(panel->remote_w * f + 0.5);
				panel->img_h = (gint)(panel->remote_h * f + 0.5);
			}
			*out_image_uri = ai_pixbuf_to_data_uri(pix);
			g_object_unref(pix);
			if (!*out_image_uri) {
				g_object_unref(parser);
				return g_strdup(_("could not encode the screenshot"));
			}
			result = g_strdup_printf(
				"screenshot captured; the attached image is %dx%d "
				"pixels — use coordinates in this image space",
				panel->img_w, panel->img_h);
		}
	} else {
		result = g_strdup_printf("unknown tool \"%s\"", name);
	}
	}   /* end tool-name normalization block */

	g_object_unref(parser);
	return result;
}

/* ------------------------------------------------------------------ */
/* Tool-loop plumbing                                                  */
/* ------------------------------------------------------------------ */

/* Assistant message carrying tool calls (required by the OpenAI protocol
 * before the matching tool-result messages) */
static gchar *ai_message_json_toolcalls(const gchar *content, GPtrArray *tcs)
{
	JsonObject *msg = json_object_new();
	JsonArray *calls = json_array_new();
	guint i;

	json_object_set_string_member(msg, "role", "assistant");
	json_object_set_string_member(msg, "content", content ? content : "");
	for (i = 0; i < tcs->len; i++) {
		AiToolCall *tc = g_ptr_array_index(tcs, i);
		JsonObject *call = json_object_new();
		JsonObject *fn = json_object_new();

		json_object_set_string_member(call, "id", tc->id ? tc->id : "call");
		json_object_set_string_member(call, "type", "function");
		json_object_set_string_member(fn, "name", tc->name ? tc->name : "");
		json_object_set_string_member(fn, "arguments", tc->args->str);
		json_object_set_object_member(call, "function", fn);
		json_array_add_object_element(calls, call);
	}
	json_object_set_array_member(msg, "tool_calls", calls);
	{
		gchar *out = ai_serialize_message(msg);

		json_object_unref(msg);
		return out;
	}
}

static gchar *ai_tool_result_json(const gchar *call_id, const gchar *name,
				  const gchar *content)
{
	JsonObject *msg = json_object_new();
	gchar *out;

	json_object_set_string_member(msg, "role", "tool");
	json_object_set_string_member(msg, "tool_call_id", call_id ? call_id : "call");
	json_object_set_string_member(msg, "name", name ? name : "");
	json_object_set_string_member(msg, "content", content ? content : "");
	out = ai_serialize_message(msg);
	json_object_unref(msg);
	return out;
}

/* Tool result carrying a screenshot (multimodal content array); verified
 * to be accepted by the local mlx-serve endpoint. */
static gchar *ai_tool_result_json_with_image(const gchar *call_id,
					     const gchar *name,
					     const gchar *text,
					     const gchar *image_data_uri)
{
	JsonObject *msg = json_object_new();
	JsonArray *content = json_array_new();
	JsonObject *part;
	gchar *out;

	part = json_object_new();
	json_object_set_string_member(part, "type", "text");
	json_object_set_string_member(part, "text", text ? text : "screenshot captured");
	json_array_add_object_element(content, part);

	part = json_object_new();
	json_object_set_string_member(part, "type", "image_url");
	{
		JsonObject *iu = json_object_new();

		json_object_set_string_member(iu, "url", image_data_uri);
		json_object_set_object_member(part, "image_url", iu);
	}
	json_array_add_object_element(content, part);

	json_object_set_string_member(msg, "role", "tool");
	json_object_set_string_member(msg, "tool_call_id", call_id ? call_id : "call");
	json_object_set_string_member(msg, "name", name ? name : "");
	json_object_set_array_member(msg, "content", content);
	out = ai_serialize_message(msg);
	json_object_unref(msg);
	return out;
}

/* The tools schema sent with the request when control is allowed */
static void ai_add_tools(JsonObject *root)
{
	JsonArray *tools = json_array_new();

	/* helper to build one function tool */
	struct { const char *name, *desc, *params; } t[] = {
		{ "remote_click",
		  "Click on the remote desktop screen. Coordinates are pixels in the "
		  "attached screenshot image, which is the full remote screen at "
		  "its native resolution (origin top-left). Aim at the centre of "
		  "the target element.",
		  "{\"type\":\"object\",\"properties\":{"
		  "\"x\":{\"type\":\"integer\",\"description\":\"X pixel in the screenshot\"},"
		  "\"y\":{\"type\":\"integer\",\"description\":\"Y pixel in the screenshot\"},"
		  "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"]},"
		  "\"double_click\":{\"type\":\"boolean\"}}"
		  ",\"required\":[\"x\",\"y\"]}" },
		{ "remote_type",
		  "Type text into the remote session as if the user typed it. "
		  "Use for terminal commands or text fields. Newlines press Enter.",
		  "{\"type\":\"object\",\"properties\":{"
		  "\"text\":{\"type\":\"string\"},"
		  "\"press_enter\":{\"type\":\"boolean\",\"description\":\"press Enter at the end (default true)\"}}"
		  ",\"required\":[\"text\"]}" },
		{ "remote_key",
		  "Press a key or key combination on the remote machine, e.g. "
		  "\"ctrl+alt+delete\", \"enter\", \"esc\", \"win\", \"alt+tab\", \"ctrl+c\".",
		  "{\"type\":\"object\",\"properties\":{\"keys\":{\"type\":\"string\"}}"
		  ",\"required\":[\"keys\"]}" },
		{ "remote_scroll",
		  "Scroll the remote desktop view up or down.",
		  "{\"type\":\"object\",\"properties\":{"
		  "\"direction\":{\"type\":\"string\",\"enum\":[\"up\",\"down\"]},"
		  "\"amount\":{\"type\":\"integer\",\"description\":\"wheel clicks, 1-15\"}}"
		  ",\"required\":[\"direction\"]}" },
		{ "remote_screenshot",
		  "Capture the current remote screen and return it as an image. "
		  "Call this AFTER clicking or typing to see what changed before "
		  "your next action. In SSH sessions it returns the terminal text "
		  "instead.",
		  "{\"type\":\"object\",\"properties\":{}}" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(t); i++) {
		JsonParser *p = json_parser_new();
		JsonObject *tool = json_object_new();
		JsonObject *fn = json_object_new();

		json_object_set_string_member(tool, "type", "function");
		json_object_set_string_member(fn, "name", t[i].name);
		json_object_set_string_member(fn, "description", t[i].desc);
		if (json_parser_load_from_data(p, t[i].params, -1, NULL))
			json_object_set_member(fn, "parameters",
				json_node_copy(json_parser_get_root(p)));
		g_object_unref(p);
		json_object_set_object_member(tool, "function", fn);
		json_array_add_object_element(tools, tool);
	}
	json_object_set_member(root, "tools",
		json_node_init_array(json_node_alloc(), tools));
	json_object_set_string_member(root, "tool_choice", "auto");
}

/* g_idle handler: HTTP worker finished. job is freed here. */
/* Build a streaming /chat/completions request from the current history
 * (no new message) and launch it. This is also how we continue after
 * executing tool calls. */
static void ai_send_chat_request(RemminaAiPanel *panel);

static void ai_history_trim(RemminaAiPanel *panel)
{
	/* Drop the oldest exchange when the history grows, but never split a
	 * tool_calls sequence (assistant-with-tool-calls followed by tool
	 * results), which the API requires to stay contiguous. Index 0 is the
	 * system prompt and is kept. */
	while (panel->history->len > 32) {
		const gchar *m1 = g_ptr_array_index(panel->history, 1);

		if (strstr(m1, "\"tool_call_id\"") || strstr(m1, "\"tool_calls\""))
			break;
		g_free(g_ptr_array_index(panel->history, 1));
		g_ptr_array_remove_index(panel->history, 1);
	}
}

/* The system prompt is rebuilt for every request so it always matches
 * whether the control tools are attached right now — otherwise an early
 * "I cannot click" answer keeps anchoring the model after tools are on. */
static const gchar *ai_system_prompt(RemminaAiPanel *panel)
{
	static const gchar *base =
		"You are an AI assistant embedded in Remmina, a remote-desktop "
		"client. The user may attach a screenshot of their current "
		"remote session (RDP/VNC) or the text of a terminal session. "
		"Help them with whatever is on the remote machine: explain "
		"errors, suggest or explain commands, read logs, troubleshoot. "
		"Be concise. When you give shell commands, put each command on "
		"its own line in a code block so the user can send it into the "
		"session.";
	static const gchar *tools =
		" You also have control tools (remote_click, remote_type, "
		"remote_key, remote_scroll, remote_screenshot) and CAN click "
		"and type on the user's remote machine when asked. Click "
		"coordinates are absolute pixels in the attached screenshot "
		"image (origin top-left); aim at the centre of the target. "
		"Prefer remote_key combinations and remote_type over clicking "
		"when either would work. After each action, call "
		"remote_screenshot to see the result before your next action; "
		"if a click missed, correct your coordinates and retry.";
	static GString *full;

	if (!panel->tools_enabled)
		return base;
	if (!full)
		full = g_string_new(base);
	else
		g_string_assign(full, base);
	return g_string_append(full, tools)->str;
}

/* forward: defined below with the streaming code */
static void ai_send_chat_request(RemminaAiPanel *panel);

static void ai_send_chat_request(RemminaAiPanel *panel)
{
	JsonObject *root = json_object_new();
	JsonArray *msgs = json_array_new();
	guint i;
	gchar *payload, *url;
	AiHttpJob *job;

	if (panel->model && *panel->model)
		json_object_set_string_member(root, "model", panel->model);
	json_object_set_boolean_member(root, "stream", TRUE);
	if (panel->tools_enabled)
		ai_add_tools(root);

	for (i = 0; i < panel->history->len; i++) {
		JsonParser *p = json_parser_new();

		if (json_parser_load_from_data(p, g_ptr_array_index(panel->history, i),
					       -1, NULL)) {
			JsonNode *node = json_node_copy(json_parser_get_root(p));

			/* refresh the system prompt every request: it must
			 * reflect whether the control tools are attached now */
			if (i == 0 && json_node_get_node_type(node) == JSON_NODE_OBJECT) {
				JsonObject *o = json_node_get_object(node);

				if (json_object_has_member(o, "role") &&
				    g_strcmp0(json_object_get_string_member(o, "role"),
					      "system") == 0)
					json_object_set_string_member(o, "content",
						ai_system_prompt(panel));
			}
			json_array_add_element(msgs, node);
		}
		g_object_unref(p);
	}
	json_object_set_member(root, "messages",
		json_node_init_array(json_node_alloc(), msgs));

	payload = ai_serialize_message(root);
	json_object_unref(root);

	url = ai_endpoint_url(panel, "/chat/completions");
	job = ai_job_new(panel, AI_JOB_CHAT, url, payload, TRUE);
	g_free(url);
	g_free(payload);

	panel->busy = TRUE;
	panel->active_job = job;
	gtk_widget_set_sensitive(panel->send_btn, FALSE);
	ai_stream_begin(panel);
	ai_launch(job);
}

/* Continue the tool loop: feed results back and ask again */
static void ai_tool_round_continue(RemminaAiPanel *panel)
{
	panel->tool_rounds++;
	ai_history_trim(panel);
	ai_send_chat_request(panel);
}

/* The user clicked the "» Continue" link after the tool-round cap was
 * hit. The history is already consistent (the last assistant tool-call
 * message is followed by its tool results), so we only: retire the link
 * visually, drop the "(stopped)" note we fed the model so the resumed
 * history reads as an unbroken action chain, and restart the round
 * counter for a fresh cap-sized budget. */
static void ai_continue_after_limit(RemminaAiPanel *panel)
{
	GtkTextTag *cb = gtk_text_tag_table_lookup(
			gtk_text_buffer_get_tag_table(panel->chat_buf),
			"contbtn");

	panel->continue_pending = FALSE;
	if (cb) {
		GtkTextIter s, e;

		gtk_text_buffer_get_bounds(panel->chat_buf, &s, &e);
		gtk_text_buffer_remove_tag(panel->chat_buf, cb, &s, &e);
	}
	if (panel->history->len > 0) {
		gchar *last = g_ptr_array_index(panel->history,
					panel->history->len - 1);
		gchar *stop = ai_message_json("assistant",
				"(stopped: too many tool actions)");

		if (last && g_strcmp0(last, stop) == 0)
			g_ptr_array_remove_index(panel->history,
					panel->history->len - 1);
		g_free(stop);
	}
	panel->tool_rounds = 0;
	ai_tool_round_continue(panel);
}

/* Finalize a streamed assistant turn that produced tool calls */
/* ------------------------------------------------------------------ */
/* Dangerous-action guardrail                                          */
/* ------------------------------------------------------------------ */

/* Heuristic blocklist for text the AI wants typed into the remote
 * machine. All patterns are lowercase substrings matched against a
 * lowercased copy of the text. This is a guardrail, not a sandbox: it
 * catches the classic footguns so a misreading model cannot type them
 * unattended, and it can be switched off ("Confirm risky actions" in
 * the panel settings). */
static const gchar *const ai_risky_words[] = {
	/* destructive file ops */
	"rm -rf", "rm -fr", "rm -r ", "rm --recursive",
	"rd /s", "rmdir /s", "del /f", "del /s", "del /q",
	"mkfs", "dd if=", "dd of=", "diskpart", "/dev/sd",
	"format c:", "format /",
	"chmod 777", "chmod -r", "chown -r",
	/* power state */
	"shutdown", "reboot", "stop-computer", "restart-computer",
	/* download-and-run / obfuscated script */
	"| iex", "|iex", "invoke-expression", "-encodedcommand",
	"set-executionpolicy",
	/* accounts, firewall, defender, registry */
	"net user", "net localgroup", "netsh", "reg delete",
	"set-mppreference", "add-mppreference",
	"userdel", "usermod", "visudo",
	"iptables -f", "ufw disable",
	/* databases */
	"drop database", "drop table", "truncate table", "delete from",
	/* processes & force-pushes */
	"taskkill /f", "killall -9",
	"git push --force", "reset --hard",
	/* infra teardown */
	"terraform destroy",
	/* fork bomb */
	":(){",
	NULL
};

/* Both substrings must appear (download piped into a shell, recursive
 * Remove-Item, backup destruction...) */
static const gchar *const ai_risky_pairs[] = {
	"curl",  "| sh",   "curl", "|sh",
	"wget",  "| sh",   "wget", "|sh",
	"curl",  "| iex",  "curl", "|iex",
	"wget",  "| iex",  "wget", "|iex",
	"remove-item", "recurse", "remove-item", "force",
	"vssadmin", "delete", "wbadmin", "delete",
	NULL
};

static gboolean ai_text_is_risky(const gchar *text)
{
	gchar *low = g_ascii_strdown(text, -1);
	gboolean risky = FALSE;
	guint i;

	for (i = 0; ai_risky_words[i] && !risky; i++)
		if (strstr(low, ai_risky_words[i]))
			risky = TRUE;
	for (i = 0; ai_risky_pairs[i] && !risky; i += 2)
		if (strstr(low, ai_risky_pairs[i]) &&
		    strstr(low, ai_risky_pairs[i + 1]))
			risky = TRUE;
	g_free(low);
	return risky;
}

/* True when the tool call would type text matching the blocklist.
 * Clicks/scrolls/screenshots have no textual payload to judge and are
 * never gated; key combinations are routine navigation and pass too. */
static gboolean ai_toolcall_is_risky(AiToolCall *tc)
{
	const gchar *verb = tc->name;
	JsonParser *parser;
	JsonNode *root;
	const gchar *text = NULL;
	gboolean risky = FALSE;

	if (!verb)
		return FALSE;
	if (g_str_has_prefix(verb, "remote_"))
		verb += strlen("remote_");
	if (g_strcmp0(verb, "type") != 0)
		return FALSE;

	parser = json_parser_new();
	if (!json_parser_load_from_data(parser,
			tc->args && tc->args->len ? tc->args->str : "{}",
			-1, NULL)) {
		g_object_unref(parser);
		return FALSE;
	}
	root = json_parser_get_root(parser);
	if (JSON_NODE_HOLDS_OBJECT(root)) {
		JsonObject *o = json_node_get_object(root);

		if (json_object_has_member(o, "text") &&
		    JSON_NODE_HOLDS_VALUE(json_object_get_member(o, "text")))
			text = json_object_get_string_member(o, "text");
		if (text && *text)
			risky = ai_text_is_risky(text);
	}
	g_object_unref(parser);
	return risky;
}

/* Present the held round in the approval bar and wait for the user */
static void ai_approval_show(RemminaAiPanel *panel)
{
	GString *txt = g_string_new(NULL);
	gchar *lead, *combined;
	guint i;

	for (i = 0; i < panel->pending_tcs->len; i++) {
		AiToolCall *tc = g_ptr_array_index(panel->pending_tcs, i);
		JsonParser *parser = json_parser_new();
		const gchar *text = NULL;

		if (json_parser_load_from_data(parser,
				tc->args && tc->args->len ? tc->args->str : "{}",
				-1, NULL)) {
			JsonNode *root = json_parser_get_root(parser);

			if (JSON_NODE_HOLDS_OBJECT(root)) {
				JsonObject *o = json_node_get_object(root);

				if (json_object_has_member(o, "text") &&
				    JSON_NODE_HOLDS_VALUE(
					    json_object_get_member(o, "text")))
					text = json_object_get_string_member(
							o, "text");
			}
		}
		if (text && *text) {
			gchar *one = NULL, *esc;

			if (g_utf8_strlen(text, -1) > 160) {
				const gchar *end =
					g_utf8_offset_to_pointer(text, 160);

				one = g_strconcat(
					g_strndup(text, (glong)(end - text)),
					"…", NULL);
			}
			esc = g_markup_escape_text(one ? one : text, -1);
			g_string_append_printf(txt, "<b>%s</b>:\n<tt>%s</tt>\n",
					tc->name ? tc->name : "?", esc);
			g_free(esc);
			g_free(one);
		} else {
			g_string_append_printf(txt, "<b>%s</b>\n",
					tc->name ? tc->name : "?");
		}
		g_object_unref(parser);
	}
	lead = _("<span foreground=\"#e5a50a\"><b>⚠ Potentially dangerous "
		"action</b></span>\nType this on the remote machine?");
	combined = g_strconcat(lead, "\n", txt->str, NULL);
	gtk_label_set_markup(GTK_LABEL(panel->approval_label), combined);
	g_free(combined);
	g_string_free(txt, TRUE);

	/* the bar carries no_show_all so the panel's own show_all skips
	 * it; lift it for the duration of this one presentation */
	gtk_widget_set_no_show_all(panel->approval_bar, FALSE);
	gtk_widget_show_all(panel->approval_bar);
	gtk_widget_set_no_show_all(panel->approval_bar, TRUE);
}

static void ai_clear_pending(RemminaAiPanel *panel)
{
	if (panel->pending_tcs) {
		g_ptr_array_free(panel->pending_tcs, TRUE);
		panel->pending_tcs = NULL;
	}
	gtk_widget_hide(panel->approval_bar);
}

/* Allow: run every held call as if nothing happened, resume the loop */
static void ai_on_approval_allow(GtkButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	guint i;

	(void)btn;
	if (!panel->pending_tcs)
		return;
	for (i = 0; i < panel->pending_tcs->len; i++)
		ai_run_toolcall(panel, g_ptr_array_index(panel->pending_tcs, i));
	ai_clear_pending(panel);
	ai_tool_round_tail(panel);
}

/* Deny: the OpenAI protocol still demands a tool result for every tool
 * call, so feed back a refusal and let the model redirect itself. */
static void ai_on_approval_deny(GtkButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	guint i;

	(void)btn;
	if (!panel->pending_tcs)
		return;
	for (i = 0; i < panel->pending_tcs->len; i++) {
		AiToolCall *tc = g_ptr_array_index(panel->pending_tcs, i);
		gchar *sys = g_strdup_printf("⛔ %s denied by user",
				tc->name ? tc->name : "?");

		ai_chat_append(panel, NULL, "error", sys);
		g_free(sys);
		ai_history_append(panel, ai_tool_result_json(tc->id, tc->name,
			"The user denied this action as potentially dangerous. "
			"Do not retry it; ask the user or choose a safer "
			"approach."));
	}
	ai_clear_pending(panel);
	ai_tool_round_tail(panel);
}

/* ------------------------------------------------------------------ */
/* Tool loop                                                           */
/* ------------------------------------------------------------------ */

/* Execute one tool call, show its one-line summary and record the
 * tool result the API expects */
static void ai_run_toolcall(RemminaAiPanel *panel, AiToolCall *tc)
{
	gchar *img_uri = NULL;
	gchar *result = ai_execute_tool(panel,
			tc->name ? tc->name : "", tc->args->str, &img_uri);
	gchar *sys = g_strdup_printf("⚙ %s → %s",
			tc->name ? tc->name : "?", result);

	ai_chat_append(panel, NULL, "tool", sys);
	g_free(sys);
	if (img_uri) {
		ai_history_append(panel,
			ai_tool_result_json_with_image(tc->id, tc->name,
							result, img_uri));
		g_free(img_uri);
	} else {
		ai_history_append(panel,
			ai_tool_result_json(tc->id, tc->name, result));
	}
	g_free(result);

	/* The AI just acted on the remote machine: hand keyboard focus to
	 * the session so the user can type into whatever it opened there
	 * (a credentials prompt, a form field) without clicking first —
	 * their caret has been parked in the chat entry since they sent
	 * the message, and nothing else moves it back. Skipped while the
	 * user is composing a follow-up (entry non-empty). Focus is
	 * per-window: this never steals the keyboard from another window. */
	{
		const gchar *verb = tc->name;
		RemminaProtocolWidget *gp = ai_active_gp(panel);

		if (gp && verb &&
		    gtk_entry_get_text_length(GTK_ENTRY(panel->input)) == 0) {
			if (g_str_has_prefix(verb, "remote_"))
				verb += strlen("remote_");
			if (g_strcmp0(verb, "click") == 0 ||
			    g_strcmp0(verb, "type") == 0 ||
			    g_strcmp0(verb, "key") == 0 ||
			    g_strcmp0(verb, "scroll") == 0)
				remmina_protocol_widget_grab_focus(gp);
		}
	}
}

/* After a resolved tool round: keep looping, or offer the continue
 * link once the round budget is spent */
static void ai_tool_round_tail(RemminaAiPanel *panel)
{
	gint cap = ai_max_tool_rounds();

	if (panel->tool_rounds >= cap) {
		gchar *lbl = g_strdup_printf(
			_("» Continue — allow %d more actions"), cap);

		ai_chat_append(panel, NULL, "contbtn", lbl);
		g_free(lbl);
		/* tell the model it was cut short; the continue
		 * handler removes this note again on resume */
		ai_history_append(panel, ai_message_json("assistant",
			"(stopped: too many tool actions)"));
		panel->continue_pending = TRUE;
		panel->busy = FALSE;
		gtk_widget_set_sensitive(panel->send_btn, TRUE);
		gtk_widget_hide(panel->stop_btn);
		panel->tool_rounds = 0;
	} else {
		ai_tool_round_continue(panel);
	}
}

/* Stop button: cancel the running turn. Two states:
 *  - a request is in flight (active_job): flag it cancelled; the worker's
 *    write/progress callback aborts and http_done finalizes (keeping any
 *    partial text, running no tools).
 *  - the approval bar is held (no request in flight): Stop acts as a
 *    blanket Deny that also halts the loop, so every pending tool call
 *    still gets a tool result (history stays valid) but nothing runs and
 *    the model is not given another turn. */
static void
ai_on_stop(GtkButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	guint i;

	(void)btn;
	if (panel->pending_tcs) {
		for (i = 0; i < panel->pending_tcs->len; i++) {
			AiToolCall *tc =
				g_ptr_array_index(panel->pending_tcs, i);
			gchar *sys = g_strdup_printf("⏹ %s not run",
					tc->name ? tc->name : "?");

			ai_chat_append(panel, NULL, "error", sys);
			g_free(sys);
			ai_history_append(panel,
				ai_tool_result_json(tc->id, tc->name,
					"The user stopped the session before "
					"this action ran."));
		}
		ai_clear_pending(panel);
		panel->busy = FALSE;
		panel->tool_rounds = 0;
		panel->active_job = NULL;
		gtk_widget_set_sensitive(panel->send_btn, TRUE);
		gtk_widget_hide(panel->stop_btn);
		return;
	}

	if (panel->active_job) {
		panel->active_job->cancelled = TRUE;
		/* http_done finalizes once curl aborts; stop further
		 * Stop clicks meanwhile */
		gtk_widget_set_sensitive(panel->stop_btn, FALSE);
	}
}

static void ai_finalize_stream_with_tools(RemminaAiPanel *panel, AiHttpJob *job)
{
	GPtrArray *tcs = job->tool_calls;
	guint i;

	/* assistant message carrying the tool calls must precede the results */
	ai_history_append(panel, ai_message_json_toolcalls(job->content->str, tcs));

	/* Dangerous-action gate: if ANY call in this round would type
	 * blocklisted text, hold the whole round (all-or-nothing keeps
	 * the batch semantics simple) and let the user allow or deny.
	 * The call array is stolen from the job — it is freed moments
	 * after this function returns. */
	if (panel->confirm_risky) {
		for (i = 0; i < tcs->len; i++) {
			if (!ai_toolcall_is_risky(g_ptr_array_index(tcs, i)))
				continue;
			panel->pending_tcs = tcs;
			job->tool_calls = NULL;
			ai_approval_show(panel);
			return;
		}
	}

	for (i = 0; i < tcs->len; i++)
		ai_run_toolcall(panel, g_ptr_array_index(tcs, i));
	ai_tool_round_tail(panel);
}

static gboolean remmina_ai_panel_http_done(gpointer userdata)
{
	AiHttpJob *job = (AiHttpJob *)userdata;
	RemminaAiPanel *panel;
	GtkWidget *alive;

	alive = g_weak_ref_get(&job->weak_ref);
	panel = alive ? g_object_get_data(G_OBJECT(alive), "ai-panel") : NULL;

	if (!panel) {
		/* panel destroyed; the stream sink was already marked dead */
		ai_job_free(job);
		return G_SOURCE_REMOVE;
	}

	if (job->kind == AI_JOB_MODELS) {
		if (job->response->len > 0)
			ai_refresh_models_from_body(panel, job->response->str);
		else if (job->errbuf[0]) {
			gchar *e = g_strdup_printf("%s: %s",
					_("Could not reach the AI endpoint"), job->errbuf);
			ai_chat_append(panel, NULL, "error", e);
			g_free(e);
		}
		ai_job_free(job);
		return G_SOURCE_REMOVE;
	}

	/* the request that owned this pointer has returned */
	panel->active_job = NULL;

	/* user pressed Stop: finalize the turn without running any tool
	 * calls. Keep the partial text as the assistant turn so the chat
	 * reads coherently; drop partial tool-call deltas (they were never
	 * executed and would dangle in history). */
	if (job->cancelled) {
		ai_stream_end(panel);
		if (job->content->len) {
			gchar *text = g_strdup(job->content->str);

			g_free(panel->last_reply);
			panel->last_reply = text;
			ai_history_append(panel, ai_message_json("assistant", text));
			ai_history_trim(panel);
		} else {
			ai_history_append(panel, ai_message_json("assistant",
				"(stopped by user before responding)"));
		}
		ai_chat_append(panel, NULL, "tool", "⏹ stopped by user");
		panel->busy = FALSE;
		panel->tool_rounds = 0;
		gtk_widget_set_sensitive(panel->send_btn, TRUE);
		gtk_widget_hide(panel->stop_btn);
		remmina_ai_panel_update_context(panel, panel->connected);
		ai_job_free(job);
		return G_SOURCE_REMOVE;
	}

	/* streaming chat completion */
	if (job->errbuf[0] && job->content->len == 0) {
		gchar *e = g_strdup_printf("%s: %s", _("AI request failed"), job->errbuf);

		ai_stream_end(panel);
		ai_chat_append(panel, NULL, "error", e);
		g_free(e);
		panel->busy = FALSE;
		gtk_widget_set_sensitive(panel->send_btn, TRUE);
		gtk_widget_hide(panel->stop_btn);
		ai_job_free(job);
		return G_SOURCE_REMOVE;
	}

	if (job->finish_tool_calls && job->tool_calls->len > 0) {
		ai_stream_end(panel);
		ai_finalize_stream_with_tools(panel, job);
		ai_job_free(job);
		if (panel->busy == FALSE)
			remmina_ai_panel_update_context(panel, panel->connected);
		return G_SOURCE_REMOVE;
	}

	/* plain text answer */
	ai_stream_end(panel);
	{
		gchar *text = job->content->len ? g_strdup(job->content->str) : NULL;

		if (text && *text) {
			g_free(panel->last_reply);
			panel->last_reply = g_strdup(text);
			ai_history_append(panel, ai_message_json("assistant", text));
			ai_history_trim(panel);
		} else {
			ai_chat_append(panel, NULL, "error",
				       _("AI endpoint returned no text"));
		}
		g_free(text);
	}
	panel->busy = FALSE;
	panel->tool_rounds = 0;
	gtk_widget_set_sensitive(panel->send_btn, TRUE);
	gtk_widget_hide(panel->stop_btn);
	remmina_ai_panel_update_context(panel, panel->connected);

	ai_job_free(job);
	return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Chat payload construction                                           */
/* ------------------------------------------------------------------ */

/* Serialize a simple text message: {"role":role,"content":text} */
static gchar *ai_message_json(const gchar *role, const gchar *text)
{
	JsonObject *msg = json_object_new();
	gchar *out;

	json_object_set_string_member(msg, "role", role);
	json_object_set_string_member(msg, "content", text);
	out = ai_serialize_message(msg);
	json_object_unref(msg);
	return out;
}

/* Serialize a multimodal message with a screenshot:
 * content = [ {type:text,text:...}, {type:image_url,image_url:{url:...}} ] */
static gchar *ai_message_json_with_image(const gchar *role, const gchar *text,
					 const gchar *image_data_uri)
{
	JsonObject *msg = json_object_new();
	JsonArray *content = json_array_new();
	JsonObject *part;
	gchar *out;

	part = json_object_new();
	json_object_set_string_member(part, "type", "text");
	json_object_set_string_member(part, "text", text);
	json_array_add_object_element(content, part);

	part = json_object_new();
	json_object_set_string_member(part, "type", "image_url");
	{
		JsonObject *iu = json_object_new();
		json_object_set_string_member(iu, "url", image_data_uri);
		json_object_set_object_member(part, "image_url", iu);
	}
	json_array_add_object_element(content, part);

	json_object_set_string_member(msg, "role", role);
	json_object_set_array_member(msg, "content", content);
	out = ai_serialize_message(msg);
	json_object_unref(msg);
	return out;
}


/* ------------------------------------------------------------------ */
/* Model list                                                          */
/* ------------------------------------------------------------------ */

static void ai_refresh_models_from_body(RemminaAiPanel *panel, const gchar *body)
{
	JsonParser *p = json_parser_new();
	GtkTreeModel *store;
	const gchar *current;
	gboolean found = FALSE;

	if (!json_parser_load_from_data(p, body, -1, NULL)) {
		g_object_unref(p);
		return;
	}
	JsonNode *root = json_parser_get_root(p);
	if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
		g_object_unref(p);
		return;
	}
	JsonObject *o = json_node_get_object(root);
	if (!json_object_has_member(o, "data")) {
		g_object_unref(p);
		return;
	}

	store = gtk_combo_box_get_model(GTK_COMBO_BOX(panel->model_combo));
	g_signal_handlers_block_by_func(panel->model_combo,
					ai_on_model_changed, panel);
	current = panel->model;
	gtk_list_store_clear(GTK_LIST_STORE(store));
	{
		JsonArray *data = json_object_get_array_member(o, "data");
		guint i;
		for (i = 0; i < json_array_get_length(data); i++) {
			JsonObject *m = json_array_get_object_element(data, i);
			const gchar *id = json_object_get_string_member(m, "id");
			gboolean vision = FALSE;
			gboolean loaded = FALSE;

			if (json_object_has_member(m, "capabilities")) {
				JsonArray *caps = json_object_get_array_member(m, "capabilities");
				guint c;
				for (c = 0; c < json_array_get_length(caps); c++)
					if (g_strcmp0(json_array_get_string_element(caps, c),
						      "vision") == 0)
						vision = TRUE;
			}
			if (json_object_has_member(m, "state"))
				loaded = g_strcmp0(json_object_get_string_member(m, "state"),
						   "ready") == 0;
			else if (json_object_has_member(m, "loaded"))
				loaded = json_object_get_boolean_member(m, "loaded");

			gtk_list_store_insert_with_values(GTK_LIST_STORE(store), NULL, -1,
					0, id, 1, i, -1);
			if (current && g_strcmp0(id, current) == 0)
				found = TRUE;
			(void)vision;  /* reserved for gating screenshot attach */
			(void)loaded;  /* shown in the model list popup only */
		}
	}
	g_signal_handlers_unblock_by_func(panel->model_combo,
					  ai_on_model_changed, panel);

	/* keep an unknown saved model visible in the entry even if absent */
	if (!found && panel->model && *panel->model) {
		GtkListStore *ls = GTK_LIST_STORE(store);
		GtkTreeIter it;
		gtk_list_store_insert_with_values(ls, &it, 0, 0, panel->model, 1, 0, -1);
		gtk_combo_box_set_active_iter(GTK_COMBO_BOX(panel->model_combo), &it);
	} else {
		/* select the row matching the saved model, else the first one */
		GtkTreeIter it;
		if (gtk_tree_model_get_iter_first(store, &it)) {
			do {
				gchar *id = NULL;
				gtk_tree_model_get(store, &it, 0, &id, -1);
				if (!found || g_strcmp0(id, panel->model) == 0) {
					gtk_combo_box_set_active_iter(GTK_COMBO_BOX(panel->model_combo), &it);
					g_free(id);
					break;
				}
				g_free(id);
			} while (gtk_tree_model_iter_next(store, &it));
		}
	}
	g_object_unref(p);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static gchar *ai_endpoint_url(RemminaAiPanel *panel, const gchar *suffix)
{
	gchar *base = g_strdup(panel->api_url);
	gsize len = strlen(base);

	while (len > 0 && base[len - 1] == '/')
		base[--len] = '\0';
	return g_strconcat(base, suffix, NULL);
}

/* Ask the LLM about the current remote session */
static void ai_on_send(GtkButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	const gchar *typed;
	gboolean attach;
	gchar *user_msg = NULL;
	GdkPixbuf *pix = NULL;
	gchar *term_text = NULL;
	gchar *err = NULL;
	RemminaProtocolWidget *gp;

	(void)btn;
	if (panel->busy)
		return;

	typed = gtk_entry_get_text(GTK_ENTRY(panel->input));
	attach = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel->attach_check));

	if (*typed == '\0')
		return;

	gp = ai_active_gp(panel);

	if (attach) {
		if (!gp) {
			ai_chat_append(panel, NULL, "error",
				       _("Cannot attach the remote screen: no active connection"));
			return;
		}
		if (remmina_ssh_plugin_get_vte(gp))
			term_text = ai_capture_terminal_text(gp);
		else
			pix = ai_capture_remote(gp, &err);
		if (!pix && !term_text) {
			ai_chat_append(panel, NULL, "error",
				       err ? err : _("Could not capture the remote screen"));
			g_free(err);
			return;
		}
		g_free(err);
	}

	if (pix || term_text) {
		GString *ctx = g_string_new(typed);

		if (term_text)
			g_string_append_printf(ctx,
				"\n\n[Terminal content of the current SSH session]\n%s\n[end of terminal content]",
				term_text);
		if (pix) {
			gchar *uri = ai_pixbuf_to_data_uri(pix);

			if (uri) {
				user_msg = ai_message_json_with_image("user", ctx->str, uri);
				g_free(uri);
			}
			/* record geometry so tool clicks map back to the remote.
			 * img_* is what we actually sent (possibly downscaled);
			 * remote_* is the framebuffer size. */
			panel->remote_w = gdk_pixbuf_get_width(pix);
			panel->remote_h = gdk_pixbuf_get_height(pix);
			{
				gint mw = MAX(panel->remote_w, panel->remote_h);
				gdouble f = mw > AI_MAX_IMG_DIM ?
					(gdouble)AI_MAX_IMG_DIM / mw : 1.0;

				panel->img_w = (gint)(panel->remote_w * f + 0.5);
				panel->img_h = (gint)(panel->remote_h * f + 0.5);
			}
		}
		if (!user_msg)
			user_msg = ai_message_json("user", ctx->str);
		g_string_free(ctx, TRUE);
	} else {
		user_msg = ai_message_json("user", typed);
	}

	ai_chat_append(panel, NULL, "userblock", typed);

	/* sending is an explicit "back to the live conversation" action:
	 * re-engage auto-follow and jump to the bottom even if the user
	 * had scrolled up to read history, so their own message and the
	 * incoming reply are always visible */
	panel->stick_bottom = TRUE;
	ai_scroll_bottom(panel);

	g_free(term_text);
	if (pix)
		g_object_unref(pix);

	ai_history_append(panel, user_msg);
	g_free(user_msg);
	ai_history_trim(panel);
	gtk_entry_set_text(GTK_ENTRY(panel->input), "");

	/* a fresh message supersedes any pending "» Continue" offer:
	 * retire the link (drop its click styling so it reads as plain
	 * text) and clear the flag so the old line can no longer fire */
	if (panel->continue_pending) {
		GtkTextTag *cb = gtk_text_tag_table_lookup(
				gtk_text_buffer_get_tag_table(panel->chat_buf),
				"contbtn");

		panel->continue_pending = FALSE;
		if (cb) {
			GtkTextIter s, e;

			gtk_text_buffer_get_bounds(panel->chat_buf, &s, &e);
			gtk_text_buffer_remove_tag(panel->chat_buf, cb, &s, &e);
		}
	}

	/* stream the completion (and any tool round-trips) */
	ai_send_chat_request(panel);
}

/* Type the AI's last reply into the active session */
static void ai_on_type_into_session(GtkButton *btn, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	RemminaProtocolWidget *gp;
	const gchar *text;
	GtkWidget *vte;

	(void)btn;
	text = panel->last_reply;
	gp = ai_active_gp(panel);
	if (!text || !*text) {
		ai_chat_append(panel, NULL, "error", _("No AI reply to type yet"));
		return;
	}
	if (!gp) {
		ai_chat_append(panel, NULL, "error", _("No active connection in this window"));
		return;
	}

	vte = remmina_ssh_plugin_get_vte(gp);
	if (vte) {
		/* Feed straight into the SSH terminal's pty */
		vte_terminal_feed_child(VTE_TERMINAL(vte), text, strlen(text));
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel->enter_check)))
			vte_terminal_feed_child(VTE_TERMINAL(vte), "\r", 1);
		ai_chat_append(panel, NULL, "system", _("Text sent to the SSH session"));
		return;
	}

	if (remmina_protocol_widget_plugin_receives_keystrokes(gp)) {
		/* Synthesize key events through the plugin (RDP/VNC) */
		GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
		const gchar *iter = text;
		guint keyvals[3];
		gint n;

		while (*iter) {
			gunichar ch = g_utf8_get_char_validated(iter, -1);
			guint keyval;
			GdkKeymapKey *keys = NULL;
			gint n_keys = 0;

			if (ch == (gunichar)-1 || ch == (gunichar)-2)
				break;
			iter = g_utf8_next_char(iter);

			if (ch == '\n' || ch == '\r')
				keyval = GDK_KEY_Return;
			else if (ch == '\t')
				keyval = GDK_KEY_Tab;
			else
				keyval = gdk_unicode_to_keyval(ch);

			if (keyval == GDK_KEY_VoidSymbol ||
			    !gdk_keymap_get_entries_for_keyval(keymap, keyval, &keys, &n_keys))
				continue; /* untypeable on this keymap */

			n = 0;
			if (keys->level & 1)
				keyvals[n++] = GDK_KEY_Shift_L;
			if (keys->level & 2)
				keyvals[n++] = GDK_KEY_Alt_R;
			keyvals[n++] = keyval;
			remmina_protocol_widget_send_keys_signals(GTK_WIDGET(gp), keyvals, n,
				(GdkEventType)(GDK_KEY_PRESS | GDK_KEY_RELEASE));
			g_free(keys);
		}
		ai_chat_append(panel, NULL, "system", _("Text typed into the session"));
	} else {
		ai_chat_append(panel, NULL, "error",
			       _("This connection type cannot receive typed keystrokes"));
	}
}

static void ai_on_refresh_models(GtkButton *btn, gpointer data)
{
	(void)btn;
	ai_refresh_models((RemminaAiPanel *)data);
}

static void ai_refresh_models(RemminaAiPanel *panel)
{
	gchar *url = ai_endpoint_url(panel, "/models");
	AiHttpJob *job = ai_job_new(panel, AI_JOB_MODELS, url, NULL, FALSE);

	job->is_get = TRUE;
	ai_launch(job);
	g_free(url);
}

/* Settings changed via the entries */
static void ai_on_url_activate(GtkEntry *entry, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	const gchar *t = gtk_entry_get_text(entry);

	if (t && *t) {
		g_free(panel->api_url);
		panel->api_url = g_strdup(t);
		ai_panel_save_settings(panel);
		{
			gchar *msg = g_strdup_printf(_("AI endpoint set to %s"), panel->api_url);
			ai_chat_append(panel, NULL, "system", msg);
			g_free(msg);
		}
		ai_refresh_models(panel);
	}
}

static void ai_on_key_activate(GtkEntry *entry, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;

	g_free(panel->api_key);
	panel->api_key = g_strdup(gtk_entry_get_text(entry));
	ai_panel_save_settings(panel);
	ai_refresh_models(panel);
}

static void ai_on_model_changed(GtkComboBox *combo, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;
	GtkTreeIter it;

	if (gtk_combo_box_get_active_iter(combo, &it)) {
		gchar *id = NULL;
		gtk_tree_model_get(gtk_combo_box_get_model(combo), &it, 0, &id, -1);
		if (id && *id) {
			g_free(panel->model);
			panel->model = id;
			ai_panel_save_settings(panel);
			return;
		}
		g_free(id);
	}
	/* editable entry typed directly */
	{
		const gchar *t = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))));
		if (t && *t) {
			g_free(panel->model);
			panel->model = g_strdup(t);
			ai_panel_save_settings(panel);
		}
	}
}

static void ai_on_model_entry_activate(GtkEntry *entry, gpointer data)
{
	GtkWidget *combo = gtk_widget_get_ancestor(GTK_WIDGET(entry), GTK_TYPE_COMBO_BOX);

	if (combo)
		ai_on_model_changed(GTK_COMBO_BOX(combo), data);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Global proto-widget -> panel registry (one panel per session). Guarded * implicitly by main-thread-only access. */
static GHashTable *ai_registry = NULL;

static void ai_registry_bind(GtkWidget *proto, RemminaAiPanel *panel)
{
	if (!ai_registry)
		ai_registry = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_hash_table_insert(ai_registry, proto, panel);
}

static void ai_registry_unbind(GtkWidget *proto)
{
	if (ai_registry)
		g_hash_table_remove(ai_registry, proto);
}

GtkWidget *remmina_ai_panel_for_protocol_widget(GtkWidget *proto)
{
	RemminaAiPanel *panel;

	if (!ai_registry || !proto)
		return NULL;
	panel = g_hash_table_lookup(ai_registry, proto);
	return panel ? panel->box : NULL;
}

/* Convenience wrapper for callers holding only the panel widget */
void remmina_ai_panel_update(GtkWidget *panel_widget, gboolean connected)
{
	RemminaAiPanel *panel = panel_widget ?
		g_object_get_data(G_OBJECT(panel_widget), "ai-panel") : NULL;

	if (panel)
		remmina_ai_panel_update_context(panel, connected);
}

void remmina_ai_panel_update_context(RemminaAiPanel *panel, gboolean connected)
{
	const gchar *proto = NULL, *server = NULL, *name = NULL;
	gchar *label;

	panel->connected = connected;

	{
		RemminaProtocolWidget *gp = ai_active_gp(panel);
		gchar *model_short = ai_model_display_name(panel);

		ai_active_info(panel, &proto, &server, &name);

		if (!gp) {
			label = model_short ?
				g_strdup_printf("<small>%s · <i>%s</i></small>",
						_("Not connected"), model_short) :
				g_strdup_printf("<small>%s</small>", _("Not connected"));
		} else {
			const gchar *cap;
			if (g_strcmp0(proto, "RDP") == 0)
				cap = " · screen ✓";
			else if (remmina_ssh_plugin_get_vte(gp))
				cap = " · terminal ✓";
			else if (g_strcmp0(proto, "VNC") == 0 || g_strcmp0(proto, "GVNC") == 0)
				cap = " · screen ✓";
			else
				cap = " · capture limited";
			if (model_short)
				label = g_markup_printf_escaped(
					"<small><b>%s</b> %s<small>%s · <i>%s</i></small></small>",
					proto ? proto : "?",
					name && *name ? name : (server ? server : ""),
					cap, model_short);
			else
				label = g_markup_printf_escaped(
					"<small><b>%s</b> %s<small>%s</small></small>",
					proto ? proto : "?",
					name && *name ? name : (server ? server : ""),
					cap);
		}
		g_free(model_short);

		/* "attach screen" only makes sense with a live connection */
		gtk_widget_set_sensitive(panel->attach_check, gp != NULL);
		gtk_widget_set_sensitive(panel->type_btn, gp != NULL && panel->last_reply);
	}
	gtk_label_set_markup(GTK_LABEL(panel->context_label), label);
	g_free(label);
}

static void ai_panel_destroyed(GtkWidget *widget, gpointer data)
{
	RemminaAiPanel *panel = (RemminaAiPanel *)data;

	(void)widget;
	g_object_set_data(G_OBJECT(widget), "ai-panel", NULL);
	remmina_ai_panel_free(panel);
}

GtkWidget *remmina_ai_panel_new(RemminaConnectionWindow *cnnwin, GtkWidget *proto)
{
	RemminaAiPanel *panel;
	GtkWidget *w, *sw, *hb, *settings;
	GtkListStore *store;
	gchar *system_msg;

	panel = g_new0(RemminaAiPanel, 1);
	panel->cnnwin = cnnwin;
	g_weak_ref_init(&panel->proto_ref, proto);
	panel->connected = FALSE;
	panel->history = g_ptr_array_new();
	panel->busy = FALSE;
	panel->stick_bottom = TRUE;
	ai_panel_load_settings(panel);

	/* top-level vertical box */
	w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	panel->box = w;

	/* Panel colors: pure white + black text in light mode; in dark mode
	 * a near-black background that is darker than the theme chrome with
	 * light text. Chosen from the effective dark-theme preference. */
	{
		static GtkCssProvider *panel_css = NULL; /* one per process */
		GtkSettings *st = gtk_settings_get_default();
		gboolean prefer_dark = FALSE;

		if (panel_css == NULL) {
			panel_css = gtk_css_provider_new();
			gtk_css_provider_load_from_data(panel_css,
				".remmina-ai-light, .remmina-ai-light textview,"
				" .remmina-ai-light textview text,"
				" .remmina-ai-light entry { background-color: #ffffff;"
				" color: #000000; }"
				".remmina-ai-dark, .remmina-ai-dark textview,"
				" .remmina-ai-dark textview text,"
				" .remmina-ai-dark entry { background-color: #16161b;"
				" color: #e8e8e8; }"
				".remmina-ai-dark textview text selection,"
				".remmina-ai-light textview text selection {"
				" background-color: #3d5fbf; color: #ffffff; }",
				-1, NULL);
			gtk_style_context_add_provider_for_screen(
				gdk_screen_get_default(),
				GTK_STYLE_PROVIDER(panel_css),
				GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
		}
		g_object_get(st, "gtk-application-prefer-dark-theme",
			     &prefer_dark, NULL);
		gtk_style_context_add_class(gtk_widget_get_style_context(w),
			prefer_dark ? "remmina-ai-dark" : "remmina-ai-light");
	}
	g_object_set_data(G_OBJECT(w), "ai-panel", panel);
	g_signal_connect(w, "destroy", G_CALLBACK(ai_panel_destroyed), panel);
	gtk_widget_set_size_request(w, 260, -1);

	/* context label */
	panel->context_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(panel->context_label), 0.0);
	/* must not dictate the panel's minimum width */
	gtk_label_set_ellipsize(GTK_LABEL(panel->context_label),
				PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(panel->context_label), 16);
	gtk_box_pack_start(GTK_BOX(w), panel->context_label, FALSE, FALSE, 0);

	/* chat transcript */
	sw = gtk_scrolled_window_new(NULL, NULL);
	panel->scrolled = sw;
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
			GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	panel->chat_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(panel->chat_view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(panel->chat_view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(panel->chat_view), TRUE);
	panel->chat_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(panel->chat_view));
	gtk_container_add(GTK_CONTAINER(sw), panel->chat_view);
	gtk_box_pack_start(GTK_BOX(w), sw, TRUE, TRUE, 0);

	/* click a turn header to collapse/expand it */
	gtk_widget_add_events(panel->chat_view, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(panel->chat_view, "button-press-event",
			 G_CALLBACK(ai_on_chat_view_click), panel);
	/* user-message boxes painted over the text view's own background
	 * (a plain "connect" would paint them before the opaque bg and
	 * they'd be covered) */
	g_signal_connect_after(panel->chat_view, "draw",
			       G_CALLBACK(ai_on_chat_draw), panel);
	/* stop auto-scrolling when the user scrolls up to read history */
	g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)),
			 "value-changed", G_CALLBACK(ai_on_scroll_changed), panel);

	/* register this panel for its session */
	ai_registry_bind(proto, panel);

	/* dangerous-action approval bar, shown only when a risky round
	 * is held back (no_show_all keeps it out of the panel's own
	 * show_all; ai_approval_show lifts it per presentation) */
	{
		GtkWidget *abar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		GtkWidget *allow = gtk_button_new_with_mnemonic(_("_Allow"));
		GtkWidget *deny = gtk_button_new_with_mnemonic(_("_Deny"));

		panel->approval_bar = abar;
		panel->approval_label = gtk_label_new(NULL);
		gtk_label_set_line_wrap(GTK_LABEL(panel->approval_label), TRUE);
		gtk_label_set_xalign(GTK_LABEL(panel->approval_label), 0.0);
		gtk_label_set_max_width_chars(GTK_LABEL(panel->approval_label),
					      36);
		gtk_widget_set_hexpand(panel->approval_label, TRUE);
		gtk_box_pack_start(GTK_BOX(row), panel->approval_label,
				   TRUE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(row), deny, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), allow, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(abar), row, FALSE, FALSE, 0);
		g_signal_connect(allow, "clicked",
				 G_CALLBACK(ai_on_approval_allow), panel);
		g_signal_connect(deny, "clicked",
				 G_CALLBACK(ai_on_approval_deny), panel);
		gtk_widget_set_no_show_all(abar, TRUE);
		gtk_box_pack_start(GTK_BOX(w), abar, FALSE, FALSE, 0);
	}

	/* input row */
	hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	panel->input = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(panel->input), _("Ask about this session…"));
	panel->send_btn = gtk_button_new_with_mnemonic(_("_Send"));
	panel->stop_btn = gtk_button_new_with_mnemonic(_("_Stop"));
	gtk_widget_set_tooltip_text(panel->stop_btn,
			_("Stop the current request (and any pending tool actions)"));
	gtk_widget_set_no_show_all(panel->stop_btn, TRUE);  /* shown per turn */
	gtk_box_pack_start(GTK_BOX(hb), panel->input, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hb), panel->stop_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hb), panel->send_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(w), hb, FALSE, FALSE, 0);

	/* option rows: checks on top, action button below (keeps the panel
	 * narrow so it steals less of the remote screen) */
	hb = gtk_grid_new();
	gtk_orientable_set_orientation(GTK_ORIENTABLE(hb),
				       GTK_ORIENTATION_HORIZONTAL);
	gtk_grid_set_row_spacing(GTK_GRID(hb), 2);
	panel->attach_check = gtk_check_button_new_with_mnemonic(_("Attach _screen"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel->attach_check), TRUE);
	panel->enter_check = gtk_check_button_new_with_mnemonic(_("Enter"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel->enter_check), TRUE);
	gtk_widget_set_tooltip_text(panel->enter_check,
			_("Press Enter after typing text into the session"));
	panel->type_btn = gtk_button_new_with_mnemonic(_("_Type reply"));
	gtk_widget_set_tooltip_text(panel->type_btn,
			_("Type the AI's last reply into the remote session"));
	panel->tools_check = gtk_check_button_new_with_mnemonic(_("Allow _control"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel->tools_check),
				     panel->tools_enabled);
	gtk_widget_set_tooltip_text(panel->tools_check,
			_("Let the AI click and type on the remote machine by itself"));
	g_signal_connect(panel->tools_check, "toggled",
			 G_CALLBACK(ai_on_tools_toggled), panel);
	gtk_grid_attach(GTK_GRID(hb), panel->attach_check, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(hb), panel->tools_check, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(hb), panel->enter_check, 2, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(hb), panel->type_btn, 0, 1, 3, 1);
	gtk_box_pack_start(GTK_BOX(w), hb, FALSE, FALSE, 0);

	/* settings expander */
	settings = gtk_expander_new(_("AI settings"));
	{
		GtkWidget *grid = gtk_grid_new();
		GtkWidget *l;

		gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
		gtk_grid_set_column_spacing(GTK_GRID(grid), 4);

		l = gtk_label_new(_("Endpoint"));
		gtk_widget_set_halign(l, GTK_ALIGN_START);
		panel->url_entry = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(panel->url_entry), panel->api_url);
		gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), panel->url_entry, 1, 0, 2, 1);

		l = gtk_label_new(_("Model"));
		gtk_widget_set_halign(l, GTK_ALIGN_START);
		store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
		panel->model_combo = gtk_combo_box_new_with_model_and_entry(GTK_TREE_MODEL(store));
		gtk_combo_box_set_entry_text_column(GTK_COMBO_BOX(panel->model_combo), 0);
		if (panel->model)
			gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(panel->model_combo))),
					   panel->model);
		gtk_grid_attach(GTK_GRID(grid), l, 0, 1, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), panel->model_combo, 1, 1, 1, 1);

		panel->status_label = gtk_label_new(NULL);
		gtk_widget_set_halign(panel->status_label, GTK_ALIGN_START);
		gtk_grid_attach(GTK_GRID(grid), panel->status_label, 1, 3, 1, 1);

		l = gtk_label_new(_("API key"));
		gtk_widget_set_halign(l, GTK_ALIGN_START);
		panel->key_entry = gtk_entry_new();
		gtk_entry_set_visibility(GTK_ENTRY(panel->key_entry), FALSE);
		if (panel->api_key)
			gtk_entry_set_text(GTK_ENTRY(panel->key_entry), panel->api_key);
		gtk_grid_attach(GTK_GRID(grid), l, 0, 2, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), panel->key_entry, 1, 2, 1, 1);

		{
			GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
			GtkWidget *refresh = gtk_button_new_with_mnemonic(_("_Reload models"));
			gtk_box_pack_start(GTK_BOX(hrow), refresh, FALSE, FALSE, 0);
			g_signal_connect(refresh, "clicked",
					 G_CALLBACK(ai_on_refresh_models), panel);
			gtk_grid_attach(GTK_GRID(grid), hrow, 1, 4, 1, 1);
		}

		/* max tool actions per turn (ai_max_tool_rounds pref) */
		l = gtk_label_new(_("Actions/turn"));
		gtk_widget_set_halign(l, GTK_ALIGN_START);
		panel->rounds_spin = gtk_spin_button_new_with_range(1, 100, 1);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(panel->rounds_spin),
					  ai_max_tool_rounds());
		gtk_widget_set_tooltip_text(panel->rounds_spin,
			_("How many tool actions (clicks, typing, screenshots) "
			  "the AI may take in one turn before it offers to "
			  "continue. Applies to new turns."));
		gtk_grid_attach(GTK_GRID(grid), l, 0, 5, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), panel->rounds_spin, 1, 5, 1, 1);

		/* dangerous-action gate (ai_confirm_risky pref) */
		panel->confirm_check =
			gtk_check_button_new_with_mnemonic(
				_("_Confirm risky actions"));
		gtk_toggle_button_set_active(
			GTK_TOGGLE_BUTTON(panel->confirm_check),
			panel->confirm_risky);
		gtk_widget_set_tooltip_text(panel->confirm_check,
			_("Ask for approval before the AI types anything "
			  "matching a dangerous pattern (rm -rf, format, "
			  "net user, download-and-run, …)"));
		gtk_grid_attach(GTK_GRID(grid), panel->confirm_check,
				0, 6, 2, 1);

		gtk_container_add(GTK_CONTAINER(settings), grid);
	}
	gtk_box_pack_start(GTK_BOX(panel->box), settings, FALSE, FALSE, 0);

	/* wire signals */
	g_signal_connect(panel->send_btn, "clicked", G_CALLBACK(ai_on_send), panel);
	g_signal_connect(panel->stop_btn, "clicked", G_CALLBACK(ai_on_stop), panel);
	g_signal_connect(panel->input, "activate", G_CALLBACK(ai_on_send), panel);
	g_signal_connect(panel->type_btn, "clicked", G_CALLBACK(ai_on_type_into_session), panel);
	g_signal_connect(panel->url_entry, "activate", G_CALLBACK(ai_on_url_activate), panel);
	g_signal_connect(panel->key_entry, "activate", G_CALLBACK(ai_on_key_activate), panel);
	g_signal_connect(panel->model_combo, "changed", G_CALLBACK(ai_on_model_changed), panel);
	g_signal_connect(panel->rounds_spin, "value-changed",
			 G_CALLBACK(ai_on_rounds_changed), panel);
	g_signal_connect(panel->confirm_check, "toggled",
			 G_CALLBACK(ai_on_confirm_toggled), panel);
	g_signal_connect(gtk_bin_get_child(GTK_BIN(panel->model_combo)), "activate",
			 G_CALLBACK(ai_on_model_entry_activate), panel);

	/* system prompt seeds the history */
	system_msg = ai_message_json("system",
		"You are an AI assistant embedded in Remmina, a remote-desktop client. "
		"The user may attach a screenshot of their current remote session "
		"(RDP/VNC) or the text of a terminal session. Help them with whatever "
		"is on the remote machine: explain errors, suggest or explain commands, "
		"read logs, troubleshoot. Be concise. When you give shell commands, "
		"put each command on its own line in a code block so the user can "
		"send it into the session.");
	g_ptr_array_add(panel->history, system_msg);

	{
		gchar *mname = ai_model_display_name(panel);
		gchar *msg = g_strdup_printf(_("%s panel ready. Ask a question — "
				"the current remote screen is attached by default."),
				mname);

		ai_chat_append(panel, NULL, "system", msg);
		g_free(msg);
		g_free(mname);
	}
	ai_refresh_models(panel);
	remmina_ai_panel_update_context(panel, FALSE);

	return panel->box;
}
