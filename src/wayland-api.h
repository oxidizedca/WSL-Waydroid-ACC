#pragma once
#include <wayland-client-core.h>
/* The vendor statically links its Wayland client. Its exported functions must
 * handle proxies created on its connection, including our private queue. */
static struct {
    __typeof__(wl_proxy_get_version) *wl_proxy_get_version;
    __typeof__(wl_proxy_marshal_flags) *wl_proxy_marshal_flags;
    __typeof__(wl_proxy_add_listener) *wl_proxy_add_listener;
    __typeof__(wl_proxy_destroy) *wl_proxy_destroy;
    __typeof__(wl_proxy_create_wrapper) *wl_proxy_create_wrapper;
    __typeof__(wl_proxy_wrapper_destroy) *wl_proxy_wrapper_destroy;
    __typeof__(wl_proxy_set_queue) *wl_proxy_set_queue;
    __typeof__(wl_display_dispatch_queue_pending) *wl_display_dispatch_queue_pending;
    __typeof__(wl_display_create_queue) *wl_display_create_queue;
    __typeof__(wl_display_roundtrip_queue) *wl_display_roundtrip_queue;
} client;
static bool weston_texture_load_client(void *lib)
{
#define LOAD(member) if (!(client.member=dlsym(lib,#member))) return false
    LOAD(wl_proxy_get_version);
    LOAD(wl_proxy_marshal_flags);
    LOAD(wl_proxy_add_listener);
    LOAD(wl_proxy_destroy);
    LOAD(wl_proxy_create_wrapper);
    LOAD(wl_proxy_wrapper_destroy);
    LOAD(wl_proxy_set_queue);
    LOAD(wl_display_dispatch_queue_pending);
    LOAD(wl_display_create_queue);
    LOAD(wl_display_roundtrip_queue);
#undef LOAD
    return true;
}
#define wl_proxy_get_version client.wl_proxy_get_version
#define wl_proxy_marshal_flags client.wl_proxy_marshal_flags
#define wl_proxy_add_listener client.wl_proxy_add_listener
#define wl_proxy_destroy client.wl_proxy_destroy
#define wl_proxy_create_wrapper client.wl_proxy_create_wrapper
#define wl_proxy_wrapper_destroy client.wl_proxy_wrapper_destroy
#define wl_proxy_set_queue client.wl_proxy_set_queue
#define wl_display_dispatch_queue_pending client.wl_display_dispatch_queue_pending
#define wl_display_create_queue client.wl_display_create_queue
#define wl_display_roundtrip_queue client.wl_display_roundtrip_queue
