#!/bin/bash
# T07 hardware verification — fw bootstrap stage I (ISSI, boot pages give,
# fw_initializing wait, HOST_ENDIANNESS, QUERY_HCA_CAP probe; teardown on
# rmmod reclaims pages + DISABLE_HCA. MSI-X stays masked, EQ untouched).
#
# Output: /tmp/t07_run.log ; crash recorder: /var/tmp/t07_live.log
# (disk-backed, survives reboot/power loss; every line synced).
#
# Usage: sudo bash scripts/t07_verify.sh [loops]
#   loops=1 (default): single load/unload cycle with full dmesg capture.
#   loops=N          : N insmod/rmmod cycles — the stress leg of the exit
#                      criteria; stops early if dmesg shows WARNING/Oops/BUG.
LOOPS=${1:-1}

LOG=/tmp/t07_run.log
LIVE=/var/tmp/t07_live.log
exec > >(tee "$LOG") 2>&1
cd /home/zqc/code/prism

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r line; do
        printf '%s\n' "$line" >> "$LIVE"
        sync
done &
LIVE_PID=$!

echo "### unload mlx5_core"
modprobe -r mlx5_ib 2>/dev/null
modprobe -r mlx5_core && echo "mlx5_core unloaded"

for n in $(seq 1 "$LOOPS"); do
        echo "### cycle $n/$LOOPS: insmod"
        dmesg -c > /dev/null
        insmod mlx5_prism.ko && echo "=== INSMOD OK ==="
        sync

        if [ "$LOOPS" -le 1 ]; then
                echo "### dmesg"
                dmesg | grep mlx5_prism
                echo "### /proc/interrupts (mlx5_prism)"
                grep mlx5_prism /proc/interrupts
        fi

        echo "### cycle $n/$LOOPS: rmmod"
        rmmod mlx5_prism && echo "=== RMMOD OK ==="
        sleep 1

        if [ "$LOOPS" -le 1 ]; then
                echo "### post-rmmod dmesg (teardown evidence)"
                dmesg | grep mlx5_prism | tail -15
                sync
        fi
        if dmesg | grep -E 'WARNING|Oops|BUG' >/dev/null; then
                echo "!!! kernel complaint detected — stopping"
                dmesg | grep -E 'WARNING|Oops|BUG'
                break
        fi
        sync
done

kill $LIVE_PID 2>/dev/null
chmod 644 "$LOG" "$LIVE"

echo "### restore mlx5_core and capture its command stats for comparison"
modprobe mlx5_core && echo "mlx5_core restored"
sleep 5
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
for op in ENABLE_HCA QUERY_PAGES; do
        d="/sys/kernel/debug/mlx5/0000:08:00.0/commands/$op"
        echo "--- $op:"
        find "$d" -type f 2>/dev/null | while read f; do
                echo "  $(basename "$f") = $(cat "$f")"
        done
done
chmod 644 "$LOG" "$LIVE"
echo "### done — $LOG ; crash recorder: $LIVE"
