/*
 * workspace.c - taiwins desktop workspace implementation
 *
 * Copyright (c) 2019 Xichen Zhou
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <string.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <pixman.h>
#include <ctypes/helpers.h>
#include <objects/utils.h>
#include <objects/surface.h>

#include "desktop/xdg.h"
#include "objects/desktop.h"
#include "objects/layers.h"
#include "objects/logger.h"
#include "shell.h"
#include "workspace.h"
#include "layout.h"

/******************************************************************************
 * tw_xdg_view api
 *****************************************************************************/
struct tw_xdg_view *
tw_xdg_view_create(struct tw_desktop_surface *dsurf)
{
	struct tw_xdg_view *view = calloc(1, sizeof(*view));
	if (!view)
		return NULL;
	view->dsurf = dsurf;
	view->mapped = false;
	view->xwayland.is_xwayland = false;
	wl_list_init(&view->link);
	wl_signal_init(&view->dsurf_umapped_signal);
	return view;
}

void
tw_xdg_view_destroy(struct tw_xdg_view *view)
{
	wl_signal_emit(&view->dsurf_umapped_signal, view);
	tw_reset_wl_list(&view->link);
	free(view);
}

void
tw_xdg_view_set_position(struct tw_xdg_view *view, int x, int y)
{
	struct tw_surface *surface = view->dsurf->tw_surface;
	int32_t gx = view->dsurf->window_geometry.x;
	int32_t gy = view->dsurf->window_geometry.y;

        view->x = x;
	view->y = y;
	view->mapped = true;
	tw_surface_set_position(surface, x-gx, y-gy);
}

void
tw_xdg_view_configure_size(struct tw_xdg_view *view, uint32_t w, uint32_t h)
{
	view->dsurf->configure(view->dsurf, 0, 0, 0, w, h);
}

static void
tw_xdg_view_backup_geometry(struct tw_xdg_view *v)
{
	v->old_geometry.x = v->x;
	v->old_geometry.y = v->y;
	v->old_geometry.width = v->dsurf->window_geometry.w;
	v->old_geometry.height = v->dsurf->window_geometry.h;
}

/******************************************************************************
 * workspace implementation
 *****************************************************************************/

void
tw_workspace_init(struct tw_workspace *wp, struct tw_layers_manager *layers,
                  uint32_t idx)
{
	assert(idx < MAX_WORKSPACES);
	wp->idx = idx;
	wp->layers_manager = layers;
	wl_list_init(&wp->layouts);
	tw_layer_init(&wp->hidden_layer);
	tw_layer_init(&wp->fullscreen_back_layer);
	tw_layer_init(&wp->back_layer);
	tw_layer_init(&wp->mid_layer);
	tw_layer_init(&wp->front_layer);
	tw_layer_init(&wp->fullscreen_layer);
	//init layout
	/* floating_layout_init(&wp->floating_layout, &wp->floating_layer); */
	/* tiling_layout_init(&wp->tiling_layout, &wp->tiling_layer, */
	/*		   &wp->floating_layout); */
	wl_list_init(&wp->recent_views);
	wp->current_layout = LAYOUT_TILING;
}

void
tw_workspace_release(struct tw_workspace *ws)
{
	struct tw_surface *surf, *next;
	struct tw_xdg_view *view, *tmp;

	struct tw_layer *layers[]  = {
		&ws->hidden_layer,
		&ws->fullscreen_back_layer,
		&ws->back_layer,
		&ws->mid_layer,
		&ws->front_layer,
		&ws->fullscreen_layer,
	};
	//get rid of all the surface, maybe
	for (unsigned i = 0; i < NUMOF(layers); i++) {
		wl_list_for_each_safe(surf, next, &layers[i]->views,
				      links[TW_VIEW_LAYER_LINK])
			tw_reset_wl_list(&surf->links[TW_VIEW_LAYER_LINK]);
	}
	//we have this?
	wl_list_for_each_safe(view, tmp, &ws->recent_views, link)
		tw_xdg_view_destroy(view);

	/* floating_layout_end(&ws->floating_layout); */
	/* tiling_layout_end(&ws->tiling_layout); */
}

bool
tw_workspace_has_view(const struct tw_workspace *ws,
                      const struct tw_xdg_view *v)
{
	if (!v || !ws)
		return false;
	return v->layer == &ws->fullscreen_layer ||
		v->layer == &ws->front_layer ||
		v->layer == &ws->mid_layer ||
		v->layer == &ws->back_layer ||
		v->layer == &ws->fullscreen_back_layer ||
		v->layer == &ws->hidden_layer;
}

struct tw_xdg_view *
tw_workspace_get_top_view(const struct tw_workspace *ws)
{
	return wl_list_empty(&ws->recent_views) ? NULL :
		container_of(ws->recent_views.next, struct tw_xdg_view, link);
}

static struct tw_xdg_layout *
tw_workspace_view_pick_layout(const struct tw_workspace *ws,
                              const struct tw_xdg_view *v)
{
	//I could simply return v->layout
	struct tw_xdg_layout *layout;

	wl_list_for_each(layout, &ws->layouts, links[ws->idx]) {
		if (layout->type == v->type)
			return layout;
	}
	return NULL;
}

static struct tw_layer *
tw_workspace_view_pick_layer(struct tw_workspace *ws,
                             struct tw_xdg_view *v)
{
	if (v->type == LAYOUT_FLOATING || v->type == LAYOUT_MAXMIZED)
		return &ws->front_layer;
	else if (v->type == LAYOUT_FULLSCREEN)
		return &ws->fullscreen_layer;
	else //tiling layout
		return &ws->mid_layer;
}

static uint32_t
tw_workspace_n_surfaces(const struct tw_workspace *ws,
                        bool include_fullscreen )
{
	uint32_t nviews = 0;
	const struct tw_layer *dlayers[] = {
		&ws->back_layer, &ws->mid_layer, &ws->front_layer,
	};
	const struct tw_layer *flayers[] = {
		&ws->fullscreen_back_layer, &ws->fullscreen_layer,
	};

	for (unsigned i = 0; i < NUMOF(dlayers); i++)
		nviews += wl_list_length(&dlayers[i]->views);
	if (include_fullscreen)
		for (unsigned i = 0; i < NUMOF(flayers); i++)
			nviews += wl_list_length(&flayers[i]->views);
	return nviews;
}

static void
apply_layout_operations(const struct tw_xdg_layout_op *ops, const int len)
{
	for (int i = 0; i < len && !ops[i].out.end; i++) {
		struct tw_xdg_view *v = ops[i].v;
		tw_xdg_view_set_position(v, ops[i].out.pos.x,
		                         ops[i].out.pos.y);

		if (ops[i].out.size.height && ops[i].out.size.width)
			tw_xdg_view_configure_size(v, ops[i].out.size.width,
			                           ops[i].out.size.height);
	}
}

static void
arrange_view_for_layout(struct tw_workspace *ws, struct tw_xdg_layout *layout,
			struct tw_xdg_view *view,
			const enum tw_xdg_layout_command command,
			const struct tw_xdg_layout_op *arg)
{
	//this is the very smart part of the operation, you know the largest
	//possible number of operations, and give pass that into layouting
	//algorithm, so you don't need any memory allocations here we have a
	//extra buffer
	int max_len = tw_workspace_n_surfaces(ws, false) +
		((command == DPSR_add) ? 2 : 1);
	struct tw_xdg_layout_op ops[max_len];
	memset(ops, 0, sizeof(ops));
	layout->command(command, arg, view, layout, ops);
	apply_layout_operations(ops, max_len);
}

static void
arrange_view_for_workspace(struct tw_workspace *ws, struct tw_xdg_view *v,
                           const enum tw_xdg_layout_command command,
                           const struct tw_xdg_layout_op *arg)
{
	//IF v is NULL, we need to re-arrange the entire output
	if (!v) {
		//TODO: we will have a list of layouts that manage themselves.
		/* arrange_view_for_layout(ws, &ws->floating_layout, NULL, */
		/*			     command, arg); */
		/* arrange_view_for_layout(ws, &ws->tiling_layout, NULL, */
		/*			     command, arg); */
	} else {
		assert(v->layout);
		arrange_view_for_layout(ws, v->layout, v, command, arg);
	}
}

struct tw_xdg_view *
tw_workspace_switch(struct tw_workspace *to, struct tw_workspace *from)
{
	//How do I run a animation here?
	tw_layer_unset_position(&from->fullscreen_back_layer);
	tw_layer_unset_position(&from->back_layer);
	tw_layer_unset_position(&from->mid_layer);
	tw_layer_unset_position(&from->front_layer);
	tw_layer_unset_position(&from->fullscreen_layer);

	tw_layer_set_position(&to->fullscreen_back_layer,
	                      TW_LAYER_POS_FULLSCREEN_BACK,
	                      to->layers_manager);
	tw_layer_set_position(&to->back_layer, TW_LAYER_POS_DESKTOP_BACK,
	                      to->layers_manager);
	tw_layer_set_position(&to->mid_layer, TW_LAYER_POS_DESKTOP_MID,
	                      to->layers_manager);
	tw_layer_set_position(&to->front_layer, TW_LAYER_POS_DESKTOP_FRONT,
	                      to->layers_manager);
	tw_layer_set_position(&to->fullscreen_layer,
	                      TW_LAYER_POS_FULLSCREEN_FRONT,
	                      to->layers_manager);
	return tw_workspace_get_top_view(to);
}

static inline void
tw_workspace_move_front_views(struct tw_workspace *ws)
{
	struct tw_surface *replaced, *tmp;
	//move all the front layer apps to back
	wl_list_for_each_reverse_safe(replaced, tmp, &ws->front_layer.views,
	                              links[TW_VIEW_LAYER_LINK]) {
		wl_list_remove(&replaced->links[TW_VIEW_LAYER_LINK]);
		wl_list_insert(&ws->back_layer.views,
		               &replaced->links[TW_VIEW_LAYER_LINK]);
	}
}

bool
tw_workspace_focus_view(struct tw_workspace *ws, struct tw_xdg_view *v)
{
	struct wl_list *replaced;
	struct tw_surface *surface = v->dsurf->tw_surface;
	if (!tw_workspace_has_view(ws, v))
		return false;

	wl_list_remove(&surface->links[TW_VIEW_LAYER_LINK]);

	if (v->type == LAYOUT_FULLSCREEN) {
		assert(wl_list_length(&ws->fullscreen_layer.views) <= 1);
		if (!wl_list_empty(&ws->fullscreen_layer.views)) {
			replaced = ws->fullscreen_layer.views.next;

			wl_list_remove(replaced);
			wl_list_insert(&ws->fullscreen_back_layer.views,
			               replaced);
		}
		wl_list_insert(&ws->fullscreen_layer.views,
		               &surface->links[TW_VIEW_LAYER_LINK]);
	} else if (v->type == LAYOUT_FLOATING || v->type == LAYOUT_MAXMIZED) {
	        wl_list_insert(&ws->front_layer.views,
	                       &surface->links[TW_VIEW_LAYER_LINK]);
	} else if (v->type == LAYOUT_TILING) {
		tw_workspace_move_front_views(ws);
		wl_list_insert(&ws->mid_layer.views,
		               &surface->links[TW_VIEW_LAYER_LINK]);
	} else {
		tw_logl_level(TW_LOG_ERRO, "the view has invalid layer");
	}
	//finally we manage our recent views.
	wl_list_remove(&v->link);
	wl_list_insert(&ws->recent_views, &v->link);
	//TODO: we need also give keyboard focus to the surface, but this part
	//can be could be on xdg

	return true;
}

struct tw_xdg_view *
tw_workspace_defocus_view(struct tw_workspace *ws, struct tw_xdg_view *v)
{
	if (!tw_workspace_has_view(ws, v))
		goto get_top;
	wl_list_remove(&v->link);
	wl_list_insert(ws->recent_views.next, &v->link);
get_top:
	wl_list_for_each(v, &ws->recent_views, link)
		return v;
	return NULL;
}

static void
tw_workspace_add_view_with_geometry(struct tw_workspace *w,
                                    struct tw_xdg_view *view,
                                    pixman_rectangle32_t *geo)
{
	struct tw_surface *surface = view->dsurf->tw_surface;
	struct tw_xdg_layout_op arg = {
		.v = view,
		.in.default_geometry = {
			geo->x, geo->y, geo->width, geo->height
		},
	};
	view->layout = tw_workspace_view_pick_layout(w, view);
	view->layer = tw_workspace_view_pick_layer(w, view);
	tw_reset_wl_list(&surface->links[TW_VIEW_LAYER_LINK]);
	arrange_view_for_workspace(w, view, DPSR_add, &arg);

	tw_workspace_focus_view(w, view);
}

void
tw_workspace_add_view(struct tw_workspace *w, struct tw_xdg_view *view)
{
	pixman_rectangle32_t dummy_geo = {-1, -1, 0, 0};
	tw_workspace_add_view_with_geometry(w, view, &dummy_geo);
}

bool
tw_workspace_remove_view(struct tw_workspace *w, struct tw_xdg_view *view)
{
	struct tw_surface *surface;
	struct tw_xdg_layout_op arg = {
		.v = view,
	};
	if (!w || !view)
		return false;
	surface = view->dsurf->tw_surface;

	arrange_view_for_workspace(w, view, DPSR_del, &arg);
	tw_reset_wl_list(&surface->links[TW_VIEW_LAYER_LINK]);
	return true;
}

bool
tw_workspace_move_view(struct tw_workspace *w, struct tw_xdg_view *view,
                       double dx, double dy)
{
	struct tw_xdg_layout_op arg = {
		.v = view,
		.in.dx = dx,
		.in.dy = dy,
	};
	if (!tw_workspace_has_view(w, view))
		return false;
	arrange_view_for_workspace(w, view, DPSR_deplace, &arg);
	return false;
}

void
tw_workspace_resize_view(struct tw_workspace *w, struct tw_xdg_view *v,
                         double dx, double dy)
{
	struct tw_xdg_layout_op arg = {
		.v = v,
		.in.dx = dx, .in.dy = dy,
	};
	arrange_view_for_workspace(w, v, DPSR_resize, &arg);
}


void
tw_workspace_fullscreen_view(struct tw_workspace *w, struct tw_xdg_view *v,
                             struct tw_xdg_output *output, bool fullscreen)
{
	pixman_rectangle32_t geo = {
		output->output->state.x, output->output->state.y,
		output->output->state.w, output->output->state.h,
	};
	if (fullscreen) {
		tw_xdg_view_backup_geometry(v);
		v->prev_type = v->type;
		v->type = LAYOUT_FULLSCREEN;
	} else {
		geo = v->old_geometry;
		v->type = v->prev_type;
	}

        tw_workspace_remove_view(w, v);
        tw_workspace_add_view_with_geometry(w, v, &geo);
}

void
tw_workspace_maximize_view(struct tw_workspace *w, struct tw_xdg_view *v,
                           pixman_rectangle32_t *maximized_geo,
                           bool maximized)
{
	pixman_rectangle32_t geo = *maximized_geo;
	if (maximized) {
		tw_xdg_view_backup_geometry(v);
		v->prev_type = v->type;
		v->type = LAYOUT_MAXMIZED;
	} else {
		geo = v->old_geometry;
		v->type = v->prev_type;
	}
	tw_workspace_remove_view(w, v);
	tw_workspace_add_view_with_geometry(w, v, &geo);
}

//TODO: this can go as the same like fullscreen and maximized
void
tw_workspace_minimize_view(struct tw_workspace *w, struct tw_xdg_view *v)
{
	struct tw_surface *surface;
	if (!tw_workspace_has_view(w, v))
		return;
	surface = v->dsurf->tw_surface;
	tw_workspace_remove_view(w, v);
	wl_list_insert(&w->hidden_layer.views,
	               &surface->links[TW_VIEW_LAYER_LINK]);
	tw_workspace_defocus_view(w, v);
}

void
tw_workspace_switch_layout(struct tw_workspace *w, struct tw_xdg_view *view)
{
	if (view->type != LAYOUT_FLOATING && view->type != LAYOUT_TILING)
		return;
	if (tw_workspace_has_view(w, view))
		return;
	tw_workspace_remove_view(w, view);
	view->type = (view->type == LAYOUT_TILING) ?
		LAYOUT_FLOATING : LAYOUT_TILING;
	tw_workspace_add_view(w, view);
}

const char *
tw_workspace_layout_name(struct tw_workspace *ws)
{
	switch (ws->current_layout) {
	case LAYOUT_FLOATING:
		return "floating";
	case LAYOUT_TILING:
		return "tiling";
	case LAYOUT_MAXMIZED:
		return "maximized";
	case LAYOUT_FULLSCREEN:
		return "fullscreened";
	}
	return NULL;
}

void
tw_workspace_add_output(struct tw_workspace *wp, struct tw_xdg_output *output)
{
	//TODO
	/* layout_add_output(&wp->tiling_layout, output); */
}

void
tw_workspace_resize_output(struct tw_workspace *wp,
                           struct tw_xdg_output *output)
{
	/* tw_layout_resize_output(&wp->tiling_layout, output); */
	const struct tw_xdg_layout_op arg = {
		.in.o = output,
	};
	arrange_view_for_workspace(wp, NULL, DPSR_output_resize, &arg);
}

void
tw_workspace_remove_output(struct tw_workspace *w,
                           struct tw_xdg_output *output)
{
	//TODO
	/* layout_rm_output(&w->tiling_layout, output); */
}
