/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2009-2011 Vic Lee
 * Copyright (C) 2014-2015 Antenore Gatta, Fabio Castelli, Giovanni Panozzo
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

 #include "config.h"
 #include <gdk/gdk.h>

 #include "remmina_log.h"

#ifdef GDK_WINDOWING_WAYLAND
#ifdef HAVE_KDE_OUTPUT_ORDER_V1
#include "kde-output-order-v1-client.h"
#endif
#endif

#if defined(HAVE_KDE_OUTPUT_ORDER_V1)
/** KDE specifics: probe the primary monitor */
static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;

typedef struct wl_output_name_map {
	const char*name;
	GdkRectangle geometry;
} wl_output_name_map;

typedef struct wl_kde_primary_probe {
	const char*first_kde_wayland_output_name;
	GdkRectangle first_kde_wayland_geometry;
	const char*current_kde_wayland_output_name;
	GdkRectangle current_kde_wayland_geometry;
	GPtrArray*wl_monitor_output_names;
} wl_kde_primary_probe;

static void  kde_output_order_item(void *data,
		       struct kde_output_order_v1 *kde_output_order_v1,
		       const char *output_name)
{
	struct wl_kde_primary_probe *priv = (wl_kde_primary_probe*)data;
	if (priv->first_kde_wayland_output_name == NULL) {
		REMMINA_DEBUG("first (primary) in ordered monitors=%s", output_name);
		priv->first_kde_wayland_output_name = g_strdup(output_name);
	}
}

static void kde_output_order_done(void *data,
		     struct kde_output_order_v1 *kde_output_order_v1) {
}

static const struct kde_output_order_v1_listener kde_output_order_listeners = {
	.output = kde_output_order_item,
	.done = kde_output_order_done
};
static void remmina_wl_output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	struct wl_kde_primary_probe *priv = (wl_kde_primary_probe*)data;
	priv->current_kde_wayland_geometry.x = x;
	priv->current_kde_wayland_geometry.y = y;
}

static void remmina_wl_output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
	struct wl_kde_primary_probe *priv = (wl_kde_primary_probe*)data;
	priv->current_kde_wayland_geometry.height = height;
	priv->current_kde_wayland_geometry.width = width;
}
static void remmina_wl_output_done(void *data, struct wl_output *wl_output)
{
	struct wl_kde_primary_probe *priv = (wl_kde_primary_probe*)data;
	struct wl_output_name_map* new_item = g_new (struct wl_output_name_map, 1);
	new_item->name = priv->current_kde_wayland_output_name;
	new_item->geometry = priv->current_kde_wayland_geometry;
	g_ptr_array_add(priv->wl_monitor_output_names, new_item);
}

static void remmina_wl_output_scale(void *data, struct wl_output *wl_output, int32_t factor) {}
static void remmina_wl_output_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct wl_kde_primary_probe *priv = (wl_kde_primary_probe*)data;
	priv->current_kde_wayland_output_name = name;
}
static void remmina_wl_output_description(void *data, struct wl_output *wl_output, const char *description) {}

static const struct wl_output_listener wl_output_listeners = {
    .geometry = remmina_wl_output_geometry,
    .mode = remmina_wl_output_mode,
    .done = remmina_wl_output_done,
    .scale = remmina_wl_output_scale,
    .name = remmina_wl_output_name,
    .description = remmina_wl_output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, kde_output_order_v1_interface.name) == 0 && 
        version >= kde_output_order_v1_interface.version) {
		struct kde_output_order_v1 * reg = wl_registry_bind(registry, name, &kde_output_order_v1_interface, 1);
		kde_output_order_v1_add_listener(reg, &kde_output_order_listeners, data);
		wl_display_roundtrip(display);
    }
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wl_output * reg = wl_registry_bind(registry, name, &wl_output_interface, 4);
		wl_output_add_listener(reg, &wl_output_listeners, data);
	}
}

const GdkRectangle rcw_kde_first_monitor_geometry() {
	GdkRectangle result = {};
	struct wl_kde_primary_probe data = {
		.first_kde_wayland_output_name = NULL,
		.first_kde_wayland_geometry = {},
		.current_kde_wayland_geometry = {},
		.current_kde_wayland_output_name = NULL,
		.wl_monitor_output_names = g_ptr_array_new()
	};
    display = wl_display_connect(NULL);
    if (!display) return result; /* No Wayland session at all */

    struct kde_primary_output_v1 *kde_primary_output = NULL;
    struct wl_registry_listener reg_listener = {0};
    reg_listener.global = registry_global;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &reg_listener, &data);
    wl_display_roundtrip(display);
	wl_display_disconnect(display);
	
	for (gint i = 0 ; i < data.wl_monitor_output_names->len ; i++) {
		struct wl_output_name_map* item = g_ptr_array_index(data.wl_monitor_output_names, i);
		if (strcmp(item->name, data.first_kde_wayland_output_name) == 0) {
			result = item->geometry;
		}
	}

	g_ptr_array_free(data.wl_monitor_output_names, TRUE);
	return result;
}
#endif
