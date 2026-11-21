#include <system/window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

static int dataspace(int color, int format) {
    switch (color) {
    case EGL_GL_COLORSPACE_SRGB_KHR: return HAL_DATASPACE_V0_SRGB;
    case EGL_GL_COLORSPACE_DISPLAY_P3_EXT:
    case EGL_GL_COLORSPACE_DISPLAY_P3_PASSTHROUGH_EXT: return HAL_DATASPACE_DISPLAY_P3;
    case EGL_GL_COLORSPACE_DISPLAY_P3_LINEAR_EXT: return HAL_DATASPACE_DISPLAY_P3_LINEAR;
    case EGL_GL_COLORSPACE_SCRGB_EXT: return HAL_DATASPACE_V0_SCRGB;
    case EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT: return HAL_DATASPACE_V0_SCRGB_LINEAR;
    case EGL_GL_COLORSPACE_BT2020_PQ_EXT: return HAL_DATASPACE_BT2020_PQ;
    case EGL_GL_COLORSPACE_BT2020_HLG_EXT: return HAL_DATASPACE_BT2020_HLG;
    case EGL_GL_COLORSPACE_BT2020_LINEAR_EXT:
        if (format == HAL_PIXEL_FORMAT_RGBA_FP16)
            return HAL_DATASPACE_STANDARD_BT2020 | HAL_DATASPACE_TRANSFER_LINEAR | HAL_DATASPACE_RANGE_EXTENDED;
        return HAL_DATASPACE_BT2020_LINEAR;
    default: return HAL_DATASPACE_UNKNOWN;
    }
}

void shim_disconnect_window(struct ANativeWindow *window) {
    native_window_set_buffers_data_space(window, HAL_DATASPACE_UNKNOWN);
    native_window_set_buffers_format(window, 0);
    native_window_api_disconnect(window, NATIVE_WINDOW_API_EGL);
    window->common.decRef(&window->common);
}

int shim_connect_window(struct ANativeWindow *window, int format, int color) {
    if (native_window_api_connect(window, NATIVE_WINDOW_API_EGL)) return -1;
    window->common.incRef(&window->common);
    if (native_window_set_buffers_format(window, format) ||
        native_window_set_buffers_data_space(window, dataspace(color, format))) {
        shim_disconnect_window(window);
        return -1;
    }
    return 0;
}
