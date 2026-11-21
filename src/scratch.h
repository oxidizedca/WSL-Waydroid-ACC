/* The fallback owns its scratch slot until present (or its optional writer) finishes.
 * Stable slot addresses allow independent windows to read back concurrently. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct sb_slot {
    void *win;
    unsigned char *b[2];
    size_t sz;
    int cur, ok[2], bw[2], bh[2];
    uint64_t tick;
    int busy, transient;
};

static pthread_mutex_t scratch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sb_slot scratch_slots[8];
static uint64_t scratch_tick;

static void release_scratch(struct sb_slot **lease)
{
    struct sb_slot *s = *lease;
    if (!s) return;
    if (s->transient) {
        free(s->b[0]);
        free(s->b[1]);
        free(s);
    } else {
        pthread_mutex_lock(&scratch_lock);
        s->busy = 0;
        pthread_mutex_unlock(&scratch_lock);
    }
    *lease = NULL;
}

static struct sb_slot *acquire_scratch(void *win, int w, int h)
{
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / 4 / (size_t)h) return NULL;
    size_t need = (size_t)w * h * 4;
    struct sb_slot *exact = NULL, *history = NULL, *empty_match = NULL, *oldest = NULL;
    pthread_mutex_lock(&scratch_lock);
    for (unsigned i = 0; i < sizeof(scratch_slots)/sizeof(scratch_slots[0]); ++i) {
        struct sb_slot *s = &scratch_slots[i];
        if (s->busy) continue;
        if (s->tick && s->win == win) { exact = s; break; }
        int prev = s->cur ^ 1;
        if (s->ok[prev] && s->bw[prev] == w && s->bh[prev] == h &&
            (!history || s->tick > history->tick)) history = s;
        /* Recycle an idle same-sized writer with no saved frame before
         * growing the pool for another short-lived window. */
        if (!s->ok[0] && !s->ok[1] && s->sz == need && s->b[s->cur] &&
            (!empty_match || s->tick < empty_match->tick)) empty_match = s;
        if (!oldest || s->tick < oldest->tick) oldest = s;
    }
    struct sb_slot *s = exact ? exact : history ? history : empty_match ? empty_match : oldest;
    if (s) {
        s->busy = 1;
        s->tick = ++scratch_tick;
    }
    pthread_mutex_unlock(&scratch_lock);

    /* A ninth concurrent reader must not evict an active writer. */
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) return NULL;
        s->transient = 1;
    }
    if (s->sz < need || (s != exact && s != history && s->sz != need)) {
        unsigned char *a = malloc(need);
        if (!a) {
            release_scratch(&s);
            return NULL;
        }
        free(s->b[0]);
        free(s->b[1]);
        s->b[0] = a;
        s->b[1] = NULL;
        s->sz = need;
        s->ok[0] = s->ok[1] = 0;
        s->cur = 0;
    }
    /* Only a successfully saved history flips cur. A clear-allowed window
     * can keep presenting with one allocation for its entire lifetime. */
    if (!s->b[s->cur] && !(s->b[s->cur] = malloc(s->sz))) {
        release_scratch(&s);
        return NULL;
    }
    if (s != exact && s != history) s->ok[0] = s->ok[1] = 0;
    s->win = win;
    return s;
}
