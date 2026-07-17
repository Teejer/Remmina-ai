/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2016-2023 Antenore Gatta, Giovanni Panozzo
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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * Initialize theme handling.
 *
 * Sets up monitoring of the desktop's light/dark preference through the
 * XDG desktop portal (org.freedesktop.portal.Settings). When the user has
 * enabled "follow system theme" (remmina_pref.dark_theme_auto), Remmina will
 * automatically switch between the light and dark GTK variant and keep in sync
 * with the OS whenever the desktop preference changes at runtime.
 *
 * Safe to call more than once; subsequent calls are no-ops.
 */
void remmina_theme_init(void);

/**
 * @return TRUE if the desktop currently prefers a dark color scheme, as
 * reported by the XDG desktop portal. Returns FALSE when the portal is
 * unavailable or reports no preference.
 */
gboolean remmina_theme_system_prefers_dark(void);

/**
 * Compute the effective dark-theme state and apply it to the default
 * GtkSettings ("gtk-application-prefer-dark-theme"). When dark_theme_auto is
 * enabled the value follows the desktop preference, otherwise it follows the
 * manual remmina_pref.dark_theme setting. Applying the setting restyles every
 * Remmina window immediately.
 */
void remmina_theme_apply(void);

G_END_DECLS
