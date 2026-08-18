#!/usr/bin/env python3
"""Read-only probe of the FocalTech touchpad vendor command interface (hidapi).

Sends read-only commands (ReadRegister) and dumps responses. Never writes
firmware or enters upgrade mode.
"""
import sys
import time
import hid

PATH = "/dev/hidraw4"

FW1 = 0xA6
FW2 = 0xAD
VID1 = 0x9F
VID2 = 0xA3

CMD_READ = 0x50
CMD_WRITE = 0x51


def xor_checksum(report_bytes):
    c = 0
    for b in report_bytes[1:]:
        c ^= b
    return (c + 1) & 0xFF


def build_read_reg_report(addr, pad=0xFF):
    report = [0x06, pad, pad, 6, CMD_READ, addr]
    report.append(xor_checksum(report))
    return bytes(report)


def main():
    print("== opening", PATH)
    dev = hid.device()
    try:
        dev.open_path(PATH.encode())
    except Exception as e:
        print("open failed:", e)
        return 1
    print("manufacturer:", dev.get_manufacturer_string())
    print("product:", dev.get_product_string())

    print("\n== baseline feature report reads ==")
    for rid, size in [(0x0B, 128), (0x0C, 33), (0x0D, 4), (0x0E, 4), (0x06, 63)]:
        try:
            r = dev.get_feature_report(rid, size)
            print(f"  report 0x{rid:02X} ({size}B):", bytes(r).hex())
        except Exception as e:
            print(f"  report 0x{rid:02X}: ERROR {e}")

    print("\n== ReadRegister via send_feature_report (report 0x06) ==")
    for addr, name in [(FW1, "FwVersion1"), (FW2, "FwVersion2"), (VID1, "VerifyId1"), (VID2, "VerifyId2")]:
        rep = build_read_reg_report(addr)
        rep64 = rep + bytes(63 - len(rep))
        try:
            n = dev.send_feature_report(rep64)
            print(f"  send 0x{addr:02X} ({name}): ok ({n} bytes)")
        except Exception as e:
            print(f"  send 0x{addr:02X} ({name}): ERROR {e}")
        time.sleep(0.02)
        try:
            r = dev.get_feature_report(0x06, 63)
            print(f"    resp: {bytes(r).hex()}")
        except Exception as e:
            print(f"    resp: ERROR {e}")

    print("\n== ReadRegister via output write (device.write) ==")
    for addr, name in [(FW1, "FwVersion1"), (FW2, "FwVersion2")]:
        rep = build_read_reg_report(addr)
        rep63 = rep + bytes(62 - len(rep))
        try:
            n = dev.write(rep63)
            print(f"  write 0x{addr:02X} ({name}): ok ({n} bytes)")
        except Exception as e:
            print(f"  write 0x{addr:02X} ({name}): ERROR {e}")
        time.sleep(0.05)
        try:
            r = dev.read(256, timeout_ms=50)
            print(f"    input: {bytes(r).hex()}")
        except Exception as e:
            print(f"    input: ERROR {e}")

    print("\n== re-read vendor feature reports after commands ==")
    for rid, size in [(0x0B, 128), (0x0C, 33), (0x0D, 4), (0x0E, 4)]:
        try:
            r = dev.get_feature_report(rid, size)
            print(f"  report 0x{rid:02X}: {bytes(r).hex()}")
        except Exception as e:
            print(f"  report 0x{rid:02X}: ERROR {e}")

    dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
