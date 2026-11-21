/* Included by Mesa's st_cb_readpixels.c. All GL state stays on its owner. */
#include "util/simple_mtx.h"
#include "util/u_queue.h"
#include "util/os_time.h"

struct texture_cache_import {
   struct st_context *st;
   void *owner;
   uint64_t id, nonce;
   unsigned serial;
   struct pipe_resource *res;
};
static simple_mtx_t imports_lock = SIMPLE_MTX_INITIALIZER;
static struct texture_cache_import imports[16];
static unsigned victim;

static void
texture_cache_clear(struct texture_cache_import *e)
{
   pipe_resource_reference(&e->res, NULL);
   *e = (struct texture_cache_import){0};
}

void texture_cache_forget_image(void *owner);
__attribute__((visibility("default"))) void
texture_cache_forget_image(void *owner)
{
   simple_mtx_lock(&imports_lock);
   for (unsigned i = 0; i < ARRAY_SIZE(imports); ++i)
      if (imports[i].owner == owner)
         texture_cache_clear(&imports[i]);
   simple_mtx_unlock(&imports_lock);
}

static struct pipe_resource *
texture_cache_lookup(struct st_context *st, void *owner, uint64_t id, uint64_t nonce,
           unsigned serial, int w, int h)
{
   struct pipe_resource *res = NULL;
   simple_mtx_lock(&imports_lock);
   for (unsigned i = 0; i < ARRAY_SIZE(imports); ++i) {
      struct texture_cache_import *e = &imports[i];
      if (e->st != st || e->owner != owner) continue;
      if (e->id == id && e->nonce == nonce && e->serial == serial &&
          e->res->screen == st->screen && e->res->width0 == w && e->res->height0 == h)
         pipe_resource_reference(&res, e->res);
      else
         texture_cache_clear(e);
      break;
   }
   simple_mtx_unlock(&imports_lock);
   return res;
}

static void
texture_cache_remember(struct st_context *st, void *owner, uint64_t id, uint64_t nonce,
             unsigned serial, struct pipe_resource *res)
{
   simple_mtx_lock(&imports_lock);
   struct texture_cache_import *e = &imports[victim++ % ARRAY_SIZE(imports)];
   for (unsigned i = 0; i < ARRAY_SIZE(imports); ++i)
      if (!imports[i].res) { e = &imports[i]; break; }
   texture_cache_clear(e);
   *e = (struct texture_cache_import){.st=st, .owner=owner, .id=id, .nonce=nonce, .serial=serial};
   pipe_resource_reference(&e->res, res);
   simple_mtx_unlock(&imports_lock);
}

/* The worker creates one never-published destination, with its own shared
 * handle. A handed-out destination never comes back into this slot. */
struct texture_cache_spare {
   struct util_queue queue;
   struct util_queue_fence done;
   struct pipe_screen *screen;
   struct pipe_resource desc;
   struct pipe_resource *res;
   int fd;
};

static void
texture_cache_allocate(void *job, void *unused, int index)
{
   struct texture_cache_spare *s = job;
   s->res = s->screen->resource_create(s->screen, &s->desc);
   struct winsys_handle handle = {.type = WINSYS_HANDLE_TYPE_FD};
   if (s->res && s->screen->resource_get_handle(s->screen, NULL, s->res, &handle,
                                                PIPE_HANDLE_USAGE_EXPLICIT_FLUSH))
      s->fd = handle.handle;
}

static struct pipe_resource *
texture_cache_take(struct st_context *st, const struct pipe_resource *desc, int *fd)
{
   struct texture_cache_spare *s = st->texture_cache_spare;
   if (!s) {
      s = calloc(1, sizeof(*s));
      if (!s) return NULL;
      if (!util_queue_init(&s->queue, "wd-spare", 1, 1, 0, NULL)) {
         free(s);
         return NULL;
      }
      s->screen = st->screen;
      s->fd = -1;
      util_queue_fence_init(&s->done);
      st->texture_cache_spare = s;
   }
   if (!util_queue_fence_is_signalled(&s->done)) return NULL;
   __atomic_thread_fence(__ATOMIC_ACQUIRE);
   struct pipe_resource *res = NULL;
   if (s->res && s->desc.width0 == desc->width0 && s->desc.height0 == desc->height0) {
      res = s->res;
      s->res = NULL;
      *fd = s->fd;
   } else {
      pipe_resource_reference(&s->res, NULL);
      if (s->fd >= 0) close(s->fd);
   }
   s->fd = -1;
   s->desc = *desc;
   util_queue_add_job(&s->queue, s, &s->done, texture_cache_allocate, NULL, 0);
   return res;
}

static void
texture_cache_stop_spare(struct st_context *st)
{
   struct texture_cache_spare *s = st->texture_cache_spare;
   if (!s) return;
   util_queue_fence_wait(&s->done);
   util_queue_destroy(&s->queue);
   util_queue_fence_destroy(&s->done);
   pipe_resource_reference(&s->res, NULL);
   if (s->fd >= 0) close(s->fd);
   free(s);
   st->texture_cache_spare = NULL;
}

void
texture_cache_context_finish(struct st_context *st)
{
   texture_cache_stop_spare(st);
   simple_mtx_lock(&imports_lock);
   for (unsigned i = 0; i < ARRAY_SIZE(imports); ++i)
      if (imports[i].st == st) texture_cache_clear(&imports[i]);
   simple_mtx_unlock(&imports_lock);
}
