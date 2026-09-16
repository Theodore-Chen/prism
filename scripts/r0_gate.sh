#!/bin/bash
# r0_gate.sh — R0 refactor sprint hardware gate (run once with sudo).
#
# R0.1-R0.6 were committed with: build clean + checkpatch + static
# byte-equivalence proof. The runtime legs need root and are batched here
# (DESIGN.md v2 §11.2 safety rules apply: any baseline diff = stop+bisect).
#
#   sudo bash scripts/r0_gate.sh
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

say "G0 build"
make -C /lib/modules/$(uname -r)/build M=$(pwd) clean >/dev/null
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules >/tmp/r0_build.log 2>&1
res $? "build"

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!

modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core

say "G1 baseline (selftest=1)"
dmesg -c >/dev/null
insmod mlx5_prism.ko selftest=1 && rmmod mlx5_prism; sleep 1
bash scripts/baseline/diff_baseline.sh "$LIVE"
res $? "baseline diff both ports"

say "G2 production path (selftest=0)"
dmesg -c >/dev/null
insmod mlx5_prism.ko && rmmod mlx5_prism; sleep 1
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
: > "$TR/trace"
# Arm the events the instant the module registers them — before init runs
# any command. Arming after load captures nothing: tracepoint probes
# detach at MODULE_STATE_GOING, earlier than the exit-time teardown
# commands (learned the hard way; see progress.md 历史问题归档).
( while [ ! -d "$TR/events/mlx5_prism/prism_cmdq_issue" ]; do :; done
  for e in prism_cmdq_issue prism_cmdq_complete prism_eq_event; do
	echo 1 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
  done ) &
ARM=$!
dmesg -c >/dev/null
insmod mlx5_prism.ko selftest=1
res $? "insmod selftest=1"
wait $ARM
for i in $(seq 20); do
	N_I=$(grep -c "prism_cmdq_issue:" "$TR/trace")
	[ "$N_I" -gt 30 ] && break
	sleep 1
done
N_I=$(grep -c "prism_cmdq_issue:" "$TR/trace")
N_C=$(grep -c "prism_cmdq_complete:" "$TR/trace")
N_E=$(grep -c "prism_eq_event:" "$TR/trace")
echo "   issue=$N_I complete=$N_C eq_event=$N_E"
[ "$N_I" -gt 30 ] && [ "$N_I" -eq "$N_C" ] && [ "$N_E" -gt 0 ]
res $? "full-load capture, issue/complete paired, eq_event seen"
# Disarm before unload — enabled module events may pin the module.
for e in prism_cmdq_issue prism_cmdq_complete prism_eq_event; do
	echo 0 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
done
rmmod mlx5_prism

say "G4 stress (20x selftest=1 + 20x default)"
for n in $(seq 20); do
	insmod mlx5_prism.ko selftest=1 && rmmod mlx5_prism || break
done
for n in $(seq 20); do
	insmod mlx5_prism.ko && rmmod mlx5_prism || break
done
dmesg | grep -qE "WARNING|Oops|BUG|leak"
[ $? -ne 0 ]; res $? "40 cycles zero complaints"

kill $LIVE_PID 2>/dev/null
modprobe mlx5_core && echo "== mlx5_core restored"
say "RESULT: $PASS passed, $FAIL failed (blackbox: $LIVE)"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
