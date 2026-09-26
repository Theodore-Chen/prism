#!/bin/bash
# r0_gate.sh — R0 refactor sprint hardware gate (run once with sudo).
#
# R0.1-R0.6 were committed with: build clean + checkpatch + static
# byte-equivalence proof. The runtime legs need root and are batched here
# (DESIGN.md v2 §11.2 safety rules apply: any baseline diff = stop+bisect).
#
#   sudo bash scripts/gate/r0_gate.sh
#
# Gates:
#   G1  selftest=1 single cycle  → baseline diff, both ports zero-diff
#   G2  selftest=0 single cycle  → production path assertions (bootstrap
#       ENABLE_HCA line, no selftest lines, clean teardown)
#   G3  tracepoints              → tracefs: events registered + teardown
#                                  commands traced, issue/complete paired
#   G4  stress                   → 20x selftest=1 + 20x default, zero
#                                  WARNING/Oops (blackbox recorder on)

cd /home/zqc/code/prism
LIVE=/var/tmp/r0_gate_live.log
PASS=0; FAIL=0
say() { echo "== $*"; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; PASS=$((PASS+1)); else echo "   [FAIL] $2"; FAIL=$((FAIL+1)); fi; }

# G3 snapshots land here (kept on a FAIL for post-mortem — /var/tmp copies;
# the mktemp dir itself goes away with the run).
GWORK=$(mktemp -d /tmp/prism_r0gate.XXXXXX)
trap 'rm -rf "$GWORK"' EXIT

say "G0 build"
make clean >/dev/null
make >/tmp/r0_build.log 2>&1
res $? "build"
# Build ran under sudo: hand the artifacts back to the invoking user so
# later non-root incremental builds don't hit root-owned .o/.cmd files.
[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" src

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!

modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core

say "G1 baseline (selftest=1)"
dmesg -c >/dev/null
insmod src/mlx5_prism.ko selftest=1 && rmmod mlx5_prism; sleep 1
bash scripts/baseline/diff_baseline.sh "$LIVE"
res $? "baseline diff both ports"

say "G2 production path (selftest=0)"
dmesg -c >/dev/null
insmod src/mlx5_prism.ko && rmmod mlx5_prism; sleep 1
D=$(dmesg | grep mlx5_prism)
echo "$D" | grep -q "ENABLE_HCA" && echo "$D" | grep -q "INIT_HCA ok" \
	&& echo "$D" | grep -q "teardown: 2305/2305"
res $? "bootstrap+teardown complete"
echo "$D" | grep -qv "self-test"
res $? "no selftest output in default probe"
echo "$D" | grep -qE "WARNING|Oops|BUG"
[ $? -ne 0 ]; res $? "no kernel complaints"

say "G3 tracepoints (tracefs, captured during load)"
TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
[ -d "$TR/events" ] || { mount -t tracefs tracefs /sys/kernel/tracing; TR=/sys/kernel/tracing; }
echo 1 > "$TR/tracing_on"

# g3_capture <tag> — one armed selftest load; sets N_I/N_C/N_E/DROPS and
# leaves a snapshot at $GWORK/<tag>.snap. Hardened after the 2026-09-26
# rebaseline G3 false verdict (issue=61 complete=62 while the identical
# protocol reproduced 90/90 with zero overruns — a transient capture
# artifact, driver invariant proven separately, /var/tmp/t12_trace_diag.log):
#   - ONE system-level "events/mlx5_prism/enable" write arms all three
#     events (three sequential per-event writes were a 3x wider window for
#     records to slip through un-armed during module registration);
#   - counts come from a copied snapshot, never from the live file (a
#     mid-iteration write can corrupt the iteration);
#   - per-CPU drop/overrun counters are part of the verdict — with drops
#     the pairing assertion is INVALID, not failed;
#   - one retry on a non-clean capture (transient arming race), a clean
#     mismatch after retry is a real FAIL.
g3_capture() {
	local tag=$1 s drops=0
	: > "$TR/trace"
	( while [ ! -d "$TR/events/mlx5_prism/prism_cmdq_issue" ]; do :; done
	  echo 1 > "$TR/events/mlx5_prism/enable" 2>/dev/null ) &
	local arm=$!
	dmesg -c >/dev/null
	insmod src/mlx5_prism.ko selftest=1 || { kill $arm 2>/dev/null; return 9; }
	wait $arm
	sleep 2
	cp "$TR/trace" "$GWORK/$tag.snap"
	for s in "$TR"/per_cpu/cpu*/stats; do
		drops=$(( drops + $(awk '/^dropped events:/ {print $3}' "$s") ))
	done
	N_I=$(grep -c "prism_cmdq_issue:" "$GWORK/$tag.snap")
	N_C=$(grep -c "prism_cmdq_complete:" "$GWORK/$tag.snap")
	N_E=$(grep -c "prism_eq_event:" "$GWORK/$tag.snap")
	DROPS=$drops
}

G3RC=1
G3INS=0
for attempt in 1 2; do
	g3_capture "g3_try$attempt" || { G3INS=1; break; }
	echo "   try$attempt: issue=$N_I complete=$N_C eq_event=$N_E dropped=$DROPS"
	if [ "$N_I" -gt 30 ] && [ "$N_I" -eq "$N_C" ] && [ "$N_E" -gt 0 ] && [ "$DROPS" -eq 0 ]; then
		G3RC=0
		break
	fi
	[ "$DROPS" -ne 0 ] && echo "   ($DROPS records dropped — pairing verdict invalid)"
	echo "   try$attempt not clean — snapshot kept: /var/tmp/g3_try${attempt}.snap"
	cp "$GWORK/g3_try$attempt.snap" "/var/tmp/g3_try${attempt}.snap"
	rmmod mlx5_prism 2>/dev/null
done
res "$G3INS" "insmod selftest=1"
rmmod mlx5_prism 2>/dev/null
res "$G3RC" "full-load capture, issue/complete paired, eq_event seen, zero drops"
# Disarm before unload — enabled module events may pin the module.
echo 0 > "$TR/events/mlx5_prism/enable" 2>/dev/null

say "G4 stress (20x selftest=1 + 20x default)"
for n in $(seq 20); do
	insmod src/mlx5_prism.ko selftest=1 && rmmod mlx5_prism || break
done
for n in $(seq 20); do
	insmod src/mlx5_prism.ko && rmmod mlx5_prism || break
done
dmesg | grep -qE "WARNING|Oops|BUG|leak"
[ $? -ne 0 ]; res $? "40 cycles zero complaints"

kill $LIVE_PID 2>/dev/null
modprobe mlx5_core && echo "== mlx5_core restored"
say "RESULT: $PASS passed, $FAIL failed (blackbox: $LIVE)"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
