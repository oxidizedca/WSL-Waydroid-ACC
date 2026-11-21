/* gl-init.h selects GNU libc interfaces before any system header. */
#include "gl-init.h"
#include "capture.h"
#include "viewporter.h"
#include "wd-client.h"
#include "xdg.h"
#include <assert.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client.h>
#define W 1920
#define H 1080
static struct wl_display *display;
static struct wl_compositor *comp;
static struct wl_subcompositor *subcomp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wp_viewporter *vpt;
static struct wl_output *output;
static struct weston_capture_v1 *cap;
static struct wd_texture_manager_v1 *mgr;
static void reg(void *d, struct wl_registry *r, uint32_t id, const char *n,
                uint32_t ver) {
#define B(v, t, minv)                                                          \
  if (!strcmp(n, #t)) {                                                        \
    assert(ver >= minv);                                                       \
    v = wl_registry_bind(r, id, &t##_interface, minv);                         \
  }
  B(vpt, wp_viewporter, 1)
  B(comp, wl_compositor, 4) B(subcomp, wl_subcompositor, 1) B(shm, wl_shm, 1)
      B(wm, xdg_wm_base, 1) B(output, wl_output, 1) B(cap, weston_capture_v1, 1)
          B(mgr, wd_texture_manager_v1, 3)
}
static void removed(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener reg_l = {reg, removed};
static void ping(void *d, struct xdg_wm_base *w, uint32_t s) {
  xdg_wm_base_pong(w, s);
}
static const struct xdg_wm_base_listener wm_l = {ping};
static int configured, gotframe, accepted, capdone, cw, ch;
static uint32_t fmt, ack;
static void config(void *d, struct xdg_surface *x, uint32_t s) {
  xdg_surface_ack_configure(x, s);
  configured = 1;
}
static const struct xdg_surface_listener xd_l = {config};
static void topcfg(void *d, struct xdg_toplevel *t, int w, int h,
                   struct wl_array *a) {}
static void topclose(void *d, struct xdg_toplevel *t) { abort(); }
static const struct xdg_toplevel_listener top_l = {.configure = topcfg,
                                                   .close = topclose};
static void frame(void *d, struct wl_callback *c, uint32_t t) {
  wl_callback_destroy(c);
  gotframe = 1;
}
static const struct wl_callback_listener frame_l = {frame};
static void result(void *d, struct wd_texture_manager_v1 *m, uint32_t s,
                   uint32_t ok) {
  ack = s;
  accepted = ok;
}
static const struct wd_texture_manager_v1_listener mgr_l = {result};
static void cfmt(void *d, struct weston_capture_source_v1 *s, uint32_t f) {
  fmt = f;
}
static void csize(void *d, struct weston_capture_source_v1 *s, int w, int h) {
  cw = w;
  ch = h;
}
static void done(void *d, struct weston_capture_source_v1 *s) { capdone = 1; }
static void retry(void *d, struct weston_capture_source_v1 *s) { capdone = 2; }
static void fail(void *d, struct weston_capture_source_v1 *s, const char *why) {
  fprintf(stderr, "capture: %s\n", why);
  abort();
}
static const struct weston_capture_source_v1_listener cap_l = {
    cfmt, csize, done, retry, fail};
struct buf {
  struct wl_buffer *b;
  uint32_t *px;
};
static struct buf mk(int w, int h, uint32_t format, uint32_t c) {
  int fd = memfd_create("replay-shm", MFD_CLOEXEC);
  size_t sz = (size_t)w * h * 4;
  assert(fd >= 0 && !ftruncate(fd, sz));
  uint32_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  assert(p != MAP_FAILED);
  for (size_t i = 0; i < sz / 4; i++)
    p[i] = c;
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, sz);
  struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format);
  wl_shm_pool_destroy(pool);
  close(fd);
  return (struct buf){b, p};
}
static void waitfor(int *v) {
  while (!*v)
    assert(wl_display_dispatch(display) >= 0);
}
static int (*snapshot)(int, int, int *, void **, int, int64_t *);
static void (*drop)(void *);
struct offer {
  struct wl_buffer *b;
  void *held;
  int released;
};
static void release(void *d, struct wl_buffer *b) {
  ((struct offer *)d)->released++;
  fprintf(stderr, "GPU release %u\n", wl_proxy_get_id((struct wl_proxy *)b));
}
static const struct wl_buffer_listener rel_l = {release};
static const int x[] = {1850, 1874, 1900}, widths[] = {16, 20, 18},
                 heights[] = {52, 90, 108};
static const uint32_t colors[] = {0xffff0000, 0x80008000, 0xffffffff};
static void make_offer(struct offer *o, int kind, int seq) {
  GLuint t, f;
  GenTextures(1, &t);
  BindTexture(GL_TEXTURE_2D, t);
  ((__typeof__(&glTexImage2D))eglGetProcAddress("glTexImage2D"))(
      GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  GenFramebuffers(1, &f);
  BindFramebuffer(GL_FRAMEBUFFER, f);
  FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t,
                       0);
  assert(CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  __typeof__(&glClearColor) cc = (void *)eglGetProcAddress("glClearColor");
  __typeof__(&glClear) clear = (void *)eglGetProcAddress("glClear");
  __typeof__(&glEnable) en = (void *)eglGetProcAddress("glEnable");
  __typeof__(&glDisable) dis = (void *)eglGetProcAddress("glDisable");
  __typeof__(&glScissor) sc = (void *)eglGetProcAddress("glScissor");
  cc(0, kind == 1, kind == 0, kind != 2);
  clear(GL_COLOR_BUFFER_BIT);
  if (kind == 2) {
    en(GL_SCISSOR_TEST);
    for (int i = 0; i < 3; i++) {
      uint32_t c = colors[i];
      cc((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f,
         (c >> 24) / 255.f);
      sc(x[i], (H - heights[i]) / 2, widths[i], heights[i]);
      clear(GL_COLOR_BUFFER_BIT);
    }
    dis(GL_SCISSOR_TEST);
  }
  int fd[2];
  int64_t tm[4] = {};
  assert(!snapshot(W, H, fd, &o->held, 0, tm));
  struct pollfd p = {fd[1], POLLIN, 0};
  assert(poll(&p, 1, 3000) == 1 && (p.revents & POLLIN));
  close(fd[1]);
  o->b = wd_texture_manager_v1_prepare_target(mgr, fd[0], W, H, seq);
  close(fd[0]);
  wl_buffer_add_listener(o->b, &rel_l, o);
  assert(wl_display_roundtrip(display) >= 0 && ack == (uint32_t)seq &&
         accepted);
  DeleteFramebuffers(1, &f);
  DeleteTextures(1, &t);
}
static void put(struct wl_surface *s, struct wl_buffer *b) {
  wl_surface_attach(s, b, 0, 0);
  wl_surface_damage_buffer(s, 0, 0, W, H);
  wl_surface_commit(s);
}
int main(int argc, char **argv) {
  int replace = argc == 2 && !strcmp(argv[1], "replace-visible");
  int anchor = replace || (argc == 2 && !strcmp(argv[1], "reuse-anchor"));
  printf("mode=%s\n", replace  ? "replace-visible"
                      : anchor ? "reuse-anchor"
                               : "separate-surface");
  setbuf(stdout, NULL);
  assert(init_gl());
  snapshot = dlsym(RTLD_DEFAULT, "texture_cache_export_target");
  drop = dlsym(RTLD_DEFAULT, "physical_snapshot_drop");
  assert(snapshot && drop);
  display = wl_display_connect(getenv("REPLAY_SOCKET"));
  assert(display);
  struct wl_registry *r = wl_display_get_registry(display);
  wl_registry_add_listener(r, &reg_l, NULL);
  assert(wl_display_roundtrip(display) >= 0 && comp && shm && subcomp && wm &&
         output && cap && mgr && vpt);
  xdg_wm_base_add_listener(wm, &wm_l, NULL);
  wd_texture_manager_v1_add_listener(mgr, &mgr_l, NULL);
  struct wl_surface *root = wl_compositor_create_surface(comp);
  struct xdg_surface *xd = xdg_wm_base_get_xdg_surface(wm, root);
  xdg_surface_add_listener(xd, &xd_l, NULL);
  struct xdg_toplevel *top = xdg_surface_get_toplevel(xd);
  xdg_toplevel_add_listener(top, &top_l, NULL);
  xdg_toplevel_set_fullscreen(top, output);
  wl_surface_commit(root);
  waitfor(&configured);
  struct weston_capture_source_v1 *cs = weston_capture_v1_create(
      cap, output, WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER);
  weston_capture_source_v1_add_listener(cs, &cap_l, NULL);
  assert(wl_display_roundtrip(display) >= 0);
  fprintf(stderr, "readback format=%08x %dx%d\n", fmt, cw, ch);
  assert(cw == W && ch == H);
  uint32_t sf = fmt == DRM_FORMAT_XRGB8888   ? WL_SHM_FORMAT_XRGB8888
                : fmt == DRM_FORMAT_ARGB8888 ? WL_SHM_FORMAT_ARGB8888
                                             : fmt;
  assert(fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888 ||
         fmt == DRM_FORMAT_ABGR8888 || fmt == DRM_FORMAT_XBGR8888);
  struct buf out = mk(W, H, sf, 0), carrier[3], bars[3];
  struct wl_surface *child[4];
  struct wl_subsurface *sub[4];
  struct wp_viewport *vp[4];
  struct offer o[3] = {};
  for (int i = 0; i < 4; i++) {
    child[i] = wl_compositor_create_surface(comp);
    vp[i] = wp_viewporter_get_viewport(vpt, child[i]);
    sub[i] = wl_subcompositor_get_subsurface(subcomp, child[i], root);
    wl_subsurface_set_position(sub[i], i < 3 ? x[i] : 0,
                               i < 3 ? (H - heights[i]) / 2 : 0);
  }
  for (int i = 0; i < 3; i++) {
    carrier[i] = mk(W, H, WL_SHM_FORMAT_ARGB8888, 0xffff00ff);
    bars[i] = mk(widths[i], heights[i], WL_SHM_FORMAT_ARGB8888, colors[i]);
    make_offer(&o[i], i, i + 1);
  }
  uint32_t *ref[2] = {calloc(W * H, 4), calloc(W * H, 4)};
  assert(ref[0] && ref[1]);
  for (int step = 0; step < 12; step++) {
    int base = step % 2, cached = step / 2 % 2;
    int slot = anchor ? 0 : 3;
    for (int i = 0; i < 3; i++) {
      if (cached && i == slot)
        continue;
      wp_viewport_set_source(vp[i], 0, 0, wl_fixed_from_int(widths[i]),
                             wl_fixed_from_int(heights[i]));
      wp_viewport_set_destination(vp[i], widths[i], heights[i]);
      wl_subsurface_set_position(sub[i], x[i], (H - heights[i]) / 2);
      put(child[i], cached ? NULL : bars[i].b);
    }
    if (cached)
      wd_texture_manager_v1_reuse(mgr, o[2].b, carrier[2].b);
    if (cached) {
      wp_viewport_set_source(vp[slot], 0, 0, wl_fixed_from_int(W),
                             wl_fixed_from_int(H));
      wp_viewport_set_destination(vp[slot], W, H);
      wl_subsurface_set_position(sub[slot], 0, 0);
      put(child[slot], carrier[2].b);
    } else if (!anchor)
      put(child[3], NULL);
    wd_texture_manager_v1_reuse(mgr, o[base].b, carrier[base].b);
    gotframe = 0;
    wl_callback_add_listener(wl_surface_frame(root), &frame_l, NULL);
    put(root, carrier[base].b);
    wd_texture_manager_v1_finish(mgr);
    waitfor(&gotframe);
    do {
      capdone = 0;
      weston_capture_source_v1_capture(cs, out.b);
      waitfor(&capdone);
    } while (capdone == 2);
    size_t diff = 0;
    uint32_t bg = out.px[W * H / 2 + 20] & 0xffffff;
    uint32_t target =
        base ? 0xff00
             : (fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888
                    ? 0xff
                    : 0xff0000);
    assert(bg == target);
    for (int i = 0; i < 3; i++)
      assert((out.px[W * H / 2 + x[i] + 2] & 0xffffff) != bg ||
             (i == 1 && base == 1));
    if (step < 2)
      memcpy(ref[base], out.px, W * H * 4);
    else
      for (int i = 0; i < W * H; i++)
        diff += (out.px[i] & 0xffffff) != (ref[base][i] & 0xffffff);
    printf("step=%d base=%d cached=%d different_rgb_pixels=%zu\n", step, base,
           cached, diff);
    if (diff)
      return 2;
  }
  for (int i = 0; i < 4; i++)
    put(child[i], NULL);
  if (replace) {
    wp_viewport_set_source(vp[0], 0, 0, wl_fixed_from_int(widths[0]),
                           wl_fixed_from_int(heights[0]));
    wp_viewport_set_destination(vp[0], widths[0], heights[0]);
    wl_subsurface_set_position(sub[0], x[0], (H - heights[0]) / 2);
    put(child[0], bars[0].b);
  }
  struct buf neutral = mk(W, H, WL_SHM_FORMAT_XRGB8888, 0xff112233);
  put(root, neutral.b);
  for (int i = 0; i < 3; i++)
    wd_texture_manager_v1_retire(mgr, o[i].b);
  wd_texture_manager_v1_finish(mgr);
  for (int j = 0; j < 4; j++) {
    capdone = 0;
    weston_capture_source_v1_capture(cs, out.b);
    waitfor(&capdone);
  }
  fprintf(stderr, "release counts %d %d %d\n", o[0].released, o[1].released,
          o[2].released);
  if (replace)
    assert(o[2].released == 1);
  wl_surface_destroy(child[anchor ? 0 : 3]);
  assert(wl_display_roundtrip(display) >= 0);
  fprintf(stderr, "after destroying cache surface %d %d %d\n", o[0].released,
          o[1].released, o[2].released);
  for (int i = 0; i < 3; i++) {
    waitfor(&o[i].released);
    assert(o[i].released == 1);
    wl_buffer_destroy(o[i].b);
    drop(o[i].held);
  }
  assert(wl_display_roundtrip(display) >= 0);
  puts("retired=3 released=3");
  wl_display_disconnect(display);
  return 0;
}
