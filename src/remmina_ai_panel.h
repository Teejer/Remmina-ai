/*
 * Remmina - The GTK+ Remote Desktop Client
 *
 * AI side panel: chat with an OpenAI-compatible LLM endpoint (default:
 * the local mlx-serve server) and let it "see" the active remote
 * connection (RDP/VNC screen capture, SSH terminal text) and type text
 * into it.
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

#pragma once

#include "remmina_protocol_widget.h"
#include "rcw.h"

G_BEGIN_DECLS

typedef struct _RemminaAiPanel RemminaAiPanel;

/* Create an AI panel bound to one session (one panel per open tab).
 * proto is the session's RemminaProtocolWidget (may be pre-connect).
 * The returned widget is owned by the caller (packed into the window grid). */
GtkWidget *remmina_ai_panel_new(RemminaConnectionWindow *cnnwin, GtkWidget *proto);

/* Refresh the "active connection" label / button sensitivity.
 * connected is whether that session is currently connected.
 * Call when the connection state changes. */
void remmina_ai_panel_update_context(RemminaAiPanel *panel, gboolean connected);
void remmina_ai_panel_update(GtkWidget *panel_widget, gboolean connected);

/* The panel bound to a session's protocol widget, or NULL */
GtkWidget *remmina_ai_panel_for_protocol_widget(GtkWidget *proto);

G_END_DECLS
