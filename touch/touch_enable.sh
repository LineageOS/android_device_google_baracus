#!/system/bin/sh
# Enable all RAPT Touch input devices
# The hazelred touch controller creates input devices that are disabled by default
for d in /sys/class/input/event*/device; do
    name=$(cat "$d/name" 2>/dev/null)
    case "$name" in
        *RAPT*)
            echo 1 > "$d/enabled" 2>/dev/null
            ;;
    esac
done
