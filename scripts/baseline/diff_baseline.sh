#!/bin/bash
# diff_baseline.sh — behavior-preservation gate for the R0 refactor sprint.
#
# Compares the LAST complete load/unload cycle in a dmesg capture against
# the per-port baselines. The two functions are NOT interchangeable:
# caps hexdumps differ by a per-function identifier byte (port 0 vs port 1),
# each stable across cycles — hence two templates. Any difference outside
# the normalize.sh whitelist is a behavior change: STOP, bisect, re-test on
# hardware before proceeding (DESIGN.md v2 §11.2 safety rules).
#
# Usage:
#   sudo bash scripts/gate/t09_verify.sh 1          # fresh single cycle
#   sudo bash scripts/baseline/diff_baseline.sh    # reads /var/tmp/t09_live.log
#   sudo bash scripts/baseline/diff_baseline.sh <dmesg-file>
#
# Exit 0 = zero diff (PASS). Exit 1 = behavior drift (FAIL) — diffs printed.

BASELINE_DIR="$(cd "$(dirname "$0")" && pwd)"
LIVE="${1:-/var/tmp/t09_live.log}"

# Scratch files go to a PRIVATE mktemp dir, never shared /tmp paths: with
# fs.protected_regular=2 (Ubuntu default) even root gets EACCES writing
# over a pre-existing /tmp file owned by another user — fixed filenames
# across users are a trap (2026-09-17 rebaseline incident, progress.md
# 归档⑦). Callers that need the per-port diff files afterwards (e.g.
# rebaseline.sh's expected-drift filter) set PRISM_BASELINE_WORK and
# own the directory's lifetime.
if [ -n "${PRISM_BASELINE_WORK:-}" ]; then
    WORK="$PRISM_BASELINE_WORK"
    mkdir -p "$WORK"
else
    WORK=$(mktemp -d /tmp/prism_baseline.XXXXXX)
    trap 'rm -rf "$WORK"' EXIT
fi

[ -r "$LIVE" ] || { echo "dmesg source missing: $LIVE"; exit 2; }

# Last cycle = everything after the final port-0 "BAR0 mapped" line.
# Both functions probe in a fixed order (.0 then .1), so port-0's line is
# the cycle start marker.
LAST_START=$(grep -n '0000:08:00.0: BAR0 mapped' "$LIVE" | tail -1 | cut -d: -f1)
[ -n "$LAST_START" ] || { echo "no load cycle found in $LIVE"; exit 2; }
tail -n +"$LAST_START" "$LIVE" | bash "$BASELINE_DIR/normalize.sh" > "$WORK/prism_norm_lastcycle.txt"

RC=0
for PORT in 0000:08:00.0 0000:08:00.1; do
    BASELINE="$BASELINE_DIR/baseline_dmesg_${PORT##*.}.txt"
    [ -r "$BASELINE" ] || { echo "[FAIL] $PORT: baseline missing: $BASELINE"; RC=1; continue; }
    grep " $PORT: " "$WORK/prism_norm_lastcycle.txt" > "$WORK/prism_norm_$PORT.txt"
    # Strip the BDF so the port streams are directly comparable to the
    # per-port baseline template.
    sed -i "s/mlx5_prism $PORT: //" "$WORK/prism_norm_$PORT.txt"
    if [ ! -s "$WORK/prism_norm_$PORT.txt" ]; then
        echo "[FAIL] $PORT: no normalized lines extracted"
        RC=1
        continue
    fi
    if diff -u "$BASELINE" "$WORK/prism_norm_$PORT.txt" > "$WORK/prism_diff_$PORT.txt"; then
        echo "[PASS] $PORT: zero diff vs baseline"
    else
        echo "[FAIL] $PORT: behavior drift vs baseline — see $WORK/prism_diff_$PORT.txt"
        cat "$WORK/prism_diff_$PORT.txt"
        RC=1
    fi
done

exit $RC
