/* Included by the pinned Weston 14 gl-renderer. One immutable EGLImage per offer.
 * The desktop context owns the GL memory object only until EGL takes a reference. */
#include <libweston/plugin-registry.h>
#include "wd-server.h"
#include "wd-protocol.c"

static EGLContext weston_texture_desktop;
static struct wl_global *weston_texture_global;
struct weston_texture_offer {
    EGLImageKHR image;
    EGLDisplay display;
    int width, height;
    bool opaque, physical;
    struct weston_buffer_reference hold;
    struct wl_listener carrier_gone;
    struct wl_resource *carrier, *resource;
    struct wl_list link;
};
static struct wl_list weston_texture_pending;

static void weston_texture_unlink(struct weston_texture_offer *o)
{
    if (!o->carrier) return;
    wl_list_remove(&o->link);
    wl_list_remove(&o->carrier_gone.link);
    o->carrier = NULL;
}
static void weston_texture_cancel(struct weston_texture_offer *o)
{
    weston_texture_unlink(o);
}
static void weston_texture_carrier_gone(struct wl_listener *listener, void *unused)
{
    struct weston_texture_offer *o = wl_container_of(listener, o, carrier_gone);
    weston_texture_cancel(o);
}
static void weston_texture_dispose(struct wl_resource *resource)
{
    struct weston_texture_offer *o = wl_resource_get_user_data(resource);
    if (!o) return;
    weston_texture_unlink(o);
    weston_buffer_reference(&o->hold,NULL,BUFFER_WILL_NOT_BE_ACCESSED);
    if (o->image) eglDestroyImage(o->display, o->image);
    free(o);
}
static void weston_texture_destroy_buffer(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}
static const struct wl_buffer_interface weston_texture_buffer_impl = {weston_texture_destroy_buffer};

static EGLImageKHR weston_texture_import(struct gl_renderer *gr, int fd, int width, int height)
{
    EGLDisplay d = gr->egl_display;
    EGLContext saved = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    EGLenum api = eglQueryAPI();
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint memory = 0, texture = 0;
    GLint dedicated = GL_TRUE;
    PFNGLCREATEMEMORYOBJECTSEXTPROC create = (void *)eglGetProcAddress("glCreateMemoryObjectsEXT");
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC param = (void *)eglGetProcAddress("glMemoryObjectParameterivEXT");
    PFNGLIMPORTMEMORYFDEXTPROC import = (void *)eglGetProcAddress("glImportMemoryFdEXT");
    PFNGLTEXSTORAGEMEM2DEXTPROC storage = (void *)eglGetProcAddress("glTexStorageMem2DEXT");
    PFNGLDELETEMEMORYOBJECTSEXTPROC drop = (void *)eglGetProcAddress("glDeleteMemoryObjectsEXT");
    if (!create || !param || !import || !storage || !drop || !eglBindAPI(EGL_OPENGL_API))
        goto done;
    if (!weston_texture_desktop) {
        EGLint n;
        EGLConfig cfg;
        const EGLint attrs[] = {EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
        if (!eglChooseConfig(d, attrs, &cfg, 1, &n) || !n) goto restore;
        weston_texture_desktop = eglCreateContext(d, cfg, EGL_NO_CONTEXT, NULL);
    }
    if (!weston_texture_desktop || !eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,weston_texture_desktop)) goto restore;
    create(1,&memory);
    param(memory,GL_DEDICATED_MEMORY_OBJECT_EXT,&dedicated);
    import(memory,(uint64_t)width*height*4,GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);
    fd = -1;
    glGenTextures(1,&texture);
    glBindTexture(GL_TEXTURE_2D,texture);
    storage(GL_TEXTURE_2D,1,GL_RGBA8,width,height,memory,0);
    if (glGetError() == GL_NO_ERROR) {
        const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        image = gr->create_image(d,weston_texture_desktop,EGL_GL_TEXTURE_2D_KHR,(void *)(uintptr_t)texture,attrs);
    }
    glDeleteTextures(1,&texture);
    drop(1,&memory);
restore:
    eglBindAPI(api);
    if (!eglMakeCurrent(d,draw,read,saved) && image) {
        gr->destroy_image(d,image);
        image = EGL_NO_IMAGE_KHR;
    }
done:
    if (fd >= 0) close(fd);
    return image;
}

#include "hold.h"
static void weston_texture_finish(struct wl_client *client, struct wl_resource *manager)
{
    clear_holds(client);
    struct weston_texture_offer *o, *next;
    wl_list_for_each_safe(o,next,&weston_texture_pending,link)
        if (wl_resource_get_client(o->resource) == client) weston_texture_cancel(o);
}
static void import_offer(struct wl_client *client, struct wl_resource *manager, uint32_t id,
                     struct wl_resource *carrier, int fd, uint32_t opaque, uint32_t serial, int w, int h)
{
    struct weston_compositor *ec = wl_resource_get_user_data(manager);
    struct wl_resource *res = wl_resource_create(client,&wl_buffer_interface,1,id);
    struct weston_texture_offer *o = calloc(1,sizeof(*o));
    if (!res || !o) { close(fd); free(o); wl_client_post_no_memory(client); return; }
    o->resource = res;
    wl_resource_set_implementation(res,&weston_texture_buffer_impl,o,weston_texture_dispose);
    struct wl_shm_buffer *shm = carrier ? wl_shm_buffer_get(carrier) : NULL;
    pid_t pid; uid_t uid; gid_t gid;
    wl_client_get_credentials(client,&pid,&uid,&gid);
    char link[64], name[80] = {0};
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t len = readlink(link,name,sizeof(name)-1);
    if (shm) {w=wl_shm_buffer_get_width(shm); h=wl_shm_buffer_get_height(shm);}
    if (uid == getuid() && ((carrier && shm && w==1222 && (h==1048 || h==1080)) || (!carrier && w>0 && h>0 && w<=8192 && h<=8192)) &&
        opaque<=1 && len>0 &&
        strcmp(name,"anon_inode:dxgresource")==0) {
        o->display=get_renderer(ec)->egl_display;
        o->opaque=opaque; o->width=w; o->height=h; o->physical=!carrier;
        o->image=weston_texture_import(get_renderer(ec),fd,w,h);
    } else close(fd);
    struct weston_buffer *wb = o->image ? weston_buffer_from_resource(ec,res) : NULL;
    bool ok = wb != NULL;
    if (ok) {
        /* Hold release across repeated attaches. Retire drops this reference;
         * the compositor drops its own once the final use has ended. */
        weston_buffer_reference(&o->hold,wb,BUFFER_MAY_BE_ACCESSED);
        o->carrier_gone.notify=weston_texture_carrier_gone;
        if (!carrier) {
            wd_texture_manager_v1_send_result(manager,serial,1);
            return;
        }
        weston_texture_finish(client,manager);
        o->carrier=carrier;
        o->carrier_gone.notify=weston_texture_carrier_gone;
        wl_resource_add_destroy_listener(carrier,&o->carrier_gone);
        wl_list_insert(&weston_texture_pending,&o->link);
    }
    wd_texture_manager_v1_send_result(manager,serial,ok);
}
static void weston_texture_reuse(struct wl_client *client,struct wl_resource *manager,
                     struct wl_resource *res,struct wl_resource *carrier)
{
    if (!wl_resource_instance_of(res,&wl_buffer_interface,&weston_texture_buffer_impl)) return;
    struct weston_texture_offer *o=wl_resource_get_user_data(res);
    struct wl_shm_buffer *shm=wl_shm_buffer_get(carrier);
    if (!o || !o->hold.buffer || !shm || wl_shm_buffer_get_width(shm)!=o->width ||
        wl_shm_buffer_get_height(shm)!=o->height) return;
    weston_texture_finish(client,manager);
    o->carrier=carrier;
    wl_resource_add_destroy_listener(carrier,&o->carrier_gone);
    wl_list_insert(&weston_texture_pending,&o->link);
}
static void weston_texture_retire(struct wl_client *client,struct wl_resource *manager,struct wl_resource *res)
{
    if (!wl_resource_instance_of(res,&wl_buffer_interface,&weston_texture_buffer_impl)) return;
    struct weston_texture_offer *o=wl_resource_get_user_data(res);
    weston_texture_unlink(o);
    weston_buffer_reference(&o->hold,NULL,BUFFER_WILL_NOT_BE_ACCESSED);
}
static void weston_texture_offer(struct wl_client *c,struct wl_resource *m,uint32_t id,
                     struct wl_resource *carrier,int fd,uint32_t opaque,uint32_t serial)
{
    import_offer(c,m,id,carrier,fd,opaque,serial,0,0);
}
static void prepare_target(struct wl_client *c,struct wl_resource *m,uint32_t id,
                           int fd,uint32_t width,uint32_t height,uint32_t serial)
{
    import_offer(c,m,id,NULL,fd,0,serial,width,height);
}
static void prepare_client(struct wl_client *c,struct wl_resource *m,uint32_t id,int fd,uint32_t serial)
{
    import_offer(c,m,id,NULL,fd,0,serial,1920,1048);
}
static const struct wd_texture_manager_v1_interface weston_texture_impl = {weston_texture_offer,weston_texture_finish,weston_texture_reuse,weston_texture_retire,prepare_client,prepare_target,hold_carrier};
static void weston_texture_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *r=wl_resource_create(c,&wd_texture_manager_v1_interface,version,id);
    if (r) wl_resource_set_implementation(r,&weston_texture_impl,data,NULL);
    else wl_client_post_no_memory(c);
}
static struct wl_resource *weston_texture_consume(struct wl_resource *carrier)
{
    struct weston_texture_offer *o;
    struct wl_listener *hold=wl_resource_get_destroy_listener(carrier,remove_hold);
    if(hold) {remove_hold(hold,NULL); return NULL;}
    wl_list_for_each(o,&weston_texture_pending,link) {
        if (o->carrier != carrier) continue;
        weston_texture_unlink(o);
        return o->resource;
    }
    return carrier;
}
static const struct {struct wl_resource *(*consume)(struct wl_resource *);} weston_texture_api={weston_texture_consume};
static void weston_texture_start(struct weston_compositor *ec)
{
    wl_list_init(&holds);
    wl_list_init(&weston_texture_pending);
    if (weston_plugin_api_register(ec,"wd-texture-attach-v1",&weston_texture_api,sizeof(weston_texture_api))==0)
        weston_texture_global=wl_global_create(ec->wl_display,&wd_texture_manager_v1_interface,3,ec,weston_texture_bind);
}
static bool weston_texture_fill(struct weston_compositor *ec, struct weston_buffer *b)
{
    struct weston_texture_offer *o=wl_resource_get_user_data(b->resource);
    if (!o || !o->image) return false;
    struct gl_buffer_state *gb=zalloc(sizeof(*gb));
    if (!gb) return false;
    gb->gr=get_renderer(ec);
    gb->num_images=1;
    gb->images[0]=o->image;
    o->image=EGL_NO_IMAGE_KHR;
    gb->shader_variant=o->opaque ? SHADER_VARIANT_RGBX : SHADER_VARIANT_RGBA;
    b->width=o->width; b->height=o->height;
    b->buffer_origin=o->physical ? ORIGIN_TOP_LEFT : ORIGIN_BOTTOM_LEFT;
    b->format_modifier=DRM_FORMAT_MOD_INVALID;
    b->pixel_format=pixel_format_get_info(o->opaque ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_ABGR8888);
    pixman_region32_init(&gb->texture_damage);
    ensure_textures(gb,GL_TEXTURE_2D,1);
    b->renderer_private=gb;
    gb->destroy_listener.notify=handle_buffer_destroy;
    wl_signal_add(&b->destroy_signal,&gb->destroy_listener);
    return true;
}
