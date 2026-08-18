/* grab_probe.c - minimal test: grab a device and report whether events arrive */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/input/event13";
    int secs = argc > 2 ? atoi(argv[2]) : 8;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return 1;
    }
    if (ioctl(fd, EVIOCGRAB, 1) != 0) {
        fprintf(stderr, "EVIOCGRAB failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "grabbed %s, listening %ds...\n", path, secs);
    int count = 0;
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        struct input_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == sizeof(ev)) {
            count++;
            if (count <= 20)
                fprintf(stderr, "ev type=%d code=%d value=%d\n",
                        ev.type, ev.code, ev.value);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(20000);
        } else if (n < 0) {
            fprintf(stderr, "read error: %s\n", strerror(errno));
            break;
        }
    }
    fprintf(stderr, "total events in %ds: %d\n", secs, count);
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    return 0;
}
