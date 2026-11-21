#include <EGL/Platform.h>
#include <pthread.h>
#include <string.h>

// The shim returns the same Mesa EGLDisplay for the Android physical display.
// Android writes its tracing callbacks into this table after obtaining it.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *owner;
static angle::PlatformMethods methods;

extern "C" bool ANGLEGetDisplayPlatform(void *display, const char *const names[],
                                        unsigned count, void *context, void *out) {
    if (!display || !names || !out || count != angle::g_NumPlatformMethods) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!names[i] || strcmp(names[i], angle::g_PlatformMethodNames[i])) return false;
    }
    pthread_mutex_lock(&lock);
    bool supported = !owner || (owner == display && methods.context == context);
    if (supported) {
        owner = display;
        methods.context = context;
        *static_cast<angle::PlatformMethods **>(out) = &methods;
    }
    pthread_mutex_unlock(&lock);
    return supported;
}

extern "C" void ANGLEResetDisplayPlatform(void *display) {
    pthread_mutex_lock(&lock);
    if (owner == display) {
        methods = angle::PlatformMethods{};
        owner = nullptr;
    }
    pthread_mutex_unlock(&lock);
}
