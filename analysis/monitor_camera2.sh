#!/bin/bash
# Monitor camera disable mechanism - press Fn+F10 during the window.
# Usage: sudo bash monitor_camera2.sh
echo "=== initial ==="
echo "privacy: $(v4l2-ctl -d /dev/video0 --get-ctrl privacy 2>/dev/null)"
echo "camera authorized: $(cat /sys/bus/usb/devices/1-1/authorized 2>/dev/null)"
echo "camera runtime: $(cat /sys/bus/usb/devices/1-1/power/runtime_status 2>/dev/null)"
echo ""

# Find asus-wmi hotkeys device
for i in $(seq 0 31); do
    name=$(cat /sys/class/input/event$i/device/name 2>/dev/null)
    if [ "$name" = "Asus WMI hotkeys" ]; then
        DEV=/dev/input/event$i
        break
    fi
done

echo "watching $DEV (no grab) for 25s - PRESS Fn+F10 a few times"
echo ""

# Watch key events WITHOUT grab (so system still processes them)
timeout 25 evtest "$DEV" 2>/dev/null | grep --line-buffered "KEY_CAMERA\|KEY_MICMUTE\|MSC_SCAN" &
EVTEST_PID=$!

for t in $(seq 1 25); do
    sleep 1
    p=$(v4l2-ctl -d /dev/video0 --get-ctrl privacy 2>/dev/null | grep -o '[0-9]*$')
    a=$(cat /sys/bus/usb/devices/1-1/authorized 2>/dev/null)
    r=$(cat /sys/bus/usb/devices/1-1/power/runtime_status 2>/dev/null)
    mic=$(cat /sys/class/leds/platform::micmute/brightness 2>/dev/null)
    echo "t=${t}s privacy=$p auth=$a runtime=$r micled=$mic"
done

kill $EVTEST_PID 2>/dev/null
echo "=== done ==="