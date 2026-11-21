/* 最小 system/graphics.h 替身：只为 gralloc0 实验提供 gralloc.h / 测试用到的
 * android_ycbcr 与几个像素格式常量。完整 AOSP 头文件依赖链太长。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    HAL_PIXEL_FORMAT_RGBA_8888 = 1,
    HAL_PIXEL_FORMAT_RGBX_8888 = 2,
    HAL_PIXEL_FORMAT_RGB_888   = 3,
    HAL_PIXEL_FORMAT_RGB_565   = 4,
    HAL_PIXEL_FORMAT_BGRA_8888 = 5,
    HAL_PIXEL_FORMAT_YV12      = 0x32315659,
};

#define PIXEL_FORMAT_RGBA_8888 HAL_PIXEL_FORMAT_RGBA_8888
#define PIXEL_FORMAT_RGBX_8888 HAL_PIXEL_FORMAT_RGBX_8888
#define PIXEL_FORMAT_RGB_888   HAL_PIXEL_FORMAT_RGB_888
#define PIXEL_FORMAT_RGB_565   HAL_PIXEL_FORMAT_RGB_565
#define PIXEL_FORMAT_BGRA_8888 HAL_PIXEL_FORMAT_BGRA_8888

typedef struct android_ycbcr {
    void    *y;
    void    *cb;
    void    *cr;
    size_t   ystride;
    size_t   cstride;
    size_t   chroma_step;
    uint32_t reserved[8];
} android_ycbcr;
