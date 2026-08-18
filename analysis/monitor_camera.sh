#!/bin/bash
# Monitor what happens when Fn+F9/Fn+F10 are pressed.
# Usage: sudo bash monitor_camera.sh
echo "=== initial state ==="
echo "camera USB authorized: $(cat /sys/bus/usb/devices/1-1/authorized 2>/dev/null)"
echo "camera USB power: $(cat /sys/bus/usb/devices/1-1/power/control 2>/dev/null)"
echo "camera USB runtime: $(cat /sys/bus/usb/devices/1-1/power/runtime_status 2>/dev/null)"
echo "micmute LED: $(cat /sys/class/leds/platform::micmute/brightness 2>/dev/null)"
echo "camera LED: $(ls /sys/class/leds/ | grep -i cam || echo 'none')"
echo ""
echo "=== watching for 30s - press Fn+F9 and Fn+F10 ==="
echo ""

# Watch the asus-wmi hotkeys input device
for i in $(seq 0 31); do
    name=$(cat /sys/class/input/event$i/device/name 2>/dev/null)
    if [ "$name" = "Asus WMI hotkeys" ]; then
        DEV=/dev/input/event$i
        echo "watching $DEV ($name)"
        break
    fi
done

# Monitor in background: key events
timeout 30 evtest --grab "$DEV" 2>/dev/null | while read -r line; do
    echo "KEY: $line"
done &
EVTEST_PID=$!

# Monitor camera + LED state
for t in $(seq 1 30); do
    sleep 1
    auth=$(cat /sys/bus/usb/devices/1-1/authorized 2>/dev/null)
    mic=$(cat /sys/class/leds/platform::micmute/brightness 2>/dev/null)
    camled=$(ls /sys/class/leds/ 2>/dev/null | grep -i cam || echo "none")
    echo "t=${t}s auth=$auth micled=$mic camled=$camled"
done

kill $EVTEST_PID 2>/dev/null
echo "=== done ==="