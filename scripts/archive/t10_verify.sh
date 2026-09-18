#!/bin/bash
# [已归档 2026-09-18] T10 已完成（三档全绿 + 基线重采 ALL GREEN）。
# 本脚本按当时仓库布局书写，事件模式验证流程已沉淀为基线 + r0_gate，
# 不可直接重跑（判据针对 T10 转正窗口，勿跑后改）。
#
# t10_verify.sh — T10 事件模式分级放量验证（判据预登记，勿跑后改）。
#
# Usage: sudo bash scripts/gate/t10_verify.sh [stage1|stage2|stress|full]
#   stage1（默认）: 双口各 1 次事件模式加载——最安全首档
#   stage2        : 5 次事件模式加载 + eq_mode=poll 回退对照 1 次
#   stress        : 100 次 insmod/rmmod 压测（黑匣子记录器在案）
#   full          : stage1 + stage2 + stress
#
# 预登记判据：
#   S1 每口含 "EQ up (event mode)" 且自检行 "10/10 commands ok in event mode"
#   S2 自检行 events == consumed-in-hardirq（全部 EQE 由硬中断消费）
#   S3 interrupts > 0 且与 events 同数量级（events/4 ≤ irq ≤ events×4）
#   S4 拆除干净：DESTROY_EQ ok + 2305/2305 pages reclaimed + rmmod OK
#   S5 无 "EQ irq storm"、无 WARNING/Oops/BUG
#   S6 (stage2) poll 回退："(armed, poll fallback" + "10/10 ... with an EQE each"
#   S7 (stress) 100/100 循环 INSMOD/RMMOD 全 OK
#
# 注：事件模式转正属有意行为变更——本脚本全绿后单独跑 rebaseline.sh 重采基线。
# 输出: /var/tmp/t10_verify.log ; 黑匣子: /var/tmp/t10_live.log

set -u
cd /home/zqc/code/prism
STAGE=${1:-stage1}
LOG=/var/tmp/t10_verify.log
LIVE=/var/tmp/t10_live.log
PORTS="0000:08:00.0 0000:08:00.1"
exec > >(tee "$LOG") 2>&1

[ -f src/mlx5_prism.ko ] || { echo "!! 先在仓库根 make"; exit 1; }

PASS=0; FAIL=0
say() { echo "== $*"; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; PASS=$((PASS+1)); else echo "   [FAIL] $2"; FAIL=$((FAIL+1)); fi; }

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!
trap 'kill $LIVE_PID 2>/dev/null' EXIT

# --- 单次事件模式加载的完整判据（S1-S5），参数：周期号 ---
check_event_load() {
	local n=$1 p ev irq hard
	for p in $PORTS; do
		dmesg | grep "mlx5_prism $p" | grep -q "EQ up (event mode)"
		res $? "[$n/$p] S1 EQ up (event mode)"
		dmesg | grep "mlx5_prism $p" | grep -q "10/10 commands ok in event mode"
		res $? "[$n/$p] S1 事件模式自检 10/10"
		ev=$(dmesg | grep "mlx5_prism $p" | sed -n 's/.*(\([0-9]*\) events, \([0-9]*\) consumed in hardirq, \([0-9]*\) interrupts).*/\1 \2 \3/p' | tail -1)
		set -- $ev
		if [ $# -eq 3 ]; then
			[ "$1" -gt 0 ] && [ "$1" -eq "$2" ]
			res $? "[$n/$p] S2 events($1) == hardirq($2)"
			hard=$3
			# 同数量级: events/4 <= irq <= events*4 且 > 0
			if [ "$hard" -gt 0 ] && [ $((hard * 4)) -ge "$1" ] && [ "$hard" -le $(($1 * 4)) ]; then
				res 0 "[$n/$p] S3 irq($hard) ~ events($1) 同数量级"
			else
				res 1 "[$n/$p] S3 irq($hard) vs events($1)"
			fi
		else
			res 1 "[$n/$p] S2/S3 自检行解析失败: '$ev'"
		fi
		dmesg | grep "mlx5_prism $p" | grep -q "DESTROY_EQ ok"
		res $? "[$n/$p] S4 DESTROY_EQ ok"
		dmesg | grep "mlx5_prism $p" | grep -q "2305/2305 pages reclaimed"
		res $? "[$n/$p] S4 页回收全量"
	done
	! dmesg | grep mlx5_prism | grep -q "EQ irq storm"
	res $? "[$n] S5 无风暴护栏触发"
	! dmesg | grep -qE 'WARNING|Oops|BUG'
	res $? "[$n] S5 无内核投诉"
}

unload_all() {
	modprobe -r mlx5_ib 2>/dev/null
	modprobe -r mlx5_core 2>/dev/null
	rmmod mlx5_prism 2>/dev/null
}

echo "### T10 $STAGE 验证开始 $(date)"
unload_all
sleep 1

case $STAGE in
stage1|full)
	say "stage1: 双口各 1 次事件模式加载"
	dmesg -c > /dev/null
	insmod src/mlx5_prism.ko selftest=1
	res $? "insmod"
	rmmod mlx5_prism
	res $? "rmmod"
	# 判据在 rmmod 后全量核对：S4 的拆除证据（DESTROY_EQ/页回收）在
	# rmmod 时才进 dmesg（stage1 首跑把检查放在 rmmod 前，白报 4 个
	# FAIL——硬件侧拆除本就干净，黑匣子可证）。
	check_event_load 1
	;;
esac

case $STAGE in
stage2|full)
	say "stage2: 5 次事件模式加载"
	ok=0
	for n in $(seq 1 5); do
		dmesg -c > /dev/null
		if insmod src/mlx5_prism.ko selftest=1 && rmmod mlx5_prism &&
		   dmesg | grep mlx5_prism | grep -q "10/10 commands ok in event mode"; then
			ok=$((ok + 1))
			echo "   cycle $n ok"
		else
			echo "   cycle $n FAIL — 保留 dmesg 于黑匣子"
			dmesg | grep mlx5_prism | tail -20
		fi
		dmesg | grep -qE 'WARNING|Oops|BUG' && { echo "!!! 内核投诉，停止"; break; }
	done
	[ "$ok" -eq 5 ]; res $? "stage2 5/5 事件模式循环"
	ok2=0
	for p in $PORTS; do dmesg | grep "mlx5_prism $p" | grep -q "2305/2305" && ok2=$((ok2+1)); done
	[ "$ok2" -eq 2 ]; res $? "stage2 双口页回收"

	say "stage2: eq_mode=poll 回退对照（T09 语义）"
	dmesg -c > /dev/null
	insmod src/mlx5_prism.ko selftest=1 eq_mode=poll
	res $? "insmod (poll)"
	dmesg | grep mlx5_prism | grep -q "(armed, poll fallback"
	res $? "S6 poll fallback 行"
	dmesg | grep mlx5_prism | grep -q "10/10 commands ok with an EQE each"
	res $? "S6 轮询自检 10/10"
	rmmod mlx5_prism
	res $? "rmmod (poll)"
	;;
esac

case $STAGE in
stress|full)
	say "stress: 100 次 insmod/rmmod"
	ok=0
	for n in $(seq 1 100); do
		dmesg -c > /dev/null
		if insmod src/mlx5_prism.ko selftest=1 2>/dev/null && rmmod mlx5_prism 2>/dev/null; then
			ok=$((ok + 1))
		else
			echo "!!! cycle $n 失败 — 停止，查 $LIVE"
			break
		fi
		if dmesg | grep -qE 'WARNING|Oops|BUG'; then
			echo "!!! cycle $n 内核投诉 — 停止"
			dmesg | grep -E 'WARNING|Oops|BUG' | head
			break
		fi
		[ $((n % 25)) -eq 0 ] && echo "   $n/100 ..."
	done
	[ "$ok" -eq 100 ]; res $? "S7 压测 $ok/100"
	;;
esac

kill $LIVE_PID 2>/dev/null
modprobe mlx5_core 2>/dev/null && echo "### mlx5_core 已恢复"
echo "### $STAGE 完成：PASS=$PASS FAIL=$FAIL — $LOG ; 黑匣子 $LIVE"
[ "$FAIL" -eq 0 ]
