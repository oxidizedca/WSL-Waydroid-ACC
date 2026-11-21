/* The EGL client can drop its GraphicBuffer as soon as eglCreateImage returns.
 * Keep one strong reference for each CPU lock, including locks moved to the
 * texture cache. The final unlock must run before the final release. */
static int lock_mapping(img_t *im)
{
    if (im->is_ahb) {
        if (!g_ahb_acquire || !g_ahb_release || !g_ahb_unlock ||
            (!g_ahb_lock_info && !g_ahb_lock)) return 0;
        g_ahb_acquire(im->buffer);
        int bpp = 0, row = 0;
        void *addr = NULL;
        int rc = g_ahb_lock_info ?
            g_ahb_lock_info(im->buffer, 0x33, -1, NULL, &addr, &bpp, &row) :
            g_ahb_lock(im->buffer, 0x33, -1, NULL, &addr);
        if (rc || !addr) {
            if (!rc) g_ahb_unlock(im->buffer, NULL);
            g_ahb_release(im->buffer);
            return 0;
        }
        if (row > 0 && bpp > 0) im->stride = row / bpp;
        im->vaddr = addr;
        im->ahb_locked = 1;
    } else {
        anb_t *anb = im->buffer;
        if (!im->handle || !g_gm || !anb->incRef || !anb->decRef) return 0;
        anb->incRef(anb);
        const int usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
        int rc = g_gm->lock(g_gm, im->handle, usage, 0, 0, im->w, im->h, (void **)&im->vaddr);
        if (rc || !im->vaddr) {
            if (!rc) g_gm->unlock(g_gm, im->handle);
            anb->decRef(anb);
            return 0;
        }
    }
    im->owns_mapping = 1;
    return 1;
}

static void drop_mapping(void *buffer, void *handle, int is_ahb)
{
    if (is_ahb) {
        g_ahb_unlock(buffer, NULL);
        g_ahb_release(buffer);
    } else {
        anb_t *anb = buffer;
        g_gm->unlock(g_gm, handle);
        anb->decRef(anb);
    }
}
