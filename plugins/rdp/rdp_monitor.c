/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2016-2020 Antenore Gatta, Giovanni Panozzo
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

#include "rdp_plugin.h"
#include "rdp_monitor.h"

/** @ToDo Utility functions should be moved somewhere else */
gint remmina_rdp_utils_strpos(const gchar *haystack, const gchar *needle)
{
	TRACE_CALL(__func__);
	const gchar *sub;

	if (!*needle)
		return -1;

	sub = strstr(haystack, needle);
	if (!sub)
		return -1;

	return sub - haystack;
}

/* https://github.com/adlocode/xfwm4/blob/1d21be9ffc0fa1cea91905a07d1446c5227745f4/common/xfwm-common.c */

/**
* Setup monitor info into rdp settings, and returns maxwidth/maxheight
 */
void remmina_rdp_monitor_define (rfContext *rfi, guint32 *maxwidth, guint32 *maxheight)
{
	TRACE_CALL(__func__);
	rdpSettings* settings;

	if (!rfi || !rfi->clientContext.context.settings)
		return;
	settings = rfi->clientContext.context.settings;
	*maxwidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	*maxheight = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);

	gint n_monitors = remmina_plugin_service->plugin_multimon_monitor_count(rfi->protocol_widget);
	rdpMonitor* rdp_monitors = calloc(n_monitors + 1, sizeof(rdpMonitor));

	GdkRectangle destgeom = { 0, 0, 0, 0 };

	for (gint i = 0; i < n_monitors; ++i) {
		rdpMonitor* current = &rdp_monitors[i];
		gboolean is_primary;
		gint x, y, width, height, width_mm, height_mm;
		current->orig_screen = i;
		remmina_plugin_service->plugin_multimon_monitor_info(rfi->protocol_widget, i,
			&is_primary, &x, &y, &width, &height, &width_mm, &height_mm);
		current->x = x;
		current->y = y;
		current->is_primary = is_primary;
		current->width = width;
		current->height = height;
		current->attributes.physicalWidth = width_mm;
		current->attributes.physicalHeight = height_mm;
		destgeom.x = MIN(current->x, destgeom.x);
		destgeom.y = MIN(current->y, destgeom.y);
		destgeom.width = MAX(current->x+current->width, destgeom.width);
		destgeom.height = MAX(current->y+current->height, destgeom.height);
	}

#if FREERDP_CHECK_VERSION(3, 11, 0)
	if (!freerdp_settings_set_monitor_def_array_sorted(settings, rdp_monitors, n_monitors)) {
		// NOOP
	}
	freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount, n_monitors);
#else
	freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount, n_monitors);
#endif
	free(rdp_monitors);

	REMMINA_PLUGIN_DEBUG("%d monitors have been configured", freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount));
	*maxwidth = destgeom.x < 0 ? destgeom.width-destgeom.x: destgeom.width;
	*maxheight = destgeom.y < 0 ? destgeom.height-destgeom.y: destgeom.height;

	REMMINA_PLUGIN_DEBUG("maxw and maxh: %ux%u", *maxwidth, *maxheight);
}
