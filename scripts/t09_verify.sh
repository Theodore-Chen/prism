#!/bin/bash
# T09 hardware verification — full fw bootstrap (ISSI, boot pages,
# HOST_ENDIANNESS, caps probe, SET_HCA_CAP known-good, init pages ~2299,
# INIT_HCA -> OPERATIONAL with timing) + EQ lifecycle: ALLOC_UAR ->
# CREATE_EQ (ladder, QUERY_EQ readback authoritative) -> polling EQE
# consumption (selftest: 10x QUERY_PAGES, one EQE each; event engine
# engages on ~10% of loads -> destroy/recreate retry ladder) ->
# DESTROY_EQ + DEALLOC_UAR; rmmod teardown: TEARDOWN_HCA -> TAKE (2305)
# -> DISABLE_HCA. Interrupt line is enabled but inert: events arrive
# with polling semantics, /proc/interrupts counts stay zero.
#
# Output: /tmp/t09_run.log ; crash recorder: /var/tmp/t09_live.log
# (disk-backed, survives reboot/power loss; every line synced).
#
# Usage: sudo bash scripts/t09_verify.sh [loops]
#   loops=1 (default): single load/unload cycle with full dmesg capture.
#   loops=N          : N insmod/rmmod cycles — the stress leg of the exit
#                      criteria; stops early if dmesg shows WARNING/Oops/BUG.
LOOPS=${1:-1}

LOG=/tmp/t09_run.log
LIVE=/var/tmp/t09_live.log
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
echo "--- full mlx5_core command stats (n per opcode):"
for d in /sys/kernel/debug/mlx5/0000:08:00.0/commands/*; do
        n=$(cat "$d/n" 2>/dev/null)
        echo "  $(basename "$d") = ${n:-?}"
done
chmod 644 "$LOG" "$LIVE"
echo "### done — $LOG ; crash recorder: $LIVE"
