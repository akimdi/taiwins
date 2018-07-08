#ifndef TAIWINS_H
#define TAIWINS_H

#include <wayland-server.h>
#include <compositor.h>

#ifdef  __cplusplus
extern "C" {
#endif

//at some point you will want a compositor or server struct

static inline struct weston_surface *
weston_surface_from_resource(struct wl_resource *wl_surface)
{
	return (struct weston_surface *)wl_resource_get_user_data(wl_surface);
}

struct wl_client *tw_launch_client(struct weston_compositor *ec, const char *path);
void tw_end_client(struct wl_client *client);


#ifdef  __cplusplus
}
#endif



#endif /* EOF */
