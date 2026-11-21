/* Included by egl_image.c. All accesses are serialized by image_lock. */
static struct {
    img_t *owner;
    EGLDisplay display;
    EGLContext context;
    EGLImage image;
    unsigned char *pixels;
    EGLBoolean (*destroy)(EGLDisplay,EGLImage);
} wallpaper;

static void forget_wallpaper(void)
{
    if (wallpaper.image) wallpaper.destroy(wallpaper.display,wallpaper.image);
    free(wallpaper.pixels);
    memset(&wallpaper,0,sizeof wallpaper);
}

/* Serialize display teardown with binds. The underlying EGLImage belongs to
 * this display; release it before forwarding termination to the driver. */
EGLBoolean egl_img_terminate(EGLDisplay d,EGLBoolean (*terminate)(EGLDisplay))
{
    if (!frame_surfaceflinger()) return terminate(d);
    LOCK_IMAGES();
    if (wallpaper.owner && wallpaper.display==d) forget_wallpaper();
    return terminate(d);
}

EGLBoolean egl_img_destroy_context(EGLDisplay d,EGLContext c,
                                   EGLBoolean (*destroy)(EGLDisplay,EGLContext))
{
    if (!frame_surfaceflinger()) return destroy(d,c);
    LOCK_IMAGES();
    if (wallpaper.owner && wallpaper.display==d && wallpaper.context==c)
        forget_wallpaper();
    return destroy(d,c);
}

/* Only accept the tightly packed RGBA8 BLAST wallpaper DMA-BUF. Keep one
 * image process-wide, bounded to 16 MiB of CPU pixels. */
static int wallpaper_plane(img_t *im)
{
    if (im->wallpaper_fd_tag) return im->wallpaper_fd_tag-1;
    im->wallpaper_fd_tag=-1;
    if (im->stride!=im->w || im->format!=1 || im->w<=0 || im->h<=0 ||
        (uint64_t)im->w*im->h>4194304) return -1;
    const native_handle_t *h=im->handle ? im->handle : ahb_native_handle(im->buffer);
    if (!h || h->numFds<1) return -1;
    char path[64],link[160];
    snprintf(path,sizeof path,"/proc/self/fd/%d",h->data[0]);
    ssize_t n=readlink(path,link,sizeof(link)-1);
    if (n<=0 || (size_t)n>=sizeof(link)-1) return -1;
    link[n]=0;
    unsigned queue,consumer;
    int end=0;
    struct stat st;
    if (sscanf(link,"/dmabuf:Wallpaper#%u(BLAST Consumer)%u%n",&queue,&consumer,&end)!=2 ||
        end!=n || fstat(h->data[0],&st) || st.st_size!=(off_t)im->w*im->h*4) return -1;
    int fd=fcntl(h->data[0],F_DUPFD_CLOEXEC,3);
    if (fd>=0) im->wallpaper_fd_tag=fd+1;
    return fd;
}

enum wallpaper_action { WALL_PASSTHROUGH, WALL_UPLOAD, WALL_REBIND };
static enum wallpaper_action wallpaper_pixels(img_t *im,EGLDisplay d,EGLContext ctx,
                                               const void *source,
                                               EGLBoolean (*destroy)(EGLDisplay,EGLImage))
{
    int fd=wallpaper_plane(im);
    if (fd<0) return WALL_PASSTHROUGH;
    size_t bytes=(size_t)im->w*im->h*4;
    if (wallpaper.owner!=im || wallpaper.context!=ctx || wallpaper.display!=d) {
        forget_wallpaper();
        wallpaper.pixels=malloc(bytes);
        if (!wallpaper.pixels) return WALL_PASSTHROUGH;
        wallpaper.owner=im;
        wallpaper.context=ctx;
        wallpaper.display=d;
        wallpaper.destroy=destroy;
    }
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
    if (ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync)) {
        forget_wallpaper();
        return WALL_PASSTHROUGH;
    }
    int same=wallpaper.image && !memcmp(source,wallpaper.pixels,bytes);
    if (!same) memcpy(wallpaper.pixels,source,bytes);
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;
    if (ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync)) {
        forget_wallpaper();
        return WALL_PASSTHROUGH;
    }
    if (same) return WALL_REBIND;
    if (wallpaper.image) destroy(d,wallpaper.image);
    wallpaper.image=NULL;
    return WALL_UPLOAD;
}
