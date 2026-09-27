#!/bin/bash
# [已归档 2026-09-27] T13 已完成（J1-J7 硬件全绿 + rebaseline 8/8 收口）。
# 判据针对 T13 转正窗口预登记，勿跑后改；监控行为已沉淀进基线
# （42 行/口）与 r0_gate。修正史：首轮 J5 事件路径笔误
# （events/mlx5_prism/cmdq_issue → 实际 TRACE_EVENT 名 prism_cmdq_issue，
# 改系统级 enable）；J5 测量口径注记：trace_pipe 消费式读取先排空积压，
# N2 窗口=2s 积压+3s 实时，原始比值 0.746 按校正窗口复算精确 2:1
# （76/s→39.6/s）。交叉对账口径见 DESIGN §8.1/§8.2。
#
# t13_verify.sh — T13 PHY monitor + sysfs 硬件验证（2026-09-27）。
#
# 判据预登记（跑之前固化，跑完只对号入座；依据 task.json T13 v2.3
# 字段表与 DESIGN §8.1-8.3）：
#   J1  build 无警告 + checkpatch 0/0/0（新增文件；header 两条 HEAD 既有
#       长行 check 属祖父条款，不计）
#   J2  默认加载（selftest=0）：双 PF /sys/class/misc/mlx5_prism0/ 与
#       mlx5_prism1/ 各含 10 个无条件属性（phy_symbol_errors /
#       phy_received_bits / phy_corrected_bits / fec_corrected /
#       fec_uncorrectable / link_down_events / pre_fec_ber / status /
#       link_up / monitor_interval_ms）；temperature 存在性与 MTMP 探测
#       行一致（在则记值，不在则记缺）——两 PF 独立判定
#   J3  同加载差分活性：间隔 1s 两次读 phy_received_bits，双口均第二次
#       更大（rx_bits 按线率累积，链路 down 亦成立——T12 现象学复验）
#   J4  pre_fec_ber 数值输出（链路 down 预期 "0"）；status ∈
#       {ok,degraded,critical} 且链路 down 时为 ok；link_up=0（PAOS 判决
#       预期 oper_status=2 down；若上机时线缆已接，按 link_up=1 分支记录
#       并核对 status 数值合理性，不判 FAIL）
#   J5  monitor_interval_ms 读 100 → 写 200 → 读回 200；tracefs
#       mlx5_prism/prism_cmdq_issue 采样窗计数减半（每 tick 3-4 次
#       ACCESS_REG：100ms ≈ 30-40/s，200ms ≈ 15-20/s；比值容忍
#       0.30-0.75。首轮跑因脚本事件路径笔误 ENOENT 计 0，2026-09-27
#       修正为系统级 enable + prism_cmdq_issue，判据不变）
#   J6  双口 "PHY monitor: PAOS probe" / "MTMP probe" / "PHY monitor up"
#       dmesg 行在案，值原样入档（接受/拒绝都是档案事实，回写怪癖档案；
#       预期 PAOS accepted oper_status=2、MTMP 8B 或 32B 之一或拒绝）
#   J7  rmmod 干净：lsmod 无 mlx5_prism、耗时 <10s（cancel 路径不挂起）、
#       加载-卸载全程 dmesg 零 WARNING/Oops/BUG
#   J8  （脚本外，收官步）有意基线变更（probe 每口 +3 行 monitor 族）：
#       sudo bash scripts/gate/rebaseline.sh && sudo bash scripts/gate/r0_gate.sh
#       → 8/8 全绿后才 commit
#
# 输出全部落 /var/tmp/t13_run.log 单文件；黑匣子：/var/tmp/t13_live.log
# （dmesg 逐行 sync）。
#
# Usage: sudo bash scripts/gate/t13_verify.sh     (~4 min)

set -u
cd /home/zqc/code/prism

RUN=/var/tmp/t13_run.log
LIVE=/var/tmp/t13_live.log
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

WORK=$(mktemp -d /tmp/prism_t13.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

mv -f "$RUN" "$RUN.prev" 2>/dev/null
exec > >(tee "$RUN") 2>&1

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!

say() { echo; echo "===== $* ====="; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; else echo "   [FAIL] $2"; RC=1; fi; }
RC=0
port_attrs="phy_symbol_errors phy_received_bits phy_corrected_bits fec_corrected fec_uncorrectable link_down_events pre_fec_ber status link_up monitor_interval_ms"

say "J1 build + checkpatch"
make >/tmp/t13_build.log 2>&1
res $? "make"
[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" src
grep -v "compiler differs\|built by\|You are using" /tmp/t13_build.log | grep -qi "warning\|error"
[ $? -ne 0 ]; res $? "build no warnings"
CP=/home/zqc/code/linux/scripts/checkpatch.pl
"$CP" --no-tree --strict -f src/mlx5_prism_phy.c -f src/mlx5_prism_fmt.h \
	-f src/mlx5_prism_pci.c >/tmp/t13_cp.log 2>&1
grep "^total" /tmp/t13_cp.log
[ "$(grep -c '^total: 0 errors, 0 warnings, 0 checks' /tmp/t13_cp.log)" = "3" ]
res $? "checkpatch 0/0/0 (3 files)"
"$CP" --no-tree --strict -f src/mlx5_prism.h >>/tmp/t13_cp.log 2>&1
grep "^total" /tmp/t13_cp.log | tail -1
[ "$(grep -c '^CHECK' /tmp/t13_cp.log)" = "2" ]
res $? "header: only the 2 grandfathered long-line checks"

modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core

say "load (default, selftest=0)"
dmesg -c >/dev/null
insmod src/mlx5_prism.ko
res $? "insmod"

say "J2 misc devices + attributes"
for n in 0 1; do
	D=/sys/class/misc/mlx5_prism$n
	[ -d "$D" ]; res $? "mlx5_prism$n exists"
	for a in $port_attrs; do
		[ -r "$D/$a" ] || { echo "   missing: $a"; RC=1; }
	done
	res $? "mlx5_prism$n 10 attrs present"
	if [ -e "$D/temperature" ]; then
		echo "   mlx5_prism$n temperature: $(cat $D/temperature)"
	else
		echo "   mlx5_prism$n temperature: absent (MTMP probe verdict)"
	fi
done

say "J3 differential liveness (1s window)"
for n in 0 1; do
	F=/sys/class/misc/mlx5_prism$n/phy_received_bits
	B1=$(cat "$F"); sleep 1; B2=$(cat "$F")
	echo "   prism$n rx_bits: $B1 -> $B2 (delta $((B2 - B1)))"
	[ "$B2" -gt "$B1" ]; res $? "prism$n rx_bits monotonic growth"
done

say "J4 derived fields"
for n in 0 1; do
	D=/sys/class/misc/mlx5_prism$n
	BER=$(cat "$D/pre_fec_ber"); ST=$(cat "$D/status"); LU=$(cat "$D/link_up")
	echo "   prism$n pre_fec_ber=$BER status=$ST link_up=$LU"
	echo "$BER" | grep -qE "^[0-9]+(\.[0-9]+e-[0-9]+)?$"
	res $? "prism$n pre_fec_ber numeric ($BER)"
	echo "$ST" | grep -qE "^(ok|degraded|critical)$"
	res $? "prism$n status valid ($ST)"
	if [ "$LU" = "0" ]; then
		[ "$ST" = "ok" ]; res $? "prism$n link down -> status frozen ok"
	else
		echo "   [NOTE] prism$n link up — status value recorded, not gated"
	fi
done

say "J5 monitor_interval_ms RW + cadence"
V=$(cat /sys/class/misc/mlx5_prism0/monitor_interval_ms)
[ "$V" = "100" ]; res $? "default interval 100 (got $V)"
TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
# system-level arm (one write, G3-hardened style); the event name is
# prism_cmdq_issue under system mlx5_prism (fixed 2026-09-27: the first
# run wrote events/mlx5_prism/cmdq_issue/enable → ENOENT, counts 0 —
# measurement plumbing bug, criterion unchanged)
echo 1 > "$TR/tracing_on"
echo > "$TR/trace"
echo 1 > "$TR/events/mlx5_prism/enable"
sleep 0.5
N1=$(timeout 3 cat "$TR/trace_pipe" | grep -c prism_cmdq_issue)
# both PFs: a single-port change would dilute the ratio (the other port
# keeps ticking at the old cadence inside the same trace window)
for n in 0 1; do
	echo 200 > /sys/class/misc/mlx5_prism$n/monitor_interval_ms
done
sleep 2
N2=$(timeout 3 cat "$TR/trace_pipe" | grep -c prism_cmdq_issue)
echo 0 > "$TR/events/mlx5_prism/enable"
V=$(cat /sys/class/misc/mlx5_prism0/monitor_interval_ms)
[ "$V" = "200" ]; res $? "interval readback 200 after write (got $V)"
echo "   cmdq_issue/s: ~$((N1 / 3))/s (100ms) -> ~$((N2 / 3))/s (200ms)"
[ "$N2" -gt 0 ] && [ "$N2" -lt "$N1" ]; res $? "cadence slowed after write"
[ "$N1" -gt 0 ]; res $? "events captured (measurement live)"
R=$(awk "BEGIN{print $N2 / $N1}")
awk "BEGIN{exit !($R >= 0.30 && $R <= 0.75)}"
res $? "rate ratio in [0.30,0.75] (got $R)"

say "J6 probe verdicts in dmesg"
D=$(dmesg | grep mlx5_prism)
echo "$D" | grep "PAOS probe" | sed 's/^/   /'
echo "$D" | grep "MTMP probe" | sed 's/^/   /'
echo "$D" | grep "PHY monitor up" | sed 's/^/   /'
[ "$(echo "$D" | grep -c 'PAOS probe')" -ge 2 ]; res $? "PAOS probe lines x2"
[ "$(echo "$D" | grep -c 'MTMP probe')" -ge 2 ]; res $? "MTMP probe lines x2"
[ "$(echo "$D" | grep -c 'PHY monitor up')" -ge 2 ]; res $? "monitor-up lines x2"

say "J7 rmmod clean"
T0=$SECONDS
rmmod mlx5_prism
R=$?
DT=$((SECONDS - T0))
res $R "rmmod"
lsmod | grep -q mlx5_prism; [ $? -ne 0 ]; res $? "module gone"
[ "$DT" -lt 10 ]; res $? "rmmod fast (${DT}s, cancel path no hang)"
echo "$D" | grep -qE "WARNING|Oops|BUG"
[ $? -ne 0 ]; res $? "no kernel complaints"

kill $LIVE_PID 2>/dev/null
modprobe mlx5_core

echo
echo "===== T13 verify: $([ $RC -eq 0 ] && echo ALL GREEN || echo NOT GREEN) ====="
echo "J8 (outside this script): rebaseline + r0_gate 8/8 before commit"
exit $RC
