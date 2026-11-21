/* HWUI uploads a whole decoded bitmap into a temporary texture attached to
 * an Android buffer. A Mesa shadow texture does not share its storage. */
#include <sys/prctl.h>

static pthread_mutex_t bitmap_lock = PTHREAD_MUTEX_INITIALIZER;
static img_t *bitmap_uploads;
static uint64_t bitmap_serial;
static _Thread_local uint64_t pending_bitmap;

static void unlink_bitmap(img_t *im)
{
    img_t **p = &bitmap_uploads;
    while (*p && *p != im) p = &(*p)->bitmap_next;
    if (*p) *p = im->bitmap_next;
    im->bitmap_next = NULL;
}

static void forget_bitmap(img_t *im)
{
    if (!im->bitmap_candidate) return;
    pthread_mutex_lock(&bitmap_lock);
    unlink_bitmap(im);
    pthread_mutex_unlock(&bitmap_lock);
}

static void track_bitmap(img_t *im, unsigned texture)
{
    if (!texture || !im->bitmap_candidate || !im->vaddr) return;
    /* Android exposes the upload thread's name through Linux's 16-byte comm. */
    char name[16];
    if (prctl(PR_GET_NAME, name, 0, 0, 0) ||
        strcmp(name, "GrallocUploadTh")) return;

    void *ctx = cur_ctx();
    pthread_mutex_lock(&bitmap_lock);
    unlink_bitmap(im);
    im->bitmap_texture = texture;
    im->bitmap_context = ctx;
    pending_bitmap = im->bitmap_serial = ++bitmap_serial;
    im->bitmap_next = bitmap_uploads;
    bitmap_uploads = im;
    pthread_mutex_unlock(&bitmap_lock);
}

void egl_img_bitmap_subimage(unsigned target, int level, int x, int y, int w, int h,
                             unsigned format, unsigned type, const void *pixels)
{
    if (!pending_bitmap || !pixels || target != GL_TEXTURE_2D_ || level || x || y ||
        w <= 0 || h <= 0 || format != GL_RGBA_ || type != GL_UNSIGNED_BYTE_) return;
    LOCK_IMAGES();
    int texture, pbo, alignment, row_length, skip_rows, skip_pixels;
    gl_get_integerv(GL_TEXTURE_BINDING_2D_, &texture);
    gl_get_integerv(GL_PIXEL_UNPACK_BUFFER_BINDING_, &pbo);
    if (pbo) return;
    gl_get_integerv(0x0cf5, &alignment);
    gl_get_integerv(GL_UNPACK_ROW_LENGTH_, &row_length);
    gl_get_integerv(0x0cf3, &skip_rows);
    gl_get_integerv(0x0cf4, &skip_pixels);
    if (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) return;
    size_t pitch = ((size_t)(row_length ? row_length : w) * 4 + alignment - 1)
                   & ~(size_t)(alignment - 1);
    void *ctx = cur_ctx();
    pthread_mutex_lock(&bitmap_lock);
    img_t *im = bitmap_uploads;
    while (im && im->bitmap_serial != pending_bitmap) im = im->bitmap_next;
    if (!im) pending_bitmap = 0;
    if (im && im->bitmap_context == ctx && im->bitmap_texture == (unsigned)texture &&
        im->w == w && im->h == h && im->stride >= w) {
        const unsigned char *src = (const unsigned char *)pixels +
                                   (size_t)skip_rows * pitch + (size_t)skip_pixels * 4;
        for (int row = 0; row < h; ++row)
            memcpy(im->vaddr + (size_t)row * im->stride * 4, src + row * pitch, (size_t)w * 4);
        static int logged;
        if (logged < 12) {
            ++logged;
            printf("[bitmap] published %dx%d stride=%d", w, h, im->stride);
        }
        unlink_bitmap(im);
        pending_bitmap = 0;
    }
    pthread_mutex_unlock(&bitmap_lock);
}
