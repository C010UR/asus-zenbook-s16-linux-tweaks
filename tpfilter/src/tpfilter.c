// tpfilter - palm-rejection filter daemon for the ASUS Zenbook S16 touchpad
//
// Ports the palm-rejection behavior of the ASUS Precision Touchpad Windows
// driver (AsusPTPFilter.sys + AsusPTPService.exe, driver ver 16.0.0.41) to
// Linux. The touchpad firmware reports resting palms as 2-3 finger contacts;
// this daemon reads the touchpad's input events, detects palm contacts,
// drops them, and re-emits the filtered stream through a uinput device.
//
// Palm detection (calibrated for this touchpad / user):
//   (a) a contact at the far left/right edge (X < edge_pct% or X > 100-edge_pct%)
//       while another contact is active is a palm (typing posture); with exactly
//       two contacts the edge contact must be "resting" (idle for rest_ms) unless
//       edge_rest_ms is 0;
//   (b) two contacts within palm_spread of each other form one palm;
//   (c) a whole palm split into >= 3 slots in the bottom zone is a palm.
//   A contact classified as palm stays palm for palm_latch_ms (hysteresis) so
//   the classification does not flicker frame to frame.
//
// Click zones: the physical button (BTN_LEFT) is mapped to BTN_LEFT /
// BTN_MIDDLE / BTN_RIGHT from the finger X (left 39.5% / middle 21% /
// right 39.5%), replicating the Windows driver's click zones.
//
// Configuration is read from /etc/tpfilter.conf (key = value, '#' comments);
// command-line flags override it.
//
// Build:  cc -O2 -Wall -o tpfilter tpfilter.c $(pkg-config --cflags --libs libevdev)
// Usage:  tpfilter [--config PATH] [--device /dev/input/eventN] [--name NAME]
//                  [--no-fork] [--debug] [--verbose]
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <libevdev/libevdev.h>
#include <poll.h>
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
#define DEFAULT_CONFIG "/etc/tpfilter.conf"

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
    long long palm_since_us; /* time the palm latch started          */
};

struct tpfilter {
    struct libevdev *source;
    int uinput_fd;
    int nslots;
    int cur_slot;
    struct slot_state slots[MAX_SLOTS];

    /* tunables (from config file / CLI) */
    int edge_pct;       /* far-edge palm zone (% of width each side) */
    int palm_spread;    /* two contacts within this distance = one palm */
    int bottom_zone;    /* bottom zone for the >=3-slot palm rule */
    int rest_ms;        /* idle time before a contact counts as resting */
    int rest_move;      /* movement that resets the resting state */
    int edge_rest_ms;   /* require resting before dropping a 2-contact edge palm */
    int palm_latch_ms;  /* keep a palm classified after the rule stops matching */
    int buttonpad;      /* advertise INPUT_PROP_BUTTONPAD on the virtual device */

    int verbose;
    int debug;
    int active_cnt;
    long long now_us;
    long filtered_total;
    int click_zone;   /* active zone button (BTN_LEFT/MIDDLE/RIGHT) or 0 */
    char virt_name[80];
    char device_path[256];

    /* batched uinput output */
    struct input_event outbuf[64];
    int outlen;
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

static int parse_bool(const char *s)
{
    return !strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "yes") ||
           !strcmp(s, "on");
}

static void set_defaults(struct tpfilter *f)
{
    f->edge_pct = 15;
    f->palm_spread = 400;
    f->bottom_zone = 1200;
    f->rest_ms = 80;
    f->rest_move = 100;
    f->edge_rest_ms = 80;
    f->palm_latch_ms = 200;
    f->buttonpad = 1;
    f->verbose = 0;
    f->debug = 0;
    snprintf(f->virt_name, sizeof(f->virt_name), "%s", VIRTUAL_NAME);
    f->device_path[0] = 0;
}

static void load_config(struct tpfilter *f, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT)
            logmsg("cannot open config %s: %s", path, strerror(errno));
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '#' || *p == '\0')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *e = key + strlen(key);
        while (e > key && isspace((unsigned char)e[-1]))
            *--e = '\0';
        while (isspace((unsigned char)*val))
            val++;
        e = val + strlen(val);
        while (e > val && isspace((unsigned char)e[-1]))
            *--e = '\0';

        if (!strcmp(key, "device")) {
            snprintf(f->device_path, sizeof(f->device_path), "%s", val);
        } else if (!strcmp(key, "name")) {
            snprintf(f->virt_name, sizeof(f->virt_name), "%s", val);
        } else if (!strcmp(key, "edge_pct")) {
            f->edge_pct = atoi(val);
        } else if (!strcmp(key, "palm_spread")) {
            f->palm_spread = atoi(val);
        } else if (!strcmp(key, "bottom_zone")) {
            f->bottom_zone = atoi(val);
        } else if (!strcmp(key, "rest_ms")) {
            f->rest_ms = atoi(val);
        } else if (!strcmp(key, "rest_move")) {
            f->rest_move = atoi(val);
        } else if (!strcmp(key, "edge_rest_ms")) {
            f->edge_rest_ms = atoi(val);
        } else if (!strcmp(key, "palm_latch_ms")) {
            f->palm_latch_ms = atoi(val);
        } else if (!strcmp(key, "buttonpad")) {
            f->buttonpad = parse_bool(val);
        } else if (!strcmp(key, "debug")) {
            f->debug = parse_bool(val);
        } else if (!strcmp(key, "verbose")) {
            f->verbose = parse_bool(val);
        } else {
            logmsg("config %s: unknown key '%s'", path, key);
        }
    }
    fclose(fp);
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
    if (f->buttonpad)
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- event helpers ----------------------------------------------------- */

static void flush(struct tpfilter *f)
{
    if (f->outlen == 0)
        return;
    char *buf = (char *)f->outbuf;
    size_t len = (size_t)f->outlen * sizeof(struct input_event);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(f->uinput_fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = f->uinput_fd, .events = POLLOUT };
            poll(&pfd, 1, 100);
            continue;
        }
        if (n < 0) {
            logmsg("uinput write error: %s", strerror(errno));
            break;
        }
    }
    f->outlen = 0;
}

static void emit(struct tpfilter *f, int type, int code, int val)
{
    if (f->outlen >= (int)(sizeof(f->outbuf) / sizeof(f->outbuf[0])))
        flush(f);
    struct input_event *ev = &f->outbuf[f->outlen++];
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->code = code;
    ev->value = val;
}

static void emit_syn(struct tpfilter *f)
{
    emit(f, EV_SYN, SYN_REPORT, 0);
    flush(f);
}

static int in_bottom_zone(struct tpfilter *f, struct slot_state *s)
{
    return s->y >= Y_MAX - f->bottom_zone;
}

/* palms rest at the far left/right edges; fingers gesture in the middle */
static int at_far_edge(struct tpfilter *f, struct slot_state *s)
{
    return s->x < X_MAX * f->edge_pct / 100 ||
           s->x > X_MAX * (100 - f->edge_pct) / 100;
}

static int is_resting(struct tpfilter *f, struct slot_state *s)
{
    if (s->tracking_id < 0)
        return 0;
    long long idle = f->now_us - s->idle_us;
    return idle > f->rest_ms * 1000;
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

        /* contact at the far edge while another contact is active = palm.
         * With exactly two contacts, require the edge contact to be resting
         * (unless edge_rest_ms == 0) so a deliberately held edge finger is
         * not dropped. */
        if (nactive >= 2 && at_far_edge(f, s)) {
            if (nactive >= 3 || f->edge_rest_ms == 0 || is_resting(f, s))
                palm[i] = 1;
        }
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

    /* hysteresis: once a contact is palm, keep it palm for palm_latch_ms
     * after the rule stops matching, so the tracking id does not flicker. */
    for (int i = 0; i < f->nslots; i++) {
        struct slot_state *s = &f->slots[i];
        if (s->tracking_id < 0) {
            s->palm = 0;
            continue;
        }
        if (palm[i]) {
            if (!s->palm)
                s->palm_since_us = f->now_us;
            s->palm = 1;
        } else if (s->palm &&
                   f->now_us - s->palm_since_us < f->palm_latch_ms * 1000LL) {
            s->palm = 1;
        } else {
            s->palm = 0;
        }
    }
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

/* ---- source device open/close ------------------------------------------ */

static int open_source(struct tpfilter *f, const char *devpath)
{
    int fd;
    if (devpath) {
        fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 || libevdev_new_from_fd(fd, &f->source) < 0) {
            if (fd >= 0)
                close(fd);
            f->source = NULL;
            logmsg("cannot open device %s", devpath);
            return -1;
        }
    } else {
        fd = find_touchpad(&f->source);
        if (fd < 0) {
            logmsg("touchpad not found");
            return -1;
        }
    }
    if (libevdev_grab(f->source, LIBEVDEV_GRAB) != 0)
        logmsg("WARNING: could not grab source device; real touchpad still live");
    else
        logmsg("grabbed source device (exclusive)");
    return fd;
}

static void close_source(struct tpfilter *f, int fd)
{
    if (f->source) {
        libevdev_grab(f->source, LIBEVDEV_UNGRAB);
        libevdev_free(f->source);
        f->source = NULL;
    }
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv)
{
    struct tpfilter f;
    int fd;
    int no_fork = 0;
    const char *config_path = DEFAULT_CONFIG;

    memset(&f, 0, sizeof(f));
    f.cur_slot = 0;
    set_defaults(&f);
    for (int i = 0; i < MAX_SLOTS; i++)
        f.slots[i].tracking_id = -1;

    /* first pass: locate the config file, then apply CLI overrides */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config_path = argv[++i];
    }
    load_config(&f, config_path);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            snprintf(f.device_path, sizeof(f.device_path), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            snprintf(f.virt_name, sizeof(f.virt_name), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--no-fork")) {
            no_fork = 1;
        } else if (!strcmp(argv[i], "--verbose")) {
            f.verbose = 1;
        } else if (!strcmp(argv[i], "--debug")) {
            f.debug = 1;
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            i++; /* already handled in the first pass */
        }
    }

    f.nslots = 5;

    logmsg("touchpad: x[%d..%d] y[%d..%d] res=%d,%d slots=%d",
           X_MIN, X_MAX, Y_MIN, Y_MAX, X_RES, Y_RES, f.nslots);
    logmsg("palm: edge=%d%% bottom_zone=%d spread=%d rest=%dms/%d "
           "edge_rest=%dms latch=%dms buttonpad=%d",
           f.edge_pct, f.bottom_zone, f.palm_spread, f.rest_ms, f.rest_move,
           f.edge_rest_ms, f.palm_latch_ms, f.buttonpad);

    f.uinput_fd = create_virtual_device(&f);
    if (f.uinput_fd < 0)
        return 1;
    logmsg("virtual device created: %s", f.virt_name);

    if (!no_fork && daemon(0, 0) < 0)
        logmsg("daemon() failed: %s", strerror(errno));

    fd = open_source(&f, f.device_path[0] ? f.device_path : NULL);
    if (fd < 0)
        return 1;

    for (;;) {
        struct input_event ev;
        int rc = libevdev_next_event(f.source, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == -EAGAIN) {
            /* libevdev has no queued events and the fd is drained; block
             * until the device produces more. */
            struct pollfd pfd = {
                .fd = libevdev_get_fd(f.source),
                .events = POLLIN,
            };
            int prc = poll(&pfd, 1, -1);
            if (prc < 0) {
                if (errno == EINTR)
                    continue;
                logmsg("poll error: %s", strerror(errno));
                break;
            }
            if (prc == 0)
                continue;
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                logmsg("source device gone (revents=%#x), re-detecting...",
                       pfd.revents);
                close_source(&f, fd);
                for (;;) {
                    fd = open_source(&f, f.device_path[0] ? f.device_path : NULL);
                    if (fd >= 0)
                        break;
                    sleep(1);
                }
            }
            continue;
        }
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
        if (rc < 0) {
            if (rc == -ENODEV || rc == -EIO) {
                logmsg("source device read error (%s), re-detecting...",
                       strerror(-rc));
                close_source(&f, fd);
                for (;;) {
                    fd = open_source(&f, f.device_path[0] ? f.device_path : NULL);
                    if (fd >= 0)
                        break;
                    sleep(1);
                }
                continue;
            }
            logmsg("read error: %s", strerror(-rc));
            break;
        }

        if (ev.type == EV_SYN)
            f.now_us = now_us();

        if (f.verbose && ev.type == EV_SYN)
            logmsg("ev: SYN code=%d", ev.code);

        switch (ev.type) {
        case EV_ABS:
            switch (ev.code) {
            case ABS_MT_SLOT:
                f.cur_slot = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                f.slots[f.cur_slot].tracking_id = ev.value;
                if (ev.value >= 0) {
                    f.slots[f.cur_slot].idle_us = now_us();
                    f.slots[f.cur_slot].palm_since_us = 0;
                }
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
            flush(&f);
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
            flush(&f);
            break;
        case EV_MSC:
            flush(&f);
            if (ev.code == MSC_TIMESTAMP)
                emit(&f, EV_MSC, MSC_TIMESTAMP, ev.value);
            flush(&f);
            break;
        case EV_SYN:
            /* update rest state: track movement per slot */
            for (int i = 0; i < f.nslots; i++) {
                struct slot_state *s = &f.slots[i];
                if (s->tracking_id >= 0) {
                    int dx = abs(s->x - s->last_x);
                    int dy = abs(s->y - s->last_y);
                    if (dx + dy >= f.rest_move)
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

    close_source(&f, fd);
    ioctl(f.uinput_fd, UI_DEV_DESTROY);
    close(f.uinput_fd);
    return 0;
}