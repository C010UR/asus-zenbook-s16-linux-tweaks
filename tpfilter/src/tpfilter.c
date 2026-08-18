// tpfilter - palm-rejection filter daemon for the ASUS Zenbook S16 touchpad
//
// Ports the palm-rejection behavior of the ASUS Precision Touchpad Windows
// driver (AsusPTPFilter.sys + AsusPTPService.exe, driver ver 16.0.0.41) to
// Linux. The touchpad firmware reports resting palms as 2-3 finger contacts;
// this daemon reads the touchpad's input events, detects palm contacts,
// drops them, and re-emits the filtered stream through a uinput device.
//
// Palm detection (calibrated for this touchpad / user):
//   (a) a contact at the far left/right edge (X < 15% or X > 85%) while
//       another contact is active is a palm (typing posture);
//   (b) two contacts within palm_spread of each other form one palm;
//   (c) a whole palm split into >= 3 slots in the bottom zone is a palm.
//
// Click zones: the physical button (BTN_LEFT) is mapped to BTN_LEFT /
// BTN_MIDDLE / BTN_RIGHT from the finger X (left 39.5% / middle 21% /
// right 39.5%), replicating the Windows driver's click zones.
//
// Build:  cc -O2 -Wall -o tpfilter tpfilter.c $(pkg-config --cflags --libs libevdev)
// Usage:  tpfilter [--device /dev/input/eventN] [--name NAME] [--no-fork]
//                  [--debug] [--verbose]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <libevdev/libevdev.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_SLOTS 5
#define VIRTUAL_NAME "ASUF1209 Palm Filtered Touchpad"
#define EDGE_PCT 15   /* far-edge palm zone (% of width each side) */
#define PALM_SPREAD 400

/* Original touchpad geometry from the HID report descriptor (unclamped). */
#define X_MIN 0
#define X_MAX 4762
#define Y_MIN 0
#define Y_MAX 3099
#define X_RES 32
#define Y_RES 32

struct slot_state {
    int tracking_id;   /* device tracking id, -1 if contact released */
    int x, y;          /* latest contact position                   */
    int last_x, last_y;/* position from previous frame               */
    int visible;       /* userspace currently sees this slot        */
    int palm;          /* classified as palm this frame             */
    long long idle_us; /* monotonic time of last significant move    */
};

struct tpfilter {
    struct libevdev *source;
    int uinput_fd;
    int nslots;
    int cur_slot;
    struct slot_state slots[MAX_SLOTS];
    int bottom_zone;
    int palm_spread;
    int verbose;
    int debug;
    int active_cnt;
    long long now_us;
    long filtered_total;
    int click_zone;   /* active zone button (BTN_LEFT/MIDDLE/RIGHT) or 0 */
    char virt_name[80];
};

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void logmsg(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int find_touchpad(struct libevdev **out)
{
    for (int i = 0; i < 32; i++) {
        char path[64];
        int fd;
        struct libevdev *dev = NULL;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (libevdev_new_from_fd(fd, &dev) == 0) {
            const char *name = libevdev_get_name(dev);
            if (name && strstr(name, "ASUF1209") &&
                strstr(name, "Touchpad")) {
                *out = dev;
                return fd;
            }
            libevdev_free(dev);
        }
        close(fd);
    }
    return -1;
}

/* ---- uinput setup ------------------------------------------------------ */

static void ui_abs(int fd, int code, int min, int max, int res)
{
    struct uinput_abs_setup abs;
    memset(&abs, 0, sizeof(abs));
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.resolution = res;
    ioctl(fd, UI_ABS_SETUP, &abs);
}

static void ui_set(int fd, unsigned long evbit, unsigned long bit)
{
    int req;
    switch (evbit) {
    case EV_KEY: req = UI_SET_KEYBIT; break;
    case EV_ABS: req = UI_SET_ABSBIT; break;
    case EV_MSC: req = UI_SET_MSCBIT; break;
    case EV_REL: req = UI_SET_RELBIT; break;
    default: return;
    }
    if (ioctl(fd, UI_SET_EVBIT, evbit) < 0)
        logmsg("UI_SET_EVBIT %lu failed: %s", evbit, strerror(errno));
    if (ioctl(fd, req, bit) < 0)
        logmsg("set bit %lu failed: %s", bit, strerror(errno));
}

static int create_virtual_device(struct tpfilter *f)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        logmsg("cannot open /dev/uinput: %s", strerror(errno));
        return -1;
    }
    ui_set(fd, EV_KEY, BTN_LEFT);
    ui_set(fd, EV_KEY, BTN_RIGHT);
    ui_set(fd, EV_KEY, BTN_MIDDLE);
    ui_set(fd, EV_KEY, BTN_TOUCH);
    ui_set(fd, EV_KEY, BTN_TOOL_FINGER);
    ui_set(fd, EV_KEY, BTN_TOOL_DOUBLETAP);
    ui_set(fd, EV_KEY, BTN_TOOL_TRIPLETAP);
    ui_set(fd, EV_KEY, BTN_TOOL_QUADTAP);
    ui_set(fd, EV_KEY, BTN_TOOL_QUINTTAP);

    ui_set(fd, EV_ABS, ABS_X);
    ui_set(fd, EV_ABS, ABS_Y);
    ui_set(fd, EV_ABS, ABS_MT_SLOT);
    ui_set(fd, EV_ABS, ABS_MT_POSITION_X);
    ui_set(fd, EV_ABS, ABS_MT_POSITION_Y);
    ui_set(fd, EV_ABS, ABS_MT_TRACKING_ID);
    ui_set(fd, EV_ABS, ABS_MT_TOOL_TYPE);
    ui_set(fd, EV_MSC, MSC_TIMESTAMP);

    ui_abs(fd, ABS_X, X_MIN, X_MAX, X_RES);
    ui_abs(fd, ABS_Y, Y_MIN, Y_MAX, Y_RES);
    ui_abs(fd, ABS_MT_SLOT, 0, f->nslots - 1, 0);
    ui_abs(fd, ABS_MT_POSITION_X, X_MIN, X_MAX, X_RES);
    ui_abs(fd, ABS_MT_POSITION_Y, Y_MIN, Y_MAX, Y_RES);
    ui_abs(fd, ABS_MT_TRACKING_ID, 0, 65535, 0);
    ui_abs(fd, ABS_MT_TOOL_TYPE, 0, 2, 0);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), "%s", f->virt_name);
    setup.id.bustype = BUS_I2C;
    setup.id.vendor = 0x2808;
    setup.id.product = 0x0219;
    setup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        logmsg("UI_DEV_SETUP failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- event helpers ----------------------------------------------------- */

static void emit(struct tpfilter *f, int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = val;
    write(f->uinput_fd, &ev, sizeof(ev));
}

static void emit_syn(struct tpfilter *f)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    write(f->uinput_fd, &ev, sizeof(ev));
}

static int in_bottom_zone(struct tpfilter *f, struct slot_state *s)
{
    return s->y >= Y_MAX - f->bottom_zone;
}

/* palms rest at the far left/right edges; fingers gesture in the middle */
static int at_far_edge(struct slot_state *s)
{
    return s->x < X_MAX * EDGE_PCT / 100 || s->x > X_MAX * (100 - EDGE_PCT) / 100;
}

#define REST_MS 80
#define REST_MOVE 100  /* units of allowed drift while "resting" */

static int is_resting(struct tpfilter *f, struct slot_state *s)
{
    if (s->tracking_id < 0)
        return 0;
    long long idle = f->now_us - s->idle_us;
    return idle > REST_MS * 1000;
}

static void classify(struct tpfilter *f)
{
    static unsigned char palm[MAX_SLOTS];
    int nbottom = 0, nactive = 0;

    for (int i = 0; i < f->nslots; i++) {
        palm[i] = 0;
        if (f->slots[i].tracking_id >= 0) {
            nactive++;
            if (in_bottom_zone(f, &f->slots[i]))
                nbottom++;
        }
    }
    f->active_cnt = nactive;

    for (int i = 0; i < f->nslots; i++) {
        struct slot_state *s = &f->slots[i];
        if (s->tracking_id < 0)
            continue;

        /* contact at the far edge while another contact is active = palm */
        if (nactive >= 2 && at_far_edge(s))
            palm[i] = 1;
        /* whole palm split into >=3 slots */
        if (nactive >= 3 && nbottom >= 3)
            palm[i] = 1;
    }

    /* any two contacts within palm_spread of each other = one palm */
    for (int i = 0; i < f->nslots; i++) {
        if (f->slots[i].tracking_id < 0)
            continue;
        for (int j = i + 1; j < f->nslots; j++) {
            if (f->slots[j].tracking_id < 0)
                continue;
            int dx = f->slots[i].x - f->slots[j].x;
            int dy = f->slots[i].y - f->slots[j].y;
            if (dx * dx + dy * dy < f->palm_spread * f->palm_spread) {
                palm[i] = 1;
                palm[j] = 1;
            }
        }
    }

    for (int i = 0; i < f->nslots; i++)
        f->slots[i].palm = palm[i];
}

static void forward_tool_buttons(struct tpfilter *f, int nvisible)
{
    emit(f, EV_KEY, BTN_TOUCH, nvisible > 0 ? 1 : 0);
    emit(f, EV_KEY, BTN_TOOL_FINGER, nvisible == 1 ? 1 : 0);
    emit(f, EV_KEY, BTN_TOOL_DOUBLETAP, nvisible == 2 ? 1 : 0);
    emit(f, EV_KEY, BTN_TOOL_TRIPLETAP, nvisible == 3 ? 1 : 0);
    emit(f, EV_KEY, BTN_TOOL_QUADTAP, nvisible == 4 ? 1 : 0);
    emit(f, EV_KEY, BTN_TOOL_QUINTTAP, nvisible == 5 ? 1 : 0);
}

/* X position of the primary (first non-palm) contact for click zoning */
static int primary_finger_x(struct tpfilter *f)
{
    for (int i = 0; i < f->nslots; i++) {
        if (f->slots[i].tracking_id >= 0 && !f->slots[i].palm)
            return f->slots[i].x;
    }
    return X_MAX / 2;
}

/* map a finger X to a click zone button (left 39.5% / middle 21% / right 39.5%) */
static int click_zone_for_x(int x)
{
    if (x < X_MAX * 395 / 1000)
        return BTN_LEFT;
    if (x < X_MAX * 605 / 1000)
        return BTN_MIDDLE;
    return BTN_RIGHT;
}

static void handle_syn(struct tpfilter *f)
{
    int nvisible = 0;
    int npalm = 0;

    classify(f);

    for (int i = 0; i < f->nslots; i++) {
        struct slot_state *s = &f->slots[i];
        if (s->tracking_id >= 0) {
            if (s->palm)
                npalm++;
            else
                nvisible++;
        }
    }
    if (npalm > 0)
        f->filtered_total++;

    if (f->debug && f->active_cnt > 0) {
        char line[512], tmp[96];
        line[0] = 0;
        for (int i = 0; i < f->nslots; i++) {
            struct slot_state *s = &f->slots[i];
            if (s->tracking_id < 0)
                continue;
            snprintf(tmp, sizeof(tmp), " s%d=%s(%d,%d)%s",
                     i, s->palm ? "PALM" : "fng ", s->x, s->y,
                     is_resting(f, s) ? "*" : "");
            strncat(line, tmp, sizeof(line) - strlen(line) - 1);
        }
        logmsg("cnt=%d%s [filtered=%ld]", f->active_cnt, line,
               f->filtered_total);
    }

    for (int i = 0; i < f->nslots; i++) {
        struct slot_state *s = &f->slots[i];
        if (s->tracking_id >= 0) {
            if (!s->palm) {
                emit(f, EV_ABS, ABS_MT_SLOT, i);
                if (!s->visible) {
                    emit(f, EV_ABS, ABS_MT_TRACKING_ID, s->tracking_id);
                    emit(f, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
                }
                emit(f, EV_ABS, ABS_MT_POSITION_X, s->x);
                emit(f, EV_ABS, ABS_MT_POSITION_Y, s->y);
                s->visible = 1;
            } else {
                if (s->visible) {
                    emit(f, EV_ABS, ABS_MT_SLOT, i);
                    emit(f, EV_ABS, ABS_MT_TRACKING_ID, -1);
                    s->visible = 0;
                }
            }
        } else {
            if (s->visible) {
                emit(f, EV_ABS, ABS_MT_SLOT, i);
                emit(f, EV_ABS, ABS_MT_TRACKING_ID, -1);
                s->visible = 0;
            }
        }
    }

    int have_xy = 0;
    for (int i = 0; i < f->nslots && !have_xy; i++) {
        if (f->slots[i].visible) {
            emit(f, EV_ABS, ABS_X, f->slots[i].x);
            emit(f, EV_ABS, ABS_Y, f->slots[i].y);
            have_xy = 1;
        }
    }

    forward_tool_buttons(f, nvisible);
    emit_syn(f);
}

int main(int argc, char **argv)
{
    struct tpfilter f;
    const char *devpath = NULL;
    int fd;
    int no_fork = 0;

    memset(&f, 0, sizeof(f));
    f.cur_slot = 0;
    snprintf(f.virt_name, sizeof(f.virt_name), "%s", VIRTUAL_NAME);
    for (int i = 0; i < MAX_SLOTS; i++)
        f.slots[i].tracking_id = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            devpath = argv[++i];
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            snprintf(f.virt_name, sizeof(f.virt_name), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--no-fork")) {
            no_fork = 1;
        } else if (!strcmp(argv[i], "--verbose")) {
            f.verbose = 1;
        } else if (!strcmp(argv[i], "--debug")) {
            f.debug = 1;
        }
    }

    if (devpath) {
        fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 || libevdev_new_from_fd(fd, &f.source) < 0) {
            logmsg("cannot open device %s", devpath);
            return 1;
        }
    } else {
        fd = find_touchpad(&f.source);
        if (fd < 0) {
            logmsg("touchpad not found");
            return 1;
        }
    }

    f.nslots = 5;
    f.bottom_zone = 1200;   /* bottom zone for the >=3-slot palm rule */
    f.palm_spread = PALM_SPREAD;

    logmsg("touchpad: x[%d..%d] y[%d..%d] res=%d,%d slots=%d",
           X_MIN, X_MAX, Y_MIN, Y_MAX, X_RES, Y_RES, f.nslots);
    logmsg("palm: edge=%d%% bottom_zone=%d spread=%d",
           EDGE_PCT, f.bottom_zone, f.palm_spread);

    f.uinput_fd = create_virtual_device(&f);
    if (f.uinput_fd < 0) {
        libevdev_free(f.source);
        close(fd);
        return 1;
    }
    logmsg("virtual device created: %s", f.virt_name);

    if (!no_fork && daemon(0, 0) < 0)
        logmsg("daemon() failed: %s", strerror(errno));

    if (libevdev_grab(f.source, LIBEVDEV_GRAB) != 0)
        logmsg("WARNING: could not grab source device; real touchpad still live");
    else
        logmsg("grabbed source device (exclusive)");

    for (;;) {
        struct input_event ev;
        int rc = libevdev_next_event(f.source, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            /* device was grabbed while another reader had it open:
             * kernel sent SYN_DROPPED, libevdev is in sync mode.
             * Drain the state snapshot, then resume normal reading. */
            while (libevdev_next_event(f.source, LIBEVDEV_READ_FLAG_SYNC, &ev)
                   == LIBEVDEV_READ_STATUS_SYNC)
                ;
            logmsg("synced after SYN_DROPPED");
            continue;
        }
        if (rc < 0 && rc != -EAGAIN) {
            logmsg("read error: %s", strerror(-rc));
            break;
        }
        if (rc == -EAGAIN)
            continue;

        f.now_us = now_us();

        if (f.verbose) {
            if (ev.type == EV_SYN) {
                logmsg("ev: SYN code=%d", ev.code);
            } else if (ev.type == EV_ABS) {
                logmsg("ev: ABS code=%d val=%d (slot=%d)", ev.code, ev.value,
                       f.cur_slot);
            } else if (ev.type == EV_KEY) {
                logmsg("ev: KEY code=%#x val=%d", ev.code, ev.value);
            }
        }

        switch (ev.type) {
        case EV_ABS:
            switch (ev.code) {
            case ABS_MT_SLOT:
                f.cur_slot = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                f.slots[f.cur_slot].tracking_id = ev.value;
                if (ev.value >= 0)
                    f.slots[f.cur_slot].idle_us = f.now_us;
                break;
            case ABS_MT_POSITION_X:
                f.slots[f.cur_slot].x = ev.value;
                break;
            case ABS_MT_POSITION_Y:
                f.slots[f.cur_slot].y = ev.value;
                break;
            }
            break;
        case EV_KEY:
            if (ev.code == BTN_LEFT) {
                if (ev.value == 1) {
                    f.click_zone = click_zone_for_x(primary_finger_x(&f));
                    if (f.debug)
                        logmsg("click: x=%d -> zone=%#x", primary_finger_x(&f),
                               f.click_zone);
                    emit(&f, EV_KEY, f.click_zone, 1);
                } else if (ev.value == 0) {
                    if (f.click_zone)
                        emit(&f, EV_KEY, f.click_zone, 0);
                    f.click_zone = 0;
                }
            } else if (ev.code != BTN_TOUCH && ev.code != BTN_TOOL_FINGER &&
                       ev.code != BTN_TOOL_DOUBLETAP &&
                       ev.code != BTN_TOOL_TRIPLETAP &&
                       ev.code != BTN_TOOL_QUADTAP &&
                       ev.code != BTN_TOOL_QUINTTAP) {
                emit(&f, EV_KEY, ev.code, ev.value);
            }
            break;
        case EV_MSC:
            if (ev.code == MSC_TIMESTAMP)
                emit(&f, EV_MSC, MSC_TIMESTAMP, ev.value);
            break;
        case EV_SYN:
            /* update rest state: track movement per slot */
            for (int i = 0; i < f.nslots; i++) {
                struct slot_state *s = &f.slots[i];
                if (s->tracking_id >= 0) {
                    int dx = abs(s->x - s->last_x);
                    int dy = abs(s->y - s->last_y);
                    if (dx + dy >= 4)
                        s->idle_us = f.now_us;
                    s->last_x = s->x;
                    s->last_y = s->y;
                }
            }
            if (ev.code == SYN_REPORT)
                handle_syn(&f);
            break;
        }
    }

    libevdev_grab(f.source, LIBEVDEV_UNGRAB);
    ioctl(f.uinput_fd, UI_DEV_DESTROY);
    close(f.uinput_fd);
    libevdev_free(f.source);
    close(fd);
    return 0;
}
