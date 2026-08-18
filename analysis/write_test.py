#!/usr/bin/env python3
"""Safe write tests on the FocalTech touchpad.

Tests WriteRegister round-trip on unused registers (restores original value).
Writes a benign pattern to the 128-byte vendor report and reads it back.
"""
import sys
import time
import hid

PATH = "/dev/hidraw4"
CMD_READ = 0x50
CMD_WRITE = 0x51


def xor_checksum(report_bytes):
    c = 0
    for b in report_bytes[1:]:
        c ^= b
    return (c + 1) & 0xFF


def build_cmd_report(cmd, payload):
    body = [cmd] + list(payload)
    report = [0x06, 0xFF, 0xFF, len(body) + 2, cmd] + list(payload)
    report.append(xor_checksum(report))
    return bytes(report)


def send_cmd(dev, buf):
    rep = buf + bytes(63 - len(buf))
    dev.send_feature_report(rep)


def read_reg(dev, addr):
    send_cmd(dev, build_cmd_report(CMD_READ, [addr]))
    time.sleep(0.01)
    r = bytes(dev.get_feature_report(0x06, 63))
    return r[6] if len(r) >= 7 and r[4] == CMD_READ else None


def write_reg(dev, addr, val):
    send_cmd(dev, build_cmd_report(CMD_WRITE, [addr, val]))
    time.sleep(0.01)
    r = bytes(dev.get_feature_report(0x06, 63))
    return r


def main():
    dev = hid.device()
    dev.open_path(PATH.encode())

    print("== WriteRegister round-trip on unused registers ==")
    for addr in [0x7E, 0x7D, 0x0A]:
        orig = read_reg(dev, addr)
        print(f"  reg 0x{addr:02X}: orig=0x{orig:02X}")
        if orig is None:
            continue
        test = 0x00 if orig != 0x00 else 0x55
        write_reg(dev, addr, test)
        after = read_reg(dev, addr)
        print(f"    wrote 0x{test:02X} -> read 0x{after:02X}")
        # restore
        write_reg(dev, addr, orig)
        restored = read_reg(dev, addr)
        print(f"    restored -> 0x{restored:02X}")

    print("\n== write/read 128-byte vendor report 0x0B ==")
    # write a benign pattern
    pattern = bytes([0x0B] + [0x5A] * 127)
    try:
        n = dev.send_feature_report(pattern)
        print(f"  wrote report 0x0B: {n} bytes")
    except Exception as e:
        print(f"  write report 0x0B failed: {e}")
    time.sleep(0.02)
    try:
        r = bytes(dev.get_feature_report(0x0B, 128))
        print(f"  read back: {r[:16].hex()}... len={len(r)}")
    except Exception as e:
        print(f"  read report 0x0B failed: {e}")

    print("\n== write/read report 0x0C (c6+c7) ==")
    try:
        n = dev.send_feature_report(bytes([0x0C] + [0x00] * 33))
        print(f"  wrote report 0x0C: {n} bytes")
    except Exception as e:
        print(f"  write report 0x0C failed: {e}")
    time.sleep(0.02)
    try:
        r = bytes(dev.get_feature_report(0x0C, 33))
        print(f"  read back: {r[:16].hex()}... len={len(r)}")
    except Exception as e:
        print(f"  read report 0x0C failed: {e}")

    dev.close()


if __name__ == "__main__":
    sys.exit(main())
