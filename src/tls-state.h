/* This storage belongs to the bionic-loaded EGL shim. The guest modules
 * retain their original PT_TLS images and resolver entry points. */
struct tls_state {
    void *blocks[TLS_MAX_MODS];
    unsigned registered, cleanup_round;
};
static __thread struct tls_state native_state;
static pthread_once_t cleanup_once = PTHREAD_ONCE_INIT;
static pthread_key_t cleanup_key;
static int cleanup_error;

static void destroy_tls_state(void *value) {
    struct tls_state *state = value;
    /* EGL and driver pthread-key destructors may still call a guest TLS
     * resolver during thread teardown. Keep the images through those passes. */
    if (++state->cleanup_round < PTHREAD_DESTRUCTOR_ITERATIONS &&
        pthread_setspecific(cleanup_key, state) == 0) return;
    for (unsigned i = 0; i < TLS_MAX_MODS; i++) {
        free(state->blocks[i]);
        state->blocks[i] = NULL;
    }
    state->registered = 0;
    state->cleanup_round = 0;
}

static void make_cleanup_key(void) {
    cleanup_error = pthread_key_create(&cleanup_key, destroy_tls_state);
}

static struct tls_state *cur_tls_state(void) {
    struct tls_state *state = &native_state;
    if (!state->registered) {
        if (pthread_once(&cleanup_once, make_cleanup_key) || cleanup_error ||
            pthread_setspecific(cleanup_key, state)) return NULL;
        state->registered = 1;
    }
    return state;
}
