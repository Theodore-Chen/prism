#!/bin/bash
# t10prep_eqc_probe.sh — eqc 编码翻案实验（T10 前置；判别式预登记，跑前勿改）
#
# Hypotheses under test (progress.md R0.2 + T10-prep 预登记节):
#   H1  固件按 mlx5_ifc/PRM 语义解读 eqc（MSB-first per dword）。旧编码
#       ((log&7)<<29)|uar 把 log 写进了 reserved 位，log_eq_size 字段实际
#       读到 eqc[12]&0x1f = uar&0x1f = 0 —— "1 条目 EQ"是我们字面请求的。
#   H0  固件钳制/忽略 log_eq_size，与编码无关（T09 原结论维持）。
#
# Pre-registered verdict criteria (fixed BEFORE the run):
#   A 尺寸翻案  eq_ifc=1 时 "QUERY_EQ readback (ifc windows)" 的
#               log_eq_size ≥ 6（真多条目环；旧路径恒 0）
#   B 激活率    eq_ifc=1 且 eq_engagement_retries=0：5 次加载中每次
#               自检 10/10（旧行为 ~10%/加载，几乎必挂）
#   C 长尾      eq_ifc=1 时无 "event engine silent"/耐心窗 miss 计数
#               显著少于 ctrl0 对照（黑匣子逐行统计）
#   D 中断      eq_ifc=1 时 "10/10 commands ok" 行的 interrupts > 0，
#               且 /proc/interrupts 的 prism 向量 before/after 差 > 0
#               （旧路径恒全零）
#   E 对照      ctrl（默认参数）行为与金样一致：QUERY_EQ 仍报
#               log_eq_size=0，10/10 自检通过（排除环境漂移混杂）
#
# 结论空间：
#   A  → 翻案成立：DESIGN §7 + fw_profile 重写，ifc 编码转正
#   ¬A → H1 排除：T10 严格官方路径，怪癖档案维持原样
#   A ∧ (¬B ∨ ¬C) → 环尺寸与激活/长尾解耦：两个独立怪癖并存
#   D 独立评估 intr 假设（dw5 读回 + 中断计数分离证据）
#
# 预期现象备注：ctrl0/ifc 运行里 insmod 失败是合法结果（引擎静默 +
# retries=0 → 自检 -EIO），黑匣子照捕，不要中断脚本。
#
# Usage: sudo bash scripts/t10prep_eqc_probe.sh        (~2-4 min)

set -u
cd /home/zqc/code/prism
LIVE=/var/tmp/t10prep_live.log
say() { echo "== $*"; }

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!
trap 'kill $LIVE_PID 2>/dev/null' EXIT

modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core 2>/dev/null
rmmod mlx5_prism 2>/dev/null

irq_snap() { grep -i prism /proc/interrupts 2>/dev/null || true; }
irq_sum() { awk '{s=0; for(i=2;i<=NF;i++) if($i+0>0) s+=$i; print s+0}' "$1" 2>/dev/null; }

run() { # run <tag> [module params...]
	local tag=$1; shift
	say "run $tag: insmod selftest=1 $*"
	irq_snap > /var/tmp/t10prep_irq_${tag}.before
	dmesg -c >/dev/null
	if insmod mlx5_prism.ko selftest=1 "$@"; then
		echo "   insmod ok"
	else
		echo "   insmod FAILED (legal outcome: silent engine + retries=0)"
	fi
	sleep 1
	rmmod mlx5_prism 2>/dev/null
	sleep 1
	irq_snap > /var/tmp/t10prep_irq_${tag}.after
	dmesg > /var/tmp/t10prep_dmesg_${tag}.txt
}

say "E: legacy control (defaults — should match golden behavior)"
run ctrl

say "B baseline: legacy encoding, raw engagement (retries=0, ~10% expected)"
run ctrl0 eq_engagement_retries=0

say "A/B/C/D: ifc encoding x5, raw engagement"
for i in 1 2 3 4 5; do run ifc$i eq_ifc=1 eq_engagement_retries=0; done

kill $LIVE_PID 2>/dev/null; trap - EXIT

say "verdict inputs (final adjudication from $LIVE + these files)"
echo "-- A/E: QUERY_EQ lines per run"
grep -h "QUERY_EQ" /var/tmp/t10prep_dmesg_*.txt | sed 's/^mlx5_prism: *//' | sort | uniq -c
echo "-- B: selftest outcome per run"
grep -h "eq self-test: 10/10\|no EQE for command" /var/tmp/t10prep_dmesg_*.txt | sed 's/^mlx5_prism: *//' | sort | uniq -c
echo "-- C: silence evidence per run"
for f in /var/tmp/t10prep_dmesg_*.txt; do
	echo "   $(basename $f): $(grep -c 'silent\|miss' "$f") silence-ish lines"
done
echo "-- D: irq deltas (per-run before/after sums)"
for t in ctrl ctrl0 ifc1 ifc2 ifc3 ifc4 ifc5; do
	b=$(irq_sum /var/tmp/t10prep_irq_${t}.before)
	a=$(irq_sum /var/tmp/t10prep_irq_${t}.after)
	echo "   $t: before=${b:-0} after=${a:-0}"
done
say "done — 判别式见脚本头；ctrl0 失败/ifc 失败均为合法数据点"
