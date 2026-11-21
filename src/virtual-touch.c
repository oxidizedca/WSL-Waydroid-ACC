// WSL side. Keep the uinput fd alive; actual touch events come from evdev clients.
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void check(int result, const char *operation) {
    if (result < 0) {
        perror(operation);
        exit(EXIT_FAILURE);
    }
}
static void axis(int fd, unsigned short code, int maximum) {
    struct uinput_abs_setup setup = {.code = code, .absinfo.maximum = maximum};
    check(ioctl(fd, UI_SET_ABSBIT, code), "UI_SET_ABSBIT");
    check(ioctl(fd, UI_ABS_SETUP, &setup), "UI_ABS_SETUP");
}
static int dimension(const char *key, int fallback) {
    const char *s = getenv(key);
    if (!s) return fallback;
    char *end;
    long n = strtol(s, &end, 10);
    if (!*s || *end || n < 128 || n > 8192) {
        fprintf(stderr, "Invalid %s\n", key);
        exit(EXIT_FAILURE);
    }
    return (int)n;
}
int main(void) {
    int xmax = dimension("ACC_WIDTH", 1920) - 1;
    int ymax = dimension("ACC_HEIGHT", 1080) - 1;
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    check(fd, "open /dev/uinput");
    check(ioctl(fd, UI_SET_EVBIT, EV_KEY), "EV_KEY");
    check(ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH), "BTN_TOUCH");
    check(ioctl(fd, UI_SET_EVBIT, EV_ABS), "EV_ABS");
    check(ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT), "INPUT_PROP_DIRECT");
    axis(fd, ABS_X, xmax); axis(fd, ABS_Y, ymax);
    axis(fd, ABS_MT_SLOT, 9); axis(fd, ABS_MT_TRACKING_ID, 65535);
    axis(fd, ABS_MT_POSITION_X, xmax); axis(fd, ABS_MT_POSITION_Y, ymax);
    axis(fd, ABS_MT_PRESSURE, 255);
    struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x1234,
        .product = 0x1080, .version = 1}, .name = "Waydroid ACC Multitouch"};
    check(ioctl(fd, UI_DEV_SETUP, &setup), "UI_DEV_SETUP");
    check(ioctl(fd, UI_DEV_CREATE), "UI_DEV_CREATE");
    char sysname[64] = {0};
    check(ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname), "UI_GET_SYSNAME");
    FILE *file = fopen("/run/waydroid-acc-touch/sysname", "we");
    if (!file) { perror("sysname"); return EXIT_FAILURE; }
    check(fprintf(file, "%s\n", sysname), "write sysname"); check(fclose(file), "close sysname");
    for (;;) pause();
}
