#!/usr/bin/env python3
"""Map the FocalTech touchpad register space (read-only)."""
import sys
import time
import hid

PATH = "/dev/hidraw4"
CMD_READ = 0x50


def xor_checksum(report_bytes):
    c = 0
    for b in report_bytes[1:]:
        c ^= b
    return (c + 1) & 0xFF


def build_read_reg_report(addr):
    report = [0x06, 0xFF, 0xFF, 6, CMD_READ, addr]
    report.append(xor_checksum(report))
    return bytes(report)


def read_reg(dev, addr):
    rep = build_read_reg_report(addr)
    rep64 = rep + bytes(63 - len(rep))
    dev.send_feature_report(rep64)
    time.sleep(0.005)
    r = bytes(dev.get_feature_report(0x06, 63))
    if len(r) < 7 or r[4] != CMD_READ:
        return None
    return r[6]


def main():
    dev = hid.device()
    dev.open_path(PATH.encode())

    print("addr  val  | addr  val  | addr  val  | addr  val")
    vals = {}
    for a in range(0x00, 0x100):
        v = read_reg(dev, a)
        vals[a] = v

    for row in range(0, 0x40):
        cells = []
        for base in range(0, 0x100, 0x40):
            a = base + row
            v = vals.get(a)
            cells.append(f"0x{a:02X}  0x{v:02X}" if v is not None else f"0x{a:02X}  ??")
        print(" | ".join(cells))

    dev.close()


if __name__ == "__main__":
    sys.exit(main())
