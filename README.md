# ASUS Zenbook S16 Linux fixes

Two small tools that fix annoyances on the ASUS Zenbook S16 (UM5606) under Linux.

## 1. `tpfilter` — touchpad palm rejection

The touchpad firmware reports resting palms as 2–3 finger contacts, so the
cursor jumps while typing. This daemon reads the touchpad, drops palm
contacts, and re-emits the filtered stream through a virtual device.

- Palm rules: far-edge contact while another finger is down, two contacts
  close together, or a palm split across 3+ slots in the bottom zone.
- Click zones: the physical button maps to left / middle / right by finger
  position (39.5% / 21% / 39.5%), like the Windows driver.

**Setup:**

```sh
cd tpfilter
make
sudo make install
sudo systemctl enable --now tpfilter
```

Requires `libevdev` (`sudo pacman -S libevdev`). The service runs
`/usr/local/bin/tpfilter --no-fork` and survives reboots.

## 2. `camled` — camera privacy LED

Fn+F10 disables the camera (via the UVC privacy control) but never lit the
LED. The LED is driven by the Embedded Controller through an ASUS WMI
devid, gated by firmware flags. This provides a kernel module that does the
EC dance, plus a daemon that mirrors the camera's privacy state onto the
LED.

**Setup:**

```sh
cd camled
make CC=clang LLVM=1
sudo make dkms
sudo make install
sudo systemctl enable --now camled
```

Requires the kernel headers for your running kernel (the kernel is
clang-built, hence `CC=clang LLVM=1`) and `dkms`. The module is built and
installed via DKMS, so it is **rebuilt automatically on every kernel
update**. The service loads the module with `modprobe` and runs
`/usr/local/bin/camled.py`.

## Notes

- Both services are enabled, so they start automatically on boot.
- `tpfilter` is pure userspace and survives kernel updates untouched.
- `camled` is a DKMS module, so it is rebuilt automatically on kernel
  updates (and works on the LTS kernel too).
- `analysis/` contains the reverse-engineering notes (DSDT disassembly,
  register maps, monitor scripts) that led to these fixes.
- The Windows driver package (`PrecisionTouchPad_*.exe`) is kept for
  reference; it is not needed at runtime.