#!/bin/bash
# collect_mlx5_baseline.sh — T16：mlx5_core 侧基准一站式采集（C1-C4 一致性
# 对比的 core 侧供数 + core CMDQ 延迟基准）。
#
# 口径登记（跑之前固化）：
#   - task.json T16 steps[2] 原文写 "bpftrace 采集"——本机无 perf/bpftrace
#     （T11 已裁决，progress T11 节），按既定路线走 tracefs 直采：
#     kprobe mlx5_cmd_invoke 入口/返回 = 函数驻留时间口径（与 prism P1'
#     相近但不严格等价，报告分开呈现，DESIGN §10.1）。偏差登记于
#     progress.md T16 节与 CLAUDE.md。
#   - 安全闸（collect_latency.sh 阶段 B 原文照抄）：触发链路只碰无 IP 的
#     Mellanox 网口——绝不碰承载管理/SSH 流量的口；kprobe 或安全网口不可
#     用 → 采集侧如实记缺，报告回退 T11 参考值（n=20764, p50≈5us）并标注。
#   - 快照全部落 /var/tmp/mlx5_baseline/ 永久保留（T11 教训：成功后删快照
#     会消灭证据）；summary.txt 为 compare_report.sh 的机器可读摘要。
#
# 采集内容：
#   fw.txt          ethtool -i（C2：固件版本，预期 14.32.1908）
#   core_dmesg.txt  core 引导 dmesg（C3：引导序列对照）
#   debugfs.txt     /sys/kernel/debug/mlx5/ 快照（C1：caps 侧能采到什么采
#                   什么——命令统计必在；caps 字段有无如实入档）
#   ethtool_S.txt   ethtool -S 双口（C4：同名计数器对账）
#   mlx5_core.txt   kprobe 延迟 trace（P1' core 基准）
#   summary.txt     摘要（版本/关键计数器/kprobe 样本数）
#
# Usage: sudo bash scripts/gate/collect_mlx5_baseline.sh
#
# 修正史：
#   trip-3（2026-10-01，首跑两处仪表修正，判据不变）：①kprobe 腿捕获
#   为空记缺——独立运行时 tracing_on 未开（collect_latency 同手法能成
#   是沾了阶段 A 开关的光）；②summary.txt 的 fw 字段误抓 ethtool -i 的
#   `version:`（driver 版本）而非 `firmware-version:`（固件版本）——
#   C2 对照曾显示错值。kprobe 数据与 collect_latency 阶段 B 冗余（报告
#   core 基准取 latency_report 的 n=20700），首跑缺此腿无数据损失。

set -u
cd /home/zqc/code/prism
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

OUT=/var/tmp/mlx5_baseline
rm -rf "$OUT"; mkdir -p "$OUT"

TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
[ -d "$TR/events" ] || { mount -t tracefs tracefs /sys/kernel/tracing; TR=/sys/kernel/tracing; }

echo "=== mlx5_core 侧基准采集 $(date '+%F %T') ==="

# ---- 会话准备：prism 让位，core 上位 ----
rmmod mlx5_prism 2>/dev/null
modprobe -r mlx5_ib 2>/dev/null
modprobe -r mlx5_core 2>/dev/null
dmesg -c >/dev/null
modprobe mlx5_core || { echo "!! modprobe mlx5_core 失败"; exit 1; }
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
core_ready=0
for i in $(seq 30); do
	ls /sys/kernel/debug/mlx5/*/commands >/dev/null 2>&1 && { core_ready=1; break; }
	sleep 1
done
[ "$core_ready" = 1 ] || echo "!! mlx5_core 命令目录 30s 未出现——debugfs 证据会不足"

# ---- core 网口发现（vendor 15b3） ----
CORE_IFS=""
for nd in /sys/class/net/*; do
	nd=${nd##*/}
	[ -f "/sys/class/net/$nd/device/vendor" ] || continue
	[ "$(cat "/sys/class/net/$nd/device/vendor" 2>/dev/null)" = "0x15b3" ] && CORE_IFS="$CORE_IFS $nd"
done
CORE_IFS=${CORE_IFS# }
echo "core netdevs:$CORE_IFS" | tee "$OUT/netdevs.txt"
IF0=${CORE_IFS%% *}

# ---- C2：固件版本 ----
if [ -n "${IF0:-}" ]; then
	ethtool -i "$IF0" > "$OUT/fw.txt" 2>&1 && echo "fw: $(grep '^version:' "$OUT/fw.txt")"
else
	echo "fw: （无 core 网口，记缺）" > "$OUT/fw.txt"
fi

# ---- C3：引导 dmesg ----
sleep 5	# 双口引导收敛
dmesg > "$OUT/core_dmesg.txt"
echo "core_dmesg: $(wc -l < "$OUT/core_dmesg.txt") 行"

# ---- C1：debugfs 快照（能采到什么采什么，如实入档） ----
{
	echo "# /sys/kernel/debug/mlx5 快照 $(date '+%F %T')"
	for d in /sys/kernel/debug/mlx5/*; do
		echo "== $d"
		find "$d" -maxdepth 2 -type f 2>/dev/null | while read -r f; do
			if [ "$(stat -c %s "$f" 2>/dev/null || echo 999999)" -le 4096 ]; then
				echo "-- $f"; cat "$f" 2>&1
			else
				echo "-- $f（$(stat -c %s "$f") 字节，过大跳过）"
			fi
		done
	done
} > "$OUT/debugfs.txt"
echo "debugfs: $(wc -l < "$OUT/debugfs.txt") 行（caps 可得性见内文）"

# ---- C4：ethtool -S 双口 ----
for ifc in $CORE_IFS; do
	echo "== $ifc" >> "$OUT/ethtool_S.txt"
	ethtool -S "$ifc" 2>&1 >> "$OUT/ethtool_S.txt"
done
echo "ethtool_S: $(wc -l < "$OUT/ethtool_S.txt") 行"

# ---- P1' core 基准：kprobe（阶段 B 手法照抄） ----
KP=0
T_ON=$(cat "$TR/tracing_on")
echo 1 > "$TR/tracing_on"	# 独立运行无阶段 A 铺路——必须自开（trip-3 修正：
				# 首跑捕获为空记缺的根因）
if echo 'p:ml5i mlx5_cmd_invoke' > "$TR/kprobe_events" 2>"$OUT/kprobe_err" && \
   echo 'r:ml5r mlx5_cmd_invoke' >> "$TR/kprobe_events" 2>>"$OUT/kprobe_err"; then
	KP=1
else
	echo "kprobe 注册失败（$(cat "$OUT/kprobe_err" 2>/dev/null)）→ 记缺"
fi

cmd_total() {
	local t=0 d n
	for d in /sys/kernel/debug/mlx5/*/commands/*; do
		n=$(cat "$d/n" 2>/dev/null)
		t=$(( t + ${n:-0} ))
	done
	echo "$t"
}

IFACE=""
if [ "$KP" = 1 ]; then
	for ifc in $CORE_IFS; do
		# 安全闸：只碰无 IP 的网口——绝不碰承载管理/SSH 流量的口。
		ip -4 addr show dev "$ifc" 2>/dev/null | grep -q 'inet ' && continue
		ip -6 addr show dev "$ifc" scope global 2>/dev/null | grep -q 'inet6' && continue
		IFACE=$ifc
		break
	done
fi

if [ "$KP" = 1 ] && [ -n "$IFACE" ]; then
	echo "触发源：$IFACE ip link down/up ×10（无 IP 的 CX 网口）"
	before=$(cmd_total)
	echo 1 > "$TR/events/kprobes/ml5i/enable"
	echo 1 > "$TR/events/kprobes/ml5r/enable"
	: > "$TR/trace"
	for i in $(seq 10); do
		ip link set dev "$IFACE" down
		sleep 0.3
		ip link set dev "$IFACE" up
		sleep 0.7
	done
	sleep 1
	echo 0 > "$TR/events/kprobes/ml5i/enable"
	echo 0 > "$TR/events/kprobes/ml5r/enable"
	cp "$TR/trace" "$OUT/mlx5_core.txt"
	KN=$(grep -c 'ml5i:' "$OUT/mlx5_core.txt")
	echo "mlx5_core 命令总数：$before → $(cmd_total)（触发生效证据），kprobe 样本 $KN"
else
	[ "$KP" = 1 ] && echo "无可安全触发链路的 CX 网口（都配了 IP）→ 记缺"
fi
for k in ml5i ml5r; do echo "-:$k" > "$TR/kprobe_events" 2>/dev/null; done
echo "$T_ON" > "$TR/tracing_on" 2>/dev/null	# 恢复采样前状态
# 空捕获不算数
if [ -f "$OUT/mlx5_core.txt" ] && ! grep -q 'ml5i:' "$OUT/mlx5_core.txt"; then
	rm -f "$OUT/mlx5_core.txt"
	echo "kprobe 捕获为空 → 记缺（报告回退 T11 参考值并标注）"
fi

# ---- 摘要（compare_report.sh 的输入） ----
{
	echo "date $(date '+%F %T')"
	echo "fw $(grep '^firmware-version:' "$OUT/fw.txt" 2>/dev/null | awk '{print $2}')"
	echo "netdevs $CORE_IFS"
	for c in tx_packets_phy rx_packets_phy rx_symbol_err_phy link_down_events_phy; do
		v=$(grep -m1 " $c:" "$OUT/ethtool_S.txt" 2>/dev/null | awk '{print $2}')
		echo "ethtool_S $c ${v:-missing}"
	done
	if [ -f "$OUT/mlx5_core.txt" ]; then
		echo "kprobe_samples $(grep -c 'ml5i:' "$OUT/mlx5_core.txt")"
	else
		echo "kprobe_samples missing"
	fi
	echo "dmesg_lines $(wc -l < "$OUT/core_dmesg.txt")"
} > "$OUT/summary.txt"

[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT"
echo "=== 完成 — $OUT（快照永久保留）"
cat "$OUT/summary.txt"
