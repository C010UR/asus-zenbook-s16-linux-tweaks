#!/usr/bin/env python3
"""Capture raw HID input reports from the touchpad for behavior analysis.

Timeline (total 24s):
  0-8s    : one finger, slow movements
  8-16s   : rest your palm on the pad, keep it still
  16-24s  : keep palm resting and drag it slowly
Prompts are printed with timestamps so frames can be correlated.
"""
import os
import sys
import time

PATH = sys.argv[2] if len(sys.argv) > 2 else "/dev/hidraw5"
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 24

FRAME_LEN = 50  # report 0x04 = 50 bytes incl. report id


def main():
    fd = os.open(PATH, os.O_RDONLY | os.O_NONBLOCK)
    print("capturing on %s for %ds" % (PATH, DURATION), flush=True)
    start = time.time()
    buf = b""
    last_elapsed = -1
    while True:
        now = time.time()
        elapsed = int(now - start)
        if elapsed >= DURATION:
            break
        if elapsed != last_elapsed:
            last_elapsed = elapsed
            if elapsed < 8:
                print("t=%ds >>> ONE FINGER: slow movements" % elapsed, flush=True)
            elif elapsed < 16:
                print("t=%ds >>> REST PALM: keep still" % elapsed, flush=True)
            else:
                print("t=%ds >>> PALM + drag slowly" % elapsed, flush=True)
        try:
            data = os.read(fd, 4096)
        except OSError:
            time.sleep(0.001)
            continue
        if not data:
            time.sleep(0.001)
            continue
        buf += data
        while True:
            idx = buf.find(b"\x04")
            if idx == -1:
                buf = b""
                break
            if idx + FRAME_LEN > len(buf):
                buf = buf[idx:]
                break
            f = buf[idx:idx + FRAME_LEN]
            buf = buf[idx + FRAME_LEN:]
            print("FRAME %.3f %s" % (now - start, f.hex()), flush=True)
    os.close(fd)
    print("done", flush=True)


if __name__ == "__main__":
    main()
