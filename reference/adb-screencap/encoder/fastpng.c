// Android x86_64: fast path for the tested opaque, sRGB RGBA8888 screencap format.
// Exit 75 before writing anything if not supported; never retry a partial stdout.
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static void be32(unsigned char *p, uint32_t v) {
    v = htonl(v);
    memcpy(p, &v, 4);
}
static size_t chunk(unsigned char *p, const char type[4], size_t n) {
    be32(p, n); memcpy(p + 4, type, 4);
    be32(p + 8 + n, crc32(0, p + 4, n + 4));
    return n + 12;
}
static int write_all(unsigned char *p, size_t n) {
    while (n) {
        ssize_t k = write(1, p, n > 262144 ? 262144 : n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 1;
        p += k; n -= k;
    }
    return 0;
}
int main(void) {
    uint32_t h[4];
    if (fread(h,1,sizeof h,stdin) != sizeof h) return 1;
    if (!((h[0]==1920 && h[1]==1080) || (h[0]==1080 && h[1]==1920)) ||
        h[2]!=1 || h[3]!=1) return 75;
    size_t stride = (size_t)h[0]*4, bytes=(stride+1)*h[1];
    unsigned char *rows=malloc(bytes);
    if (!rows) return 75;
    for (unsigned y=0; y<h[1]; ++y) {
        unsigned char *row=rows+(stride+1)*y;
        if (fread(row+1,1,stride,stdin)!=stride) return 1;
        row[0]=1;
        unsigned char opaque=255;
        for (size_t i=4;i<=stride;i+=4) opaque &= row[i];
        if (opaque!=255) return 75;
        // PNG Sub, computed backwards to use the original left-hand pixel.
        for (size_t i=stride;i>4;--i) row[i]-=row[i-4];
    }
    if (fgetc(stdin)!=EOF || ferror(stdin)) return 1;
    uLongf len=compressBound(bytes);
    unsigned char *png=malloc(len+128);
    if (!png) return 75;
    memcpy(png,"\x89PNG\r\n\x1a\n",8);
    unsigned char *p=png+8;
    be32(p+8,h[0]); be32(p+12,h[1]); memcpy(p+16,"\x08\x06\0\0\0",5);
    p+=chunk(p,"IHDR",13);
    memset(p+8,8,4); p+=chunk(p,"sBIT",4);
    p[8]=0; p+=chunk(p,"sRGB",1);
    if (compress2(p+8,&len,rows,bytes,Z_BEST_SPEED)!=Z_OK) return 75;
    free(rows);
    p+=chunk(p,"IDAT",len); p+=chunk(p,"IEND",0);
    return write_all(png,p-png);
}
