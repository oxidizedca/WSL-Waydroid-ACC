/* The vendor dispatches its default queue independently of HWC set. Use a
 * separate manager so its results cannot satisfy an ordinary synchronous offer. */
struct target_offer {
    struct wl_buffer *buffer;
    uint64_t allocation;
    uint32_t token, generation, producer;
    const hwc_procs_t *procs;
    bool rejected, released, retired, attached;
};
static struct wd_texture_manager_v1 *async_manager;
static struct target_offer offers[16];
static pthread_mutex_t offer_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_token;
static bool need_mirror;
static struct target_offer *cached_offer;
static __thread struct target_offer *submitting;
static __thread bool offer_used;

/* On rejection Weston never acquired the buffer and sends no release. In
 * either case keep the proxy until the HWC caller has marshalled its last use. */
static void collect_offer(struct target_offer *o)
{
    if (o->retired && (o->released || o->rejected)) {
        wl_buffer_destroy(o->buffer);
        memset(o, 0, sizeof(*o));
    }
}

static void target_release(void *data, struct wl_buffer *buffer)
{
    struct target_offer *o = data;
    pthread_mutex_lock(&offer_lock);
    o->released = true;
    collect_offer(o);
    pthread_mutex_unlock(&offer_lock);
}
static const struct wl_buffer_listener async_release_listener = {target_release};

static void target_result(void *data, struct wd_texture_manager_v1 *m, uint32_t token, uint32_t ok)
{
    if (ok) return;
    const hwc_procs_t *refresh = NULL;
    pthread_mutex_lock(&offer_lock);
    for (unsigned i = 0; i < 16; ++i) {
        struct target_offer *o = &offers[i];
        if (!o->buffer || o->token != token) continue;
        o->rejected = true;
        LOG("target-import rejected token=%u generation=%u attached=%d", token, o->generation, o->attached);
        if (o->attached) {
            need_mirror = true;
            refresh = o->procs;
        }
        collect_offer(o);
        break;
    }
    pthread_mutex_unlock(&offer_lock);
    if (refresh) refresh->invalidate(refresh);
}
static const struct wd_texture_manager_v1_listener async_listener = {target_result};

static void bind_async_manager(struct wl_registry *reg, uint32_t name)
{
    struct wl_registry *wrap = wl_proxy_create_wrapper(reg);
    if (!wrap) return;
    wl_proxy_set_queue((struct wl_proxy *)wrap, NULL);
    async_manager = wl_registry_bind(wrap, name, &wd_texture_manager_v1_interface, 3);
    wl_proxy_wrapper_destroy(wrap);
    if (async_manager)
        wd_texture_manager_v1_add_listener(async_manager, &async_listener, NULL);
}

static bool take_mirror_request(void)
{
    pthread_mutex_lock(&offer_lock);
    bool pending = need_mirror;
    need_mirror = false;
    pthread_mutex_unlock(&offer_lock);
    return pending;
}

static bool start_offer(const hwc_procs_t *procs, int fd, const struct frame_frame *p)
{
    if (!async_manager || !procs || !procs->invalidate) return false;
    pthread_mutex_lock(&offer_lock);
    if (cached_offer && !cached_offer->rejected && cached_offer->allocation == p->buffer_id &&
        cached_offer->generation == p->serial && cached_offer->producer == p->sample) {
        submitting = cached_offer;
        pthread_mutex_unlock(&offer_lock);
        return true;
    }
    for (unsigned i = 0; i < 16; ++i) {
        struct target_offer *o = &offers[i];
        if (o->buffer) continue;
        /* Reserve the low bits for the slot so a live token never aliases
         * another slot on wrap. Keep the producer generation for diagnostics. */
        o->token = (++next_token << 4) | i;
        o->generation = p->serial;
        o->producer = p->sample;
        o->allocation = p->buffer_id;
        o->procs = procs;
        o->buffer = wd_texture_manager_v1_prepare_target(async_manager, fd, p->width, p->height, o->token);
        if (o->buffer) {
            wl_buffer_add_listener(o->buffer, &async_release_listener, o);
            submitting = o;
        }
        break;
    }
    pthread_mutex_unlock(&offer_lock);
    return submitting != NULL;
}

static bool attach_offer(struct wl_buffer *carrier)
{
    pthread_mutex_lock(&offer_lock);
    bool ok = !submitting->rejected;
    if (ok) {
        /* v3 processes these in wire order before vendor attach. A successful
         * reuse clears the hold. A rejected reuse leaves it on this carrier. */
        wd_texture_manager_v1_hold_carrier(async_manager, carrier);
        wd_texture_manager_v1_reuse(async_manager, submitting->buffer, carrier);
        submitting->attached = true;
        offer_used = true;
    }
    pthread_mutex_unlock(&offer_lock);
    return ok;
}

static void retire_offer(struct target_offer *o)
{
    wd_texture_manager_v1_retire(async_manager, o->buffer);
    o->retired = true;
    collect_offer(o);
}

static void end_offer(void)
{
    if (!submitting && !cached_offer) return;
    pthread_mutex_lock(&offer_lock);
    if (cached_offer && cached_offer != submitting) retire_offer(cached_offer);
    cached_offer = NULL;
    if (submitting) {
        if (offer_used && !submitting->rejected) cached_offer = submitting;
        else retire_offer(submitting);
    }
    submitting = NULL;
    offer_used = false;
    pthread_mutex_unlock(&offer_lock);
}
