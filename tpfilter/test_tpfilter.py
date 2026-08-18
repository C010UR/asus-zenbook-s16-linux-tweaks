#!/usr/bin/env python3
"""Self-contained test for tpfilter using a synthetic uinput source device.

Creates a fake touchpad (uinput), spawns tpfilter on it, emits synthetic
events (finger move, palm + finger), and reports what the virtual output
device delivers.
"""
import ctypes
import fcntl
import os
import struct
import subprocess
import sys
import time


def _ioc(dir_, type_, nr, size):
    return (dir_ << 30) | (ord(type_) << 8) | (nr) | (size << 16)


def _io(type_, nr):
    return _ioc(0, type_, nr, 0)


def _iow(type_, nr, size):
    return _ioc(1, type_, nr, size)


# uinput ioctls (computed from /usr/include/linux/uinput.h)
UI_DEV_CREATE = _io('U', 1)
UI_DEV_SETUP = _iow('U', 3, 92)   # sizeof(struct uinput_setup)
UI_ABS_SETUP = _iow('U', 4, 28)   # sizeof(struct uinput_abs_setup)
UI_SET_EVBIT = _iow('U', 100, 4)
UI_SET_KEYBIT = _iow('U', 101, 4)
UI_SET_RELBIT = _iow('U', 102, 4)
UI_SET_ABSBIT = _iow('U', 103, 4)
UI_SET_MSCBIT = _iow('U', 104, 4)
UI_SET_PROPBIT = _iow('U', 110, 4)

EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
EV_MSC = 0x04
SYN_REPORT = 0
BTN_LEFT = 0x110
BTN_TOUCH = 0x14a
BTN_TOOL_FINGER = 0x145
BTN_TOOL_DOUBLETAP = 0x14d
BTN_TOOL_TRIPLETAP = 0x14e
BTN_TOOL_QUADTAP = 0x14f
BTN_TOOL_QUINTTAP = 0x148
ABS_X = 0x00
ABS_Y = 0x01
ABS_MT_SLOT = 0x2f
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39
ABS_MT_TOOL_TYPE = 0x37
MSC_TIMESTAMP = 0x05

KEY_CAPS = {BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
            BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP}
ABS_CAPS = {
    ABS_X: (0, 4762, 32),
    ABS_Y: (0, 3099, 32),
    ABS_MT_SLOT: (0, 4, 0),
    ABS_MT_POSITION_X: (0, 4762, 32),
    ABS_MT_POSITION_Y: (0, 3099, 32),
    ABS_MT_TRACKING_ID: (0, 65535, 0),
    ABS_MT_TOOL_TYPE: (0, 2, 0),
}

NAME_SOURCE = "TPTEST Source Touchpad"


class UinputAbsSetup(ctypes.Structure):
    _fields_ = [("code", ctypes.c_ushort),
                ("reserved", ctypes.c_ushort),
                ("absinfo", ctypes.c_int * 6)]


class UinputDevSetup(ctypes.Structure):
    _fields_ = [("id", ctypes.c_ushort * 4),
                ("name", ctypes.c_char * 80),
                ("ff_effects_max", ctypes.c_uint)]


def make_abs_setup(code, minv, maxv, res):
    a = UinputAbsSetup()
    a.code = code
    a.reserved = 0
    a.absinfo[0] = 0      # value
    a.absinfo[1] = minv   # minimum
    a.absinfo[2] = maxv   # maximum
    a.absinfo[3] = 0      # fuzz
    a.absinfo[4] = 0      # flat
    a.absinfo[5] = res    # resolution
    return bytes(a)


def make_dev_setup(name, bustype, vendor, product, version):
    d = UinputDevSetup()
    d.id = (bustype, vendor, product, version)
    d.name = name.encode()[:79]
    d.ff_effects_max = 0
    return bytes(d)


def set_bit(fd, req, bit):
    fcntl.ioctl(fd, req, int(bit))


def create_uinput(name):
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_ABS, EV_MSC, EV_SYN):
        set_bit(fd, UI_SET_EVBIT, ev)
    for k in KEY_CAPS:
        set_bit(fd, UI_SET_KEYBIT, k)
    for code in ABS_CAPS:
        set_bit(fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(fd, UI_ABS_SETUP, make_abs_setup(code, *ABS_CAPS[code]))
    set_bit(fd, UI_SET_MSCBIT, MSC_TIMESTAMP)
    fcntl.ioctl(fd, UI_DEV_SETUP, make_dev_setup(name, 0x18, 0x2808, 0x0219, 1))
    set_bit(fd, UI_SET_PROPBIT, 0x01)  # INPUT_PROP_POINTER
    set_bit(fd, UI_SET_PROPBIT, 0x04)  # INPUT_PROP_BUTTONPAD
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def find_event_node(name):
    for dev in os.listdir("/sys/class/input/"):
        p = "/sys/class/input/%s/name" % dev
        if not os.path.exists(p):
            continue
        with open(p) as f:
            if f.read().strip() == name:
                # find the event device
                for ev in os.listdir("/sys/class/input/%s/" % dev):
                    if ev.startswith("event"):
                        return "/dev/input/" + ev
    return None


def ev(type, code, value):
    return struct.pack("llHHi", 0, 0, type, code, value)


def emit(fd, events):
    for e in events:
        os.write(fd, e)
    os.write(fd, ev(EV_SYN, SYN_REPORT, 0))
    time.sleep(0.01)


def mt_frame(fd, slots):
    """emit an MT frame: for each active slot emit slot+trackingid+x+y."""
    out = []
    for sid in slots:
        out.append(ev(EV_ABS, ABS_MT_SLOT, sid))
        out.append(ev(EV_ABS, ABS_MT_TRACKING_ID, slots[sid][0]))
        out.append(ev(EV_ABS, ABS_MT_POSITION_X, slots[sid][1]))
        out.append(ev(EV_ABS, ABS_MT_POSITION_Y, slots[sid][2]))
    emit(fd, out)


def main():
    print("creating source uinput...")
    src_fd = create_uinput(NAME_SOURCE)
    time.sleep(0.3)
    src_node = find_event_node(NAME_SOURCE)
    print("source node:", src_node)

    print("starting tpfilter on", src_node)
    import uuid
    vname = "TPFILTER-TEST-%s" % uuid.uuid4().hex[:8]
    proc = subprocess.Popen(
        [sys.argv[1] if len(sys.argv) > 1 else "./tpfilter",
         "--device", src_node, "--no-fork", "--debug", "--name", vname],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    time.sleep(0.5)

    # grab source (tpfilter does it, but we also want to make sure we write)
    virt_node = None
    for _ in range(20):
        virt_node = find_event_node(vname)
        if virt_node:
            break
        time.sleep(0.2)
    print("virtual node:", virt_node)
    if not virt_node:
        print("FAIL: virtual device not found")
        proc.terminate()
        out = proc.communicate(timeout=3)[0].decode()
        print("=== tpfilter output ===")
        print(out)
        return 1

    # emit synthetic events
    print("\n--- one finger move (expect finger visible, no PALM) ---")
    mt_frame(src_fd, {0: (100, 2000, 1500)})
    for x in range(2000, 2200, 40):
        mt_frame(src_fd, {0: (100, x, 1500)})

    print("\n--- palm (far-left edge) + finger mid-pad ---")
    mt_frame(src_fd, {0: (200, 200, 2700), 1: (201, 3500, 1500)})
    time.sleep(0.1)
    mt_frame(src_fd, {0: (200, 200, 2700), 1: (201, 3500, 1500)})
    time.sleep(0.2)

    print("\n--- release all ---")
    mt_frame(src_fd, {})
    time.sleep(0.2)

    proc.terminate()
    out = proc.communicate(timeout=3)[0].decode()
    print("\n=== tpfilter output ===")
    print(out)
    os.close(src_fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
