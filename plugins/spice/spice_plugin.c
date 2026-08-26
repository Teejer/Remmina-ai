/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2016-2018 Denis Ollier
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 *
 */

#include "spice_plugin.h"
#include "spice_file.h"
#include <channel-display.h>
#include <glib.h>
#include <glib-object.h>
#include <spice-channel.h>

#define XSPICE_DEFAULT_PORT 5900

enum {
	REMMINA_PLUGIN_SPICE_FEATURE_VIEWONLY = 1,
	REMMINA_PLUGIN_SPICE_FEATURE_DYNRESUPDATE,
	REMMINA_PLUGIN_SPICE_FEATURE_MULTIMON,
	REMMINA_PLUGIN_SPICE_FEATURE_PREF_DISABLECLIPBOARD,
	REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY,
	REMMINA_PLUGIN_SPICE_FEATURE_TOOL_USBREDIR,
	REMMINA_PLUGIN_SPICE_FEATURE_SCALE
};

RemminaPluginService *remmina_plugin_service = NULL;

static void remmina_plugin_spice_channel_new_cb(SpiceSession *, SpiceChannel *, RemminaProtocolWidget *);
static void remmina_plugin_spice_main_channel_event_cb(SpiceChannel *, SpiceChannelEvent, RemminaProtocolWidget *);
static void remmina_plugin_spice_agent_connected_event_cb(SpiceChannel *, RemminaProtocolWidget *);
static void remmina_plugin_spice_display_ready_cb(GObject *, GParamSpec *, RemminaProtocolWidget *);
static void remmina_plugin_spice_update_scale_mode(RemminaProtocolWidget *);
static void remmina_plugin_spice_session_open_fd(RemminaProtocolWidget *);
static void remmina_plugin_spice_channel_open_fd_cb(SpiceChannel *channel, gint tls G_GNUC_UNUSED, RemminaProtocolWidget *);
static gboolean send_key_strokes(gpointer data);

void remmina_plugin_spice_select_usb_devices(RemminaProtocolWidget *);
#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
void remmina_plugin_spice_file_transfer_new_cb(SpiceMainChannel *, SpiceFileTransferTask *, RemminaProtocolWidget *);
#  endif        /* SPICE_GTK_CHECK_VERSION(0, 31, 0) */
#endif          /* SPICE_GTK_CHECK_VERSION */

static gchar* str_replace(const gchar *string, const gchar *search, const gchar *replacement)
{
	TRACE_CALL(__func__);
	gchar *str, **arr;

	g_return_val_if_fail(string != NULL, NULL);
	g_return_val_if_fail(search != NULL, NULL);

	if (replacement == NULL)
		replacement = "";

	arr = g_strsplit(string, search, -1);
	if (arr != NULL && arr[0] != NULL)
		str = g_strjoinv(replacement, arr);
	else
		str = g_strdup(string);

	g_strfreev(arr);
	return str;
}

static void
remmina_plugin_spice_session_open_fd(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	gint fd = remmina_plugin_service->open_unix_sock(gpdata->unixPath);
	REMMINA_PLUGIN_DEBUG("Opening spice session with FD: %d -> %s", fd, gpdata->unixPath);
	spice_session_open_fd(gpdata->session, fd);
}

static void
remmina_plugin_spice_channel_open_fd_cb(SpiceChannel *channel, gint tls, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);

	gint id, type;
	g_object_get(channel, "channel-id", &id, "channel-type", &type, NULL);
	gint fd = remmina_plugin_service->open_unix_sock(gpdata->unixPath);
	REMMINA_PLUGIN_DEBUG ("Opening channel %p %s %d with FD: %d -> %s", channel, g_type_name(G_OBJECT_TYPE(channel)), id, fd, gpdata->unixPath);
	spice_channel_open_fd(channel, fd);
}

static void remmina_plugin_spice_init(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	RemminaPluginSpiceData *gpdata;
	RemminaFile *remminafile;

	gpdata = g_new0(RemminaPluginSpiceData, 1);
	g_object_set_data_full(G_OBJECT(gp), "plugin-data", gpdata, g_free);
	remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	gpdata->session = spice_session_new();
	g_signal_connect(gpdata->session,
		"channel-new",
		G_CALLBACK(remmina_plugin_spice_channel_new_cb),
		gp);
	g_object_set(gpdata->session,
		"password", g_strdup(remmina_plugin_service->file_get_string(remminafile, "password")),
		"read-only", remmina_plugin_service->file_get_int(remminafile, "viewonly", FALSE),
		"enable-audio", remmina_plugin_service->file_get_int(remminafile, "enableaudio", FALSE),
		"enable-smartcard", remmina_plugin_service->file_get_int(remminafile, "sharesmartcard", FALSE),
		"shared-dir", remmina_plugin_service->file_get_string(remminafile, "sharefolder"),
		"proxy", remmina_plugin_service->file_get_string(remminafile, "proxy"),
		NULL);       
	
	gpdata->gtk_session = spice_gtk_session_get(gpdata->session);
	g_object_set(gpdata->gtk_session,
		"auto-clipboard",
		!remmina_plugin_service->file_get_int(remminafile, "disableclipboard", FALSE),
		NULL);

	const gchar *filterstr = remmina_plugin_service->file_get_string(remminafile, "usbredir");
	if (filterstr) {
		gpdata->usbmanager = spice_usb_device_manager_get(gpdata->session, NULL);
		if (gpdata->usbmanager != NULL) {
			g_object_set(gpdata->usbmanager, "redirect-on-connect", filterstr, NULL);
		}
	}	
}

static gboolean remmina_plugin_spice_open_connection(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	gint port;
	const gchar *cacert;
	gchar *host, *tunnel;
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	/* Setup SSH tunnel if needed */
	tunnel = remmina_plugin_service->protocol_plugin_start_direct_tunnel(gp, XSPICE_DEFAULT_PORT, FALSE);
	if (!tunnel) {
		return FALSE;
	}

	/**-START- UNIX socket */
	if(strstr(tunnel, "unix:///") != NULL) {
		REMMINA_PLUGIN_DEBUG("Tunnel contain unix:// -> %s", tunnel);
		gchar *val = str_replace(tunnel, "unix://", "");
		REMMINA_PLUGIN_DEBUG("tunnel after cleaning = %s", val);
		g_object_set(gpdata->session, "unix-path", val, NULL);
		gpdata->isUnix = TRUE;
		gint fd = remmina_plugin_service->open_unix_sock(val);
		REMMINA_PLUGIN_DEBUG("Unix socket fd: %d", fd);
		gpdata->unixPath = g_strdup(val);
		if (fd > 0)
			remmina_plugin_spice_session_open_fd(gp);
		g_free(val);
	} else {

		remmina_plugin_service->get_server_port(tunnel,
				XSPICE_DEFAULT_PORT,
				&host,
				&port);

		g_object_set(gpdata->session, "host", host, NULL);
		gpdata->isUnix = FALSE;
		g_free(host);
		g_free(tunnel);

		gchar* s_port = g_strdup_printf("%i", port);

		/* Unencrypted connection */
		if (!remmina_plugin_service->file_get_int(remminafile, "usetls", FALSE)) {		
			g_object_set(gpdata->session, "port", s_port, NULL);
		}
		/* TLS encrypted connection */
		else{
			g_object_set(gpdata->session, "tls_port", s_port, NULL);

			/* Server CA certificate */
			cacert = remmina_plugin_service->file_get_string(remminafile, "cacert");
			if (cacert) {
				g_object_set(gpdata->session, "ca-file", cacert, NULL);
			}
		}
		g_free(s_port);
		s_port = NULL;

		spice_session_connect(gpdata->session);
	}
	/** -END- UNIX socket */
	/*
	 * FIXME: Add a waiting loop until the g_signal "channel-event" occurs.
	 * If the event is SPICE_CHANNEL_OPENED, TRUE should be returned,
	 * otherwise FALSE should be returned.
	 */
	return TRUE;
}

static gboolean remmina_plugin_spice_close_connection(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);

	if(gpdata->keys_queue) {
		g_async_queue_unref(gpdata->keys_queue);
	}

	if (gpdata->main_channel) {
		g_signal_handlers_disconnect_by_func(gpdata->main_channel,
			G_CALLBACK(remmina_plugin_spice_main_channel_event_cb),
			gp);
		g_signal_handlers_disconnect_by_func(gpdata->main_channel,
			G_CALLBACK(remmina_plugin_spice_agent_connected_event_cb),
			gp);
	}

	if (gpdata->session) {
		spice_session_disconnect(gpdata->session);
		g_object_unref(gpdata->session);
		gpdata->session = NULL;
		remmina_plugin_service->protocol_plugin_signal_connection_closed(gp);
	}

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
	if (gpdata->file_transfers) {
		g_hash_table_unref(gpdata->file_transfers);
	}
#  endif        /* SPICE_GTK_CHECK_VERSION(0, 31, 0) */
#endif          /* SPICE_GTK_CHECK_VERSION */

	return FALSE;
}

static gboolean remmina_plugin_spice_disable_gst_overlay(SpiceChannel *channel, void* pipeline_ptr, RemminaProtocolWidget *gp)
{
	g_signal_stop_emission_by_name(channel, "gst-video-overlay");
	return FALSE;
}

static gint mon_index_for_channel(guint id, gint main_index)
{
	if (id == 0)
		return main_index;
	if (id<=main_index)
		return id-1;
	return id;
}

static void update_display_of_channel(SpiceChannel *channel, RemminaProtocolWidget *gp, gint id)
{
	TRACE_CALL(__func__);
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	SpiceDisplay*display = NULL;
	
	if (gpdata->display == NULL && id == 0 && !gpdata->is_multimonitor) {
		REMMINA_PLUGIN_DEBUG("Creating new display for unique channel %d", id);
		gpdata->display = spice_display_new(gpdata->session, id);
		if (gpdata->display_channel == NULL)
			gpdata->display_channel = SPICE_DISPLAY_CHANNEL(channel);
		display = gpdata->display;
	}

	if (gpdata->is_multimonitor)  {				
		// main must be populated everytime		
		gint main_index = remmina_plugin_service->plugin_multimon_main_monitor_index(gp);			
		gint index = mon_index_for_channel(id, main_index);
		if (index >= remmina_plugin_service->plugin_multimon_monitor_count(gp)) {
			REMMINA_PLUGIN_DEBUG("There is no physical monitor for channel %d", id);
			return;
		}
		gboolean is_main = (gpdata->display == NULL || id == 0);
		if (gpdata->display != NULL && id == 0) {
			gint prev_main_id, prev_main_index;
			g_object_get(gpdata->display_channel, "channel-id", &prev_main_id, NULL);			
			prev_main_index = mon_index_for_channel(prev_main_id, main_index);
			if (main_index != prev_main_index) {
				REMMINA_PLUGIN_DEBUG("Channel %d no longer main monitor, now %d", prev_main_id, prev_main_index);
				remmina_plugin_service->plugin_multimon_monitor_set_drawing_area(gp, prev_main_index, GTK_WIDGET(gpdata->display));				
			} else {
				is_main = FALSE;	// main is already on right widget
			}
		}
		if (is_main) {
			REMMINA_PLUGIN_DEBUG("Creating new display for main channel %d, monitor %d", id, index);
			display = spice_display_new(gpdata->session, id);
			gpdata->display_channel = SPICE_DISPLAY_CHANNEL(channel);
			gpdata->display = display;
		} else {
			if (id >= remmina_plugin_service->plugin_multimon_monitor_count(gp)) {
				REMMINA_PLUGIN_DEBUG("No monitor available for channel %d", id);
				return;
			}
			REMMINA_PLUGIN_DEBUG("Creating new display for secondary channel=%d, monitor=%d", id, index);
			display = spice_display_new(gpdata->session, id);
			remmina_plugin_service->plugin_multimon_monitor_set_drawing_area(gp, index, GTK_WIDGET(display));
		}
	}
	if (display != NULL) {
		g_signal_connect(display, "notify::ready", G_CALLBACK(remmina_plugin_spice_display_ready_cb), gp);
		remmina_plugin_spice_display_ready_cb(G_OBJECT(display), NULL, gp);
	}
}

static void remmina_plugin_spice_channel_new_cb(SpiceSession *session, SpiceChannel *channel, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	gint id, type;
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	g_return_if_fail(gpdata != NULL);

	if(gpdata->isUnix) {
		g_signal_connect(channel, "open-fd", G_CALLBACK(remmina_plugin_spice_channel_open_fd_cb), gp);
	}

	g_object_get(channel, "channel-id", &id, "channel-type", &type, NULL);
	REMMINA_PLUGIN_DEBUG ("New spice channel %p %s %d", channel, g_type_name(G_OBJECT_TYPE(channel)), id);

	if (SPICE_IS_MAIN_CHANNEL(channel)) {
		gpdata->main_channel = SPICE_MAIN_CHANNEL(channel);
		g_signal_connect(channel,
			"channel-event",
			G_CALLBACK(remmina_plugin_spice_main_channel_event_cb),
			gp);
		g_signal_connect(channel,
			"main-agent-update",
			G_CALLBACK(remmina_plugin_spice_agent_connected_event_cb),
			gp);
#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
		g_signal_connect(channel,
			"new-file-transfer",
			G_CALLBACK(remmina_plugin_spice_file_transfer_new_cb),
			gp);
#  endif        /* SPICE_GTK_CHECK_VERSION(0, 31, 0) */
#endif          /* SPICE_GTK_CHECK_VERSION */
	}

	if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
		update_display_of_channel(channel, gp, id);

		if (remmina_plugin_service->file_get_int(remminafile, "disablegstvideooverlay", FALSE)) {
			g_signal_connect(channel,
				"gst-video-overlay",
				G_CALLBACK(remmina_plugin_spice_disable_gst_overlay),
				gp);
		}
	}

	if (SPICE_IS_INPUTS_CHANNEL(channel)) {
		REMMINA_PLUGIN_DEBUG("New inputs channel");
	}
	if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
		REMMINA_PLUGIN_DEBUG("New audio channel");
		if (remmina_plugin_service->file_get_int(remminafile, "enableaudio", FALSE)) {
			gpdata->audio = spice_audio_get(gpdata->session, NULL);
		}
	}

	if (SPICE_IS_USBREDIR_CHANNEL(channel)) {
		REMMINA_PLUGIN_DEBUG("New usbredir channel");
	}

	if (SPICE_IS_WEBDAV_CHANNEL(channel)) {
		REMMINA_PLUGIN_DEBUG("New webdav channel");
		if (remmina_plugin_service->file_get_string(remminafile, "sharefolder")) {
			spice_channel_connect(channel);
		}
	}
}

static gboolean remmina_plugin_spice_ask_auth(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	gint ret;
	gboolean disablepasswordstoring;
	gchar *s_password;
	gboolean save;

	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	disablepasswordstoring = remmina_plugin_service->file_get_int(remminafile, "disablepasswordstoring", FALSE);

	ret = remmina_plugin_service->protocol_plugin_init_auth(gp,
		(disablepasswordstoring ? 0 : REMMINA_MESSAGE_PANEL_FLAG_SAVEPASSWORD),
		_("Enter SPICE password"),
		NULL,
		remmina_plugin_service->file_get_string(remminafile, "password"),
		NULL,
		NULL);
	if (ret == GTK_RESPONSE_OK) {
		s_password = remmina_plugin_service->protocol_plugin_init_get_password(gp);
		save = remmina_plugin_service->protocol_plugin_init_get_savepassword(gp);
		if (save) {
			remmina_plugin_service->file_set_string(remminafile, "password", s_password);
		} else {
			remmina_plugin_service->file_set_string(remminafile, "password", NULL);
		}
	} else {
		return FALSE;
	}

	g_object_set(gpdata->session, "password", s_password, NULL);
	return TRUE;
}

static void remmina_plugin_spice_send_monitors_info(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);

	gpdata->is_multimonitor = remmina_plugin_service->file_get_int(remmina_plugin_service->protocol_plugin_get_file(gp), "multimon", FALSE) == 1;
	
	gint n_monitors = remmina_plugin_service->plugin_multimon_monitor_count(gp);
	if (n_monitors == 0) {
		gpdata->is_multimonitor = 0;
		return;
	}
	
	gint main_index = remmina_plugin_service->plugin_multimon_main_monitor_index(gp);
	// Send monitors information, with main monitor moved to index 0
	for (gint i=0;i<n_monitors;i++) {
		gint x, y, width, height, width_mm, height_mm;
		gint index = mon_index_for_channel(i, main_index);
		gboolean isEnabled = (gpdata->is_multimonitor || i == 0);
		if (isEnabled) {
			if (!remmina_plugin_service->plugin_multimon_monitor_info(gp, index, NULL, &x, &y, &width, &height, &width_mm, &height_mm))
				continue;
			REMMINA_PLUGIN_DEBUG("sending monitor info#%d: %d,%d %dx%d", i , x, y, width, height);
			spice_main_channel_update_display_mm(SPICE_MAIN_CHANNEL(gpdata->main_channel), i, width_mm, height_mm, FALSE);
			spice_main_channel_update_display(SPICE_MAIN_CHANNEL(gpdata->main_channel), i, x, y, width, height, FALSE);
		} else {
			REMMINA_PLUGIN_DEBUG("not sending monitor info#%d", i);
		}
	}
	spice_main_channel_send_monitor_config(SPICE_MAIN_CHANNEL(gpdata->main_channel));
	if (gpdata->display == NULL)
		return;
	
	GList*channels_head = spice_session_get_channels(gpdata->session);
	GList*channels = channels_head;
	while (channels != NULL) {
		SpiceChannel*c = SPICE_CHANNEL(channels->data);
		if (SPICE_IS_DISPLAY_CHANNEL(c)) {
			SpiceDisplayChannel*dsp = SPICE_DISPLAY_CHANNEL(c);
			gint id;
			g_object_get(c, "channel-id", &id, NULL);
			if (dsp != gpdata->display_channel) {
				if (gpdata->is_multimonitor) {
					update_display_of_channel(c, gp, id);
				} else {
					remmina_plugin_service->plugin_multimon_monitor_set_drawing_area(gp, id, NULL);
				}
			}
		}		
		channels = g_list_next(channels);
	}
	g_list_free(channels_head);
}

static void remmina_plugin_spice_main_channel_event_cb(SpiceChannel *channel, SpiceChannelEvent event, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	gchar *serverOption = g_strdup(remmina_plugin_service->file_get_string(remminafile, "server"));
	gchar *message = NULL;
	gchar *server = NULL;

	if(gpdata->isUnix) {
		gchar *val = str_replace(serverOption, "unix://", "");
		message = g_strdup_printf("Unix socket server %s", val);
		g_free(val), val = NULL;
	} else {
		gint port;
		RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
		remmina_plugin_service->get_server_port(remmina_plugin_service->file_get_string(remminafile, "server"),
			XSPICE_DEFAULT_PORT,
			&server,
			&port);
		message = g_strdup_printf("TCP server %s:%d", server, port);
	}

	switch (event) {
	case SPICE_CHANNEL_CLOSED:
		remmina_plugin_service->protocol_plugin_set_error(gp, _("Disconnected from the SPICE %s."), message);
		remmina_plugin_spice_close_connection(gp);
		REMMINA_PLUGIN_AUDIT(_("Disconnected from %s via SPICE"), message);
		break;
	case SPICE_CHANNEL_OPENED:
		REMMINA_PLUGIN_AUDIT(_("Connected to %s via SPICE"), message);
		break;
	case SPICE_CHANNEL_ERROR_AUTH:
		if (remmina_plugin_spice_ask_auth(gp)) {
			remmina_plugin_spice_open_connection(gp);
		}else{
			/* Connection is cancelled by the user by clicking cancel on auth panel, close it without showing errors */
			remmina_plugin_spice_close_connection(gp);
		}
		break;
	case SPICE_CHANNEL_ERROR_TLS:
		remmina_plugin_service->protocol_plugin_set_error(gp, _("TLS connection error."));
		remmina_plugin_spice_close_connection(gp);
		break;
	case SPICE_CHANNEL_ERROR_IO:
	case SPICE_CHANNEL_ERROR_LINK:
	case SPICE_CHANNEL_ERROR_CONNECT:
		remmina_plugin_service->protocol_plugin_set_error(gp, _("Connection to the SPICE server dropped."));
		remmina_plugin_spice_close_connection(gp);
		break;
	default:
		break;
	}
	g_free(server), server = NULL;
	g_free(message), message = NULL;
	g_free(serverOption), message = NULL;
}

void remmina_plugin_spice_agent_connected_event_cb(SpiceChannel *channel, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	gboolean connected;

	g_object_get(channel,
		"agent-connected", &connected,
		NULL);

	if (connected) {
		remmina_plugin_spice_send_monitors_info(gp);
		remmina_plugin_service->protocol_plugin_unlock_dynres(gp);
	} else {
		remmina_plugin_service->protocol_plugin_lock_dynres(gp);
	}
}

static void remmina_plugin_spice_display_ready_cb(GObject *display, GParamSpec *param_spec, RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	gboolean ready;

	g_object_get(display, "ready", &ready, NULL);

	if (ready) {
		RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
		RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

		g_signal_handlers_disconnect_by_func(display,
			G_CALLBACK(remmina_plugin_spice_display_ready_cb),
			gp);

		RemminaScaleMode scaleMode = remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp);
		g_object_set(display,
			"scaling", (scaleMode  == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_SCALED),
			"resize-guest", (scaleMode == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_DYNRES),
			NULL);

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 34, 0)
		SpiceVideoCodecType videocodec = remmina_plugin_service->file_get_int(remminafile, "videocodec", 0);
		if (videocodec) {
#    if SPICE_GTK_CHECK_VERSION(0, 38, 0)
			GError *err = NULL;
			guint i;

			GArray *preferred_codecs = g_array_sized_new(FALSE, FALSE,
				sizeof(gint),
				(SPICE_VIDEO_CODEC_TYPE_ENUM_END - 1));

			g_array_append_val(preferred_codecs, videocodec);
			for (i = SPICE_VIDEO_CODEC_TYPE_MJPEG; i < SPICE_VIDEO_CODEC_TYPE_ENUM_END; ++i) {
				if (i != videocodec) {
					g_array_append_val(preferred_codecs, i);
				}
			}

			if (!spice_display_channel_change_preferred_video_codec_types(SPICE_CHANNEL(gpdata->display_channel),
					(gint *) preferred_codecs->data,
					preferred_codecs->len,
					&err)) {
				REMMINA_PLUGIN_DEBUG("Could not set video-codec preference. %s", err->message);
				g_error_free(err);
			}

			g_clear_pointer(&preferred_codecs, g_array_unref);

#    elif SPICE_GTK_CHECK_VERSION(0, 35, 0)
			spice_display_channel_change_preferred_video_codec_type(SPICE_CHANNEL(gpdata->display_channel),
					videocodec);
#    else
			spice_display_change_preferred_video_codec_type(SPICE_CHANNEL(gpdata->display_channel),
					videocodec);
#    endif
		}
#  endif
#endif

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
		SpiceImageCompression imagecompression = remmina_plugin_service->file_get_int(remminafile, "imagecompression", 0);
		if (imagecompression) {
#    if SPICE_GTK_CHECK_VERSION(0, 35, 0)
			spice_display_channel_change_preferred_compression(SPICE_CHANNEL(gpdata->display_channel),
				imagecompression);
#    else
			spice_display_change_preferred_compression(SPICE_CHANNEL(gpdata->display_channel),
				imagecompression);
#    endif
		}
#  endif
#endif

		if (GTK_WIDGET(display) == GTK_WIDGET(gpdata->display))
			gtk_container_add(GTK_CONTAINER(gp), GTK_WIDGET(display));
		gtk_widget_show(GTK_WIDGET(display));

		remmina_plugin_service->protocol_plugin_register_hostkey(gp, GTK_WIDGET(display));
		remmina_plugin_service->protocol_plugin_signal_connection_opened(gp);
	}
}

/* Send a keystroke to the plugin window */
static void remmina_plugin_spice_keystroke(RemminaProtocolWidget *gp, const guint keystrokes[], const gint keylen)
{
	TRACE_CALL(__func__);
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);

	g_return_if_fail(gpdata->display != NULL);

	if(gpdata->keys_queue == NULL) {
		gpdata->keys_queue = g_async_queue_new();
		gpdata->is_sending_keys = FALSE;
	}

	KeyStrokeData *key_stroke_data = g_malloc(sizeof(KeyStrokeData));
	key_stroke_data->keylen = keylen;
	key_stroke_data->keystrokes = g_malloc(keylen * sizeof(guint));
	memcpy(key_stroke_data->keystrokes, keystrokes, keylen * sizeof(guint));
	g_async_queue_push(gpdata->keys_queue, key_stroke_data);

	if (!gpdata->is_sending_keys) {
		gpdata->is_sending_keys = TRUE;
		g_idle_add(send_key_strokes, (gpointer)gpdata);
	}
}

static gboolean send_key_strokes(gpointer data) {
	RemminaPluginSpiceData *gpdata = (RemminaPluginSpiceData *)data;
	if (g_async_queue_length(gpdata->keys_queue) == 0) {
		gpdata->is_sending_keys = FALSE;
		return FALSE;
	}

	// Let's be nice here and wait a bit in order to avoid hammering SPICE
	g_usleep(25000);

	KeyStrokeData *key_stroke_data = g_async_queue_pop(gpdata->keys_queue);
	spice_display_send_keys(gpdata->display, key_stroke_data->keystrokes, key_stroke_data->keylen, SPICE_DISPLAY_KEY_EVENT_CLICK);
	g_free(key_stroke_data->keystrokes);
	g_free(key_stroke_data);

	return TRUE;
}

/* Send CTRL+ALT+DEL keys keystrokes to the plugin socket widget */
static void remmina_plugin_spice_send_ctrl_alt_key(RemminaProtocolWidget *gp, gint combKey1, gint combKey2)
{
	TRACE_CALL(__func__);

	guint keys[] = { GDK_KEY_Control_L, GDK_KEY_Alt_L, combKey1, combKey2 };
	gint nelements = G_N_ELEMENTS(keys);
	if (combKey2 == 0)
		nelements--;
	remmina_plugin_spice_keystroke(gp, keys, nelements);
}

static void remmina_plugin_spice_update_scale_mode(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	gint width, height;
	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaScaleMode scaleMode = remmina_plugin_service->remmina_protocol_widget_get_current_scale_mode(gp);

	g_object_set(gpdata->display,
		"scaling", (scaleMode  == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_SCALED),
		"resize-guest", (scaleMode == REMMINA_PROTOCOL_WIDGET_SCALE_MODE_DYNRES),
		NULL);

	if (scaleMode != REMMINA_PROTOCOL_WIDGET_SCALE_MODE_NONE) {
		/* In scaled mode, the SpiceDisplay will get its dimensions from its parent */
		gtk_widget_set_size_request(GTK_WIDGET(gpdata->display), -1, -1 );
	}else {
		/* In non scaled mode, the plugins forces dimensions of the SpiceDisplay */
		g_object_get(gpdata->display_channel,
			"width", &width,
			"height", &height,
			NULL);
		gtk_widget_set_size_request(GTK_WIDGET(gpdata->display), width, height);
	}
}

static gboolean remmina_plugin_spice_query_feature(RemminaProtocolWidget *gp, const RemminaProtocolFeature *feature)
{
	TRACE_CALL(__func__);

	return TRUE;
}

static void remmina_plugin_spice_call_feature(RemminaProtocolWidget *gp, const RemminaProtocolFeature *feature)
{
	TRACE_CALL(__func__);

	RemminaPluginSpiceData *gpdata = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	gboolean connected;

	switch (feature->id) {
	case REMMINA_PLUGIN_SPICE_FEATURE_VIEWONLY:
		g_object_set(gpdata->session,
			"read-only",
			remmina_plugin_service->file_get_int(remminafile, "viewonly", FALSE),
			NULL);
		break;
	case REMMINA_PLUGIN_SPICE_FEATURE_PREF_DISABLECLIPBOARD:
		g_object_set(gpdata->gtk_session,
			"auto-clipboard",
			!remmina_plugin_service->file_get_int(remminafile, "disableclipboard", FALSE),
			NULL);
		break;
	case REMMINA_PLUGIN_SPICE_FEATURE_DYNRESUPDATE:
	case REMMINA_PLUGIN_SPICE_FEATURE_SCALE:
		remmina_plugin_spice_update_scale_mode(gp);
		break;
	case REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY:
		remmina_plugin_spice_send_ctrl_alt_key(gp, GPOINTER_TO_INT(feature->opt2), GPOINTER_TO_INT(feature->opt3));
		break;
	case REMMINA_PLUGIN_SPICE_FEATURE_TOOL_USBREDIR:
		remmina_plugin_spice_select_usb_devices(gp);
		break;
	case REMMINA_PLUGIN_SPICE_FEATURE_MULTIMON:
		g_object_get(gpdata->main_channel,
			"agent-connected", &connected, NULL);
		if (connected) {
			remmina_plugin_spice_send_monitors_info(gp);
		}
		break;
	default:
		break;
	}
}

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 34, 0)
/* Array of key/value pairs for preferred video codec
 * Key - SpiceVideoCodecType (spice/enums.h)
 */
static gpointer videocodec_list[] =
{
	"0",	N_("Default"),
	"1",	"mjpeg",
	"2",	"vp8",
	"3",	"h264",
	"4",	"vp9",
	"5",	"h265",
	NULL
};
#  endif
#endif

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
/* Array of key/value pairs for preferred video codec
 * Key - SpiceImageCompression (spice/enums.h)
 */
static gpointer imagecompression_list[] =
{
	"0",	N_("Default"),
	"1",	N_("Off"),
	"2",	N_("Auto GLZ"),
	"3",	N_("Auto LZ"),
	"4",	"Quic",
	"5",	"GLZ",
	"6",	"LZ",
	"7",	"LZ4",
	NULL
};
#  endif
#endif

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 34, 0)
static gchar disablegstvideooverlay_tooltip[] =
	N_("Disable video overlay if videos are not displayed properly.\n");
#  endif
#endif

static gchar monitorids_tooltip[] =
        N_("Comma-separated list of monitors using index, model name, or manufacturer code:\n"
           "  • Index (0-based): 1,0,2\n"
           "  • Manufacturer: LEN,BOE,\n"
           "  • Model name: Model abc,BOE\n"
           "  • Mixed: 1,LEN,Model abc\n"
           "\n"
           "• First entry = main monitor (login box, toolbar)\n"
           "• Monitors must be contiguous - cannot skip monitors in the layout\n"
           "\n");

/* Array of RemminaProtocolSetting for basic settings.
 * Each item is composed by:
 * a) RemminaProtocolSettingType for setting type
 * b) Setting name
 * c) Setting description
 * d) Compact disposition
 * e) Values for REMMINA_PROTOCOL_SETTING_TYPE_SELECT or REMMINA_PROTOCOL_SETTING_TYPE_COMBO
 * f) Setting tooltip
 * g) Validation data pointer, will be passed to the validation callback method.
 * h) Validation callback method (Can be NULL. Every entry will be valid then.)
 *		use following prototype:
 *		gboolean mysetting_validator_method(gpointer key, gpointer value,
 *						    gpointer validator_data);
 *		gpointer key is a gchar* containing the setting's name,
 *		gpointer value contains the value which should be validated,
 *		gpointer validator_data contains your passed data.
 */
static const RemminaProtocolSetting remmina_plugin_spice_basic_settings[] =
{
	{ REMMINA_PROTOCOL_SETTING_TYPE_SERVER,	  "server",		 NULL,				           FALSE, NULL, NULL, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_PASSWORD, "password",	 N_("User password"),	 	   FALSE, NULL, NULL, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	  "usetls",		 N_("Use TLS encryption"),	   FALSE, NULL, NULL, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_FILE,	  "cacert",		 N_("Server CA certificate"),  FALSE, NULL, NULL, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	    "multimon",			N_("Enable multi monitor"),		  FALSE,	 NULL,		  NULL,										NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_MONITOR_LIST,	    "monitorids",		N_("List monitor IDs"),			  FALSE, NULL,		  monitorids_tooltip,								NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_TEXT,	  "sharefolder", N_("Share folder"),		   FALSE, NULL, NULL, NULL, NULL },
    { REMMINA_PROTOCOL_SETTING_TYPE_TEXT,     "proxy",       N_("Proxy"),                  FALSE, NULL, NULL, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_TEXT,	  "usbredir",	 N_("USB device redirection"), FALSE, NULL, NULL, NULL, NULL },
	{ REMMINA_PROTOCOL_SETTING_TYPE_END,	  NULL,			 NULL,				           FALSE, NULL, NULL, NULL, NULL }
};

/* Array of RemminaProtocolSetting for advanced settings.
 * Each item is composed by:
 * a) RemminaProtocolSettingType for setting type
 * b) Setting name
 * c) Setting description
 * d) Compact disposition
 * e) Values for REMMINA_PROTOCOL_SETTING_TYPE_SELECT or REMMINA_PROTOCOL_SETTING_TYPE_COMBO
 * f) Setting Tooltip
 */
static const RemminaProtocolSetting remmina_plugin_spice_advanced_settings[] =
{
#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 35, 0)
	{ REMMINA_PROTOCOL_SETTING_TYPE_SELECT,	"videocodec",	          N_("Preferred video codec"),		 FALSE, videocodec_list,       NULL},
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	"disablegstvideooverlay", N_("Turn off GStreamer overlay"),	 FALSE,	NULL,	               disablegstvideooverlay_tooltip},
#  endif
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
	{ REMMINA_PROTOCOL_SETTING_TYPE_SELECT,	"imagecompression",	      N_("Preferred image compression"), FALSE, imagecompression_list, NULL},
#  endif
#endif
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	"disableclipboard",	      N_("No clipboard sync"),		     TRUE,	NULL,	               NULL},
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	"disablepasswordstoring", N_("Forget passwords after use"),	 TRUE,	NULL,	               NULL},
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	"enableaudio",		      N_("Enable audio channel"),		 TRUE,	NULL,	               NULL},
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	"sharesmartcard",	      N_("Share smart card"),			 TRUE,	NULL,	               NULL},
	{ REMMINA_PROTOCOL_SETTING_TYPE_CHECK,	"viewonly",		          N_("View only"),				     TRUE,	NULL,	               NULL},
	{ REMMINA_PROTOCOL_SETTING_TYPE_END,	NULL,			          NULL,					             TRUE,	NULL,	               NULL}
};

/* Array for available features.
 * The last element of the array must be REMMINA_PROTOCOL_FEATURE_TYPE_END. */
static const RemminaProtocolFeature remmina_plugin_spice_features[] =
{
	{ REMMINA_PROTOCOL_FEATURE_TYPE_VIEWONLY,     REMMINA_PLUGIN_SPICE_FEATURE_VIEWONLY,	          GINT_TO_POINTER(REMMINA_PROTOCOL_FEATURE_PREF_CHECK), "viewonly",	        N_("View only")},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_PREF,         REMMINA_PLUGIN_SPICE_FEATURE_PREF_DISABLECLIPBOARD, GINT_TO_POINTER(REMMINA_PROTOCOL_FEATURE_PREF_CHECK),	"disableclipboard",	N_("No clipboard sync")},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+Delete"),					       GINT_TO_POINTER(GDK_KEY_Delete),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+Backspace"),				       GINT_TO_POINTER(GDK_KEY_BackSpace),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Shift+Ctrl+Alt+Escape"),				       GINT_TO_POINTER(GDK_KEY_Shift_L),GINT_TO_POINTER(GDK_KEY_Escape)},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F1"),					        GINT_TO_POINTER(GDK_KEY_F1),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F2"),					        GINT_TO_POINTER(GDK_KEY_F2),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F3"),					        GINT_TO_POINTER(GDK_KEY_F3),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F4"),					        GINT_TO_POINTER(GDK_KEY_F4),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F5"),					        GINT_TO_POINTER(GDK_KEY_F5),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F6"),					        GINT_TO_POINTER(GDK_KEY_F6),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F7"),					        GINT_TO_POINTER(GDK_KEY_F7),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F8"),					        GINT_TO_POINTER(GDK_KEY_F8),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F9"),					        GINT_TO_POINTER(GDK_KEY_F9),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F10"),					        GINT_TO_POINTER(GDK_KEY_F10),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F11"),					        GINT_TO_POINTER(GDK_KEY_F11),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_SEND_CTRL_ALT_KEY, N_("Send Ctrl+Alt+F12"),					        GINT_TO_POINTER(GDK_KEY_F12),		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_TOOL,         REMMINA_PLUGIN_SPICE_FEATURE_TOOL_USBREDIR,	      N_("Select USB devices for redirection"),			    NULL,		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_MULTIMON,     REMMINA_PLUGIN_SPICE_FEATURE_MULTIMON,	           NULL,			                                     NULL,       NULL },	
	{ REMMINA_PROTOCOL_FEATURE_TYPE_DYNRESUPDATE, REMMINA_PLUGIN_SPICE_FEATURE_DYNRESUPDATE,	      NULL,	                                                NULL,	            NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_SCALE,        REMMINA_PLUGIN_SPICE_FEATURE_SCALE,		          NULL,							                        NULL,		        NULL},
	{ REMMINA_PROTOCOL_FEATURE_TYPE_END,          0,						                          NULL,							                        NULL,		        NULL}
};


static RemminaProtocolPlugin remmina_plugin_spice =
{
	REMMINA_PLUGIN_TYPE_PROTOCOL,                                           // Type
	"SPICE",                                                                // Name
	N_("SPICE - Simple Protocol for Independent Computing Environments"),   // Description
	GETTEXT_PACKAGE,                                                        // Translation domain
	VERSION,                                                                // Version number
	"org.remmina.Remmina-spice-symbolic",                                   // Icon for normal connection
	"org.remmina.Remmina-spice-ssh-symbolic",                               // Icon for SSH connection
	remmina_plugin_spice_basic_settings,                                    // Array for basic settings
	remmina_plugin_spice_advanced_settings,                                 // Array for advanced settings
	REMMINA_PROTOCOL_SSH_SETTING_TUNNEL,                                    // SSH settings type
	remmina_plugin_spice_features,                                          // Array for available features
	remmina_plugin_spice_init,                                              // Plugin initialization
	remmina_plugin_spice_open_connection,                                   // Plugin open connection
	remmina_plugin_spice_close_connection,                                  // Plugin close connection
	remmina_plugin_spice_query_feature,                                     // Query for available features
	remmina_plugin_spice_call_feature,                                      // Call a feature
	remmina_plugin_spice_keystroke,                                         // Send a keystroke
	NULL,                                                                   // No screenshot support available
	NULL,																	// RCW map event
	NULL                                                                    // RCW unmap event
};

/* File plugin definition and features */
static RemminaFilePlugin remmina_spicef =
{
	REMMINA_PLUGIN_TYPE_FILE,                       // Type
	"SPICEF",                                       // Name
	N_("SPICE vv file"),         					// Description
	GETTEXT_PACKAGE,                                // Translation domain
	VERSION,         					            // Version number
	remmina_spice_file_import_test,                   // Test import function
	remmina_spice_file_import,                        // Import function
	remmina_spice_file_export_test,                   // Test export function
	remmina_spice_file_export,                        // Export function
	".vv",                        // Export extension
	NULL
};

static void remmina_plugin_spice_remove_list_option(gpointer *option_list, const gchar *option_to_remove) {
	gpointer *src, *dst;

	TRACE_CALL(__func__);

	dst = src = option_list;
	while (*src) {
		if (strcmp(*src, option_to_remove) != 0) {
			if (dst != src) {
				*dst = *src;
				*(dst + 1) = *(src + 1);
			}
			dst += 2;
		}
		src += 2;
	}
	*dst = NULL;
}

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
static gboolean remmina_plugin_spice_is_lz4_supported(void) {
	gboolean result = FALSE;
	GOptionContext *context;
	GOptionGroup *spiceGroup;
	gchar *spiceHelp;

	TRACE_CALL(__func__);

	spiceGroup = spice_get_option_group();
	context = g_option_context_new("- SPICE client test application");
	g_option_context_add_group(context, spiceGroup);

	spiceHelp = g_option_context_get_help(context, FALSE, spiceGroup);
	if (g_strcmp0(spiceHelp, "") != 0) {
		gchar **spiceHelpLines, **line;
		spiceHelpLines = g_strsplit(spiceHelp, "\n", -1);

		for (line = spiceHelpLines; *line != NULL; ++line) {
			if (g_strstr_len(*line, -1, "spice-preferred-compression")) {
				if (g_strstr_len(*line, -1, ",lz4,")) {
					result = TRUE;
				}

				break;
			}
		}

		g_strfreev(spiceHelpLines);
	}
	g_option_context_free(context);
	g_free(spiceHelp);

	return result;
}
#  endif
#endif

G_MODULE_EXPORT gboolean remmina_plugin_entry(RemminaPluginService *service);

gboolean remmina_plugin_entry(RemminaPluginService *service)
{
	TRACE_CALL(__func__);
	remmina_plugin_service = service;

	bindtextdomain(GETTEXT_PACKAGE, REMMINA_RUNTIME_LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");

#ifdef SPICE_GTK_CHECK_VERSION
#  if SPICE_GTK_CHECK_VERSION(0, 31, 0)
	if (!remmina_plugin_spice_is_lz4_supported()) {
		char key_str[10];
		sprintf(key_str, "%d", SPICE_IMAGE_COMPRESSION_LZ4);
		remmina_plugin_spice_remove_list_option(imagecompression_list, key_str);
	}
#  endif
#endif

	if (!service->register_plugin((RemminaPlugin*)&remmina_plugin_spice)) {
		return FALSE;
	}

	remmina_spicef.export_hints = _("Export connection in virt-viewer .vv file format");
	remmina_spicef.export_ext = ".vv";

	if (!service->register_plugin((RemminaPlugin *)&remmina_spicef))
		return FALSE;

	return TRUE;
}

