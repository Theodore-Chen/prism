#!/bin/bash
# diff_golden.sh — behavior-preservation gate for the R0 refactor sprint.
#
# Compares the LAST complete load/unload cycle in a dmesg capture against
# the per-port golden baselines. The two functions are NOT interchangeable:
# caps hexdumps differ by a per-function identifier byte (port 0 vs port 1),
# each stable across cycles — hence two templates. Any difference outside
# the normalize.sh whitelist is a behavior change: STOP, bisect, re-test on
# hardware before proceeding (DESIGN.md v2 §11.2 safety rules).
#
# Usage:
#   sudo bash scripts/t09_verify.sh 1          # fresh single cycle
#   sudo bash scripts/golden/diff_golden.sh    # reads /var/tmp/t09_live.log
#   sudo bash scripts/golden/diff_golden.sh <dmesg-file>
#
# Exit 0 = zero diff (PASS). Exit 1 = behavior drift (FAIL) — diffs printed.

GOLDEN_DIR="$(cd "$(dirname "$0")" && pwd)"
LIVE="${1:-/var/tmp/t09_live.log}"

[ -r "$LIVE" ] || { echo "dmesg source missing: $LIVE"; exit 2; }

# Last cycle = everything after the final port-0 "BAR0 mapped" line.
# Both functions probe in a fixed order (.0 then .1), so port-0's line is
# the cycle start marker.
LAST_START=$(grep -n '0000:08:00.0: BAR0 mapped' "$LIVE" | tail -1 | cut -d: -f1)
[ -n "$LAST_START" ] || { echo "no load cycle found in $LIVE"; exit 2; }
tail -n +"$LAST_START" "$LIVE" | bash "$GOLDEN_DIR/normalize.sh" > /tmp/prism_norm_lastcycle.txt

RC=0
for PORT in 0000:08:00.0 0000:08:00.1; do
    GOLDEN="$GOLDEN_DIR/golden_dmesg_${PORT##*.}.txt"
    [ -r "$GOLDEN" ] || { echo "[FAIL] $PORT: golden baseline missing: $GOLDEN"; RC=1; continue; }
    grep " $PORT: " /tmp/prism_norm_lastcycle.txt > /tmp/prism_norm_$PORT.txt
    # Strip the BDF so the port streams are directly comparable to the
    # per-port golden template.
    sed -i "s/mlx5_prism $PORT: //" /tmp/prism_norm_$PORT.txt
    if [ ! -s /tmp/prism_norm_$PORT.txt ]; then
        echo "[FAIL] $PORT: no normalized lines extracted"
        RC=1
        continue
    fi
    if diff -u "$GOLDEN" /tmp/prism_norm_$PORT.txt > /tmp/prism_diff_$PORT.txt; then
        echo "[PASS] $PORT: zero diff vs golden"
    else
        echo "[FAIL] $PORT: behavior drift vs golden — see /tmp/prism_diff_$PORT.txt"
        cat /tmp/prism_diff_$PORT.txt
        RC=1
    fi
done

exit $RC
