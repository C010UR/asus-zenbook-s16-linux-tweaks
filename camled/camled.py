#!/usr/bin/env python3
"""Watch the webcam privacy control and drive the ASUS camera LED.

When Fn+F10 disables the camera, the UVC driver sets the v4l2 privacy
control to 1; this daemon mirrors that state onto the asus::camera LED.
It reacts instantly to KEY_CAMERA events, then self-corrects against
the privacy control so the LED always matches the camera state.
"""
import fcntl
import glob
import os
import select
import struct
import sys
import time

V4L2_CID_PRIVACY = 0x009a0910
KEY_CAMERA = 212

VIDIOC_G_CTRL = 0xC008561B

LED_PATH = "/sys/class/leds/asus::camera/brightness"
POLL_INTERVAL = 0.2
VERIFY_INTERVAL = 1.0

EV_KEY = 0x01


def get_privacy(fd):
    ctrl = struct.pack("ii", V4L2_CID_PRIVACY, 0)
    try:
        res = fcntl.ioctl(fd, VIDIOC_G_CTRL, ctrl)
        _, value = struct.unpack("ii", res)
        return value
    except OSError:
        return None


def find_webcam():
    for path in sorted(glob.glob("/dev/video*")):
        try:
            fd = os.open(path, os.O_RDONLY)
            try:
                if get_privacy(fd) is not None:
                    return path
            finally:
                os.close(fd)
        except OSError:
            continue
    return None


def find_hotkeys():
    for path in sorted(glob.glob("/dev/input/event*")):
        try:
            with open(f"/sys/class/input/{os.path.basename(path)}/device/name") as f:
                name = f.read().strip()
            if "Asus WMI hotkeys" in name:
                return path
        except OSError:
            continue
    return None


def set_led(state):
    try:
        with open(LED_PATH, "w") as f:
            f.write("1" if state else "0")
    except OSError as e:
        print(f"camled: led write failed: {e}", file=sys.stderr)


def main():
    cam = find_webcam()
    if not cam:
        print("camled: no webcam with privacy control found", file=sys.stderr)
        sys.exit(1)
    print(f"camled: watching {cam}", file=sys.stderr)

    cam_fd = os.open(cam, os.O_RDONLY)
    hotkeys = find_hotkeys()
    if hotkeys:
        print(f"camled: watching {hotkeys} for KEY_CAMERA", file=sys.stderr)
        hk_fd = os.open(hotkeys, os.O_RDONLY)
    else:
        print("camled: hotkeys device not found, polling", file=sys.stderr)
        hk_fd = None

    last = get_privacy(cam_fd)
    if last is not None:
        set_led(last)
    last_verify = time.monotonic()

    while True:
        if hk_fd is not None:
            r, _, _ = select.select([hk_fd], [], [], POLL_INTERVAL)
            if hk_fd in r:
                data = os.read(hk_fd, 4096)
                for i in range(0, len(data) - len(data) % 24, 24):
                    ev = data[i:i + 24]
                    _, _, etype, code, value = struct.unpack("qqHHi", ev)
                    if etype == EV_KEY and code == KEY_CAMERA and value == 1:
                        time.sleep(0.05)
                        p = get_privacy(cam_fd)
                        if p is not None and p != last:
                            last = p
                            set_led(p)
                continue

        now = time.monotonic()
        if now - last_verify >= VERIFY_INTERVAL:
            p = get_privacy(cam_fd)
            if p is not None and p != last:
                last = p
                set_led(p)
            last_verify = now
        time.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    main()