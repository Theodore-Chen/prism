#!/bin/bash
# r0_trace_check.sh — minimal standalone tracefs verification of the prism
# tracepoints (G3 evidence, no perf dependency).
#
# Diagnoses the full chain: tracing_on master switch -> event registration
# at module load -> event enable -> rmmod teardown commands firing the
# events -> counts read back from the ring buffer.
#
# Usage: sudo bash scripts/r0_trace_check.sh   (~30 s)

set -u
cd /home/zqc/code/prism
TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
[ -d "$TR/events" ] || { mount -t tracefs tracefs /sys/kernel/tracing; TR=/sys/kernel/tracing; }

modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core 2>/dev/null
rmmod mlx5_prism 2>/dev/null

echo "== tracing dir: $TR"
echo "== env: tracing_on=$(cat $TR/tracing_on) buffer_kb=$(cat $TR/buffer_size_kb) cpumask=$(cat $TR/tracing_cpumask) pid_filter=[$(cat $TR/set_event_pid)] tracer=$(cat $TR/current_tracer)"
echo 1 > "$TR/tracing_on"

# Control group: a kernel-builtin event (built by the kernel's own gcc).
# Records but ours does not -> module-side fault; neither records ->
# environmental (buffer/cpumask/pid filter).
echo 1 > "$TR/events/sched/sched_switch/enable"
: > "$TR/trace"
sleep 1
echo "== control (sched_switch in 1s): $(grep -c 'sched_switch:' $TR/trace)"
echo 0 > "$TR/events/sched/sched_switch/enable"

insmod mlx5_prism.ko || { echo "FAIL: insmod"; exit 1; }
EV="$TR/events/mlx5_prism"
if [ ! -d "$EV" ]; then
	echo "FAIL: no mlx5_prism events dir — tracepoints not registered"
	rmmod mlx5_prism
	exit 1
fi
echo "== events registered: $(ls $EV | grep -c prism_)"

echo 1 > "$EV/prism_cmdq_issue/enable"
echo 1 > "$EV/prism_cmdq_complete/enable"
echo 1 > "$EV/prism_eq_event/enable"
echo "== enable bits: issue=$(cat $EV/prism_cmdq_issue/enable) complete=$(cat $EV/prism_cmdq_complete/enable) eq_event=$(cat $EV/prism_eq_event/enable)"

: > "$TR/trace"
rmmod mlx5_prism
sleep 1

NI=$(grep -c "prism_cmdq_issue:" "$TR/trace")
NC=$(grep -c "prism_cmdq_complete:" "$TR/trace")
NE=$(grep -c "prism_eq_event:" "$TR/trace")
echo "== counts: issue=$NI complete=$NC eq_event=$NE"
grep -m3 "prism_cmdq_issue:" "$TR/trace" | sed 's/^[^:]*: //'

modprobe mlx5_core 2>/dev/null
if [ "$NI" -gt 10 ] && [ "$NI" -eq "$NC" ]; then
	echo "== PASS: teardown commands traced, issue/complete paired"
	exit 0
fi
echo "== FAIL: no (or unpaired) events recorded — see diagnostics above"
exit 1
