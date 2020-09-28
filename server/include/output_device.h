/*
 * output_device.h - taiwins server output device header
 *
 * Copyright (c) 2020 Xichen Zhou
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

#ifndef TW_OUTPUT_DEVICE_H
#define TW_OUTPUT_DEVICE_H

#include <wayland-server-protocol.h>
#include <wayland-server.h>

#include <taiwins/objects/matrix.h>

#ifdef  __cplusplus
extern "C" {
#endif

struct tw_output_device_mode {
	int32_t w, h;
	int32_t refresh; /** -1 means unavailable */
	bool preferred;
};

struct tw_output_device_state {
	bool enabled;
	float scale;
	int32_t x_comp, y_comp; /**< x,y position in global space */
	struct tw_mat3 view_2d;
	enum wl_output_subpixel subpixel;
	enum wl_output_transform transform;
	struct tw_output_device_mode current_mode;

};

/**
 * @brief abstraction of an output device
 *
 * Backends will pass a tw_output_device in the new_output signal.

 * At the moment, tw_backend_output has quite a few methods that actually does
 * not belong to it, currently the user is the configs. We want
 * tw_backend_output to include this and drive direct on output devices's
 * methods.
 *
 * We would probably need to include
 */
struct tw_output_device {
	char name[32], make[32], model[32];
	char serial[16];
	int32_t phys_width, phys_height;

	/** a native window for different backend, could be none */
	void *native_window;

	struct wl_list link; /** backend: list */
	struct wl_array available_modes;

	struct tw_output_device_state state, pending;

	struct {
		struct wl_signal destroy;
		struct wl_signal present;
		struct wl_signal new_frame;
		/** emit when general wl_output information is available, or
		 * when output state changed */
                struct wl_signal info;
		/** emit right after applying pending state, backend could
		 * listen on this for applying the states to the hardware */
		struct wl_signal commit_state;
	} events;
};

void
tw_output_device_init(struct tw_output_device *device);

void
tw_output_device_fini(struct tw_output_device *device);

void
tw_output_device_set_scale(struct tw_output_device *device, float scale);

void
tw_output_device_commit_state(struct tw_output_device *device);

#ifdef  __cplusplus
}
#endif

#endif