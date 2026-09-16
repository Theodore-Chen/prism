#!/bin/bash
# r0_trace_check.sh — standalone tracefs verification of the prism
# tracepoints (G3 evidence, no perf dependency).
#
# v3 — capture during LOAD. The event directories register before module
# init runs any command, so a poll loop arms them the instant they appear
# and the whole bootstrap/self-test gets captured. (Arming after load and
# capturing at rmmod does NOT work: the kernel detaches tracepoint probes
# at MODULE_STATE_GOING — before the module exit function issues its
# teardown commands — so late commands find funcs == NULL and record
# nothing. That asymmetry cost us an afternoon.)
#
# Usage: sudo bash scripts/r0_trace_check.sh   (~30 s)

set -u
cd /home/zqc/code/prism
TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
[ -d "$TR/events" ] || { mount -t tracefs tracefs /sys/kernel/tracing; TR=/sys/kernel/tracing; }

modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core 2>/dev/null
rmmod mlx5_prism 2>/dev/null

echo "== env: tracing_on=$(cat $TR/tracing_on) buffer_kb=$(cat $TR/buffer_size_kb) tracer=$(cat $TR/current_tracer)"
echo 1 > "$TR/tracing_on"
: > "$TR/trace"

# Arm the events the instant the module registers them (before init runs
# any command). The spin is brief — registration precedes probe by far.
( while [ ! -d "$TR/events/mlx5_prism/prism_cmdq_issue" ]; do :; done
  for e in prism_cmdq_issue prism_cmdq_complete prism_eq_event; do
	echo 1 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
  done ) &
ARM=$!

insmod mlx5_prism.ko selftest=1 || { echo "FAIL: insmod"; kill $ARM 2>/dev/null; exit 1; }
wait $ARM
# Let the eq self-test tail (patience windows) land in the buffer.
for i in $(seq 20); do
	NI=$(grep -c "prism_cmdq_issue:" "$TR/trace")
	[ "$NI" -gt 30 ] && break
	sleep 1
done

NI=$(grep -c "prism_cmdq_issue:" "$TR/trace")
NC=$(grep -c "prism_cmdq_complete:" "$TR/trace")
NE=$(grep -c "prism_eq_event:" "$TR/trace")
echo "== counts: issue=$NI complete=$NC eq_event=$NE"
grep -m3 "prism_cmdq_issue:" "$TR/trace" | sed 's/^[^:]*: //'

# Disarm before unload — enabled module events may pin the module.
for e in prism_cmdq_issue prism_cmdq_complete prism_eq_event; do
	echo 0 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
done
rmmod mlx5_prism
modprobe mlx5_core 2>/dev/null

if [ "$NI" -gt 30 ] && [ "$NI" -eq "$NC" ] && [ "$NE" -gt 0 ]; then
	echo "== PASS: full-load capture, issue/complete paired, eq_event seen"
	exit 0
fi
echo "== FAIL: no (or unpaired) events recorded — see diagnostics above"
exit 1
