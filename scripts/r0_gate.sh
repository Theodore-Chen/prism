#!/bin/bash
# r0_gate.sh — R0 refactor sprint hardware gate (run once with sudo).
#
# R0.1-R0.6 were committed with: build clean + checkpatch + static
# byte-equivalence proof. The runtime legs need root and are batched here
# (DESIGN.md v2 §11.2 safety rules apply: any golden diff = stop+bisect).
#
#   sudo bash scripts/r0_gate.sh
#
# Gates:
#   G1  selftest=1 single cycle  → golden diff, both ports zero-diff
#   G2  selftest=0 single cycle  → production path assertions (new
#       ENABLE_HCA line from bootstrap, no selftest lines, clean teardown)
#   G3  tracepoints              → perf list + paired issue/complete + eq_event
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

say "G1 golden (selftest=1)"
dmesg -c >/dev/null
insmod mlx5_prism.ko selftest=1 && rmmod mlx5_prism; sleep 1
bash scripts/golden/diff_golden.sh "$LIVE"
res $? "golden diff both ports"

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

say "G3 tracepoints"
perf list 2>/dev/null | grep -q "mlx5_prism:prism_cmdq_issue"
res $? "perf list shows prism_cmdq_issue"
dmesg -c >/dev/null
perf record -o /tmp/r0_perf.data -a -e mlx5_prism:prism_cmdq_issue \
	-e mlx5_prism:prism_cmdq_complete -e mlx5_prism:prism_eq_event \
	-- insmod mlx5_prism.ko selftest=1 >/dev/null 2>&1
rmmod mlx5_prism 2>/dev/null
N_I=$(perf script -i /tmp/r0_perf.data 2>/dev/null | grep -c prism_cmdq_issue)
N_C=$(perf script -i /tmp/r0_perf.data 2>/dev/null | grep -c prism_cmdq_complete)
N_E=$(perf script -i /tmp/r0_perf.data 2>/dev/null | grep -c prism_eq_event)
echo "   issue=$N_I complete=$N_C eq_event=$N_E"
[ "$N_I" -gt 10 ] && [ "$N_C" -gt 10 ] && [ "$N_I" -eq "$N_C" ] && [ "$N_E" -gt 0 ]
res $? "issue/complete paired (>10 cmds) and eq_event captured"

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
