// Validation only: compress the identical SDR framebuffer with Android's bitmap encoder.
#include <android/bitmap.h>
#include <android/data_space.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static bool output(void *ctx, const void *buf, size_t n) { return fwrite(buf,1,n,ctx)==n; }
int main(void) {
    uint32_t h[4];
    if (fread(h,1,16,stdin)!=16 || h[0]!=1920 || h[1]!=1080 || h[2]!=1 || h[3]!=1) return 1;
    size_t n=h[0]*h[1]*4;
    void *p=malloc(n);
    if (!p || fread(p,1,n,stdin)!=n) return 1;
    AndroidBitmapInfo info={.width=h[0],.height=h[1],.stride=4*h[0],
        .format=ANDROID_BITMAP_FORMAT_RGBA_8888,.flags=ANDROID_BITMAP_FLAGS_ALPHA_PREMUL};
    return AndroidBitmap_compress(&info,ADATASPACE_SRGB,p,ANDROID_BITMAP_COMPRESS_FORMAT_PNG,
                                  100,stdout,output)!=0 || fflush(stdout)!=0;
}
