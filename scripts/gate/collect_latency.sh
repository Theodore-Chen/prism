#!/bin/bash
# collect_latency.sh — T11 延迟基线采集：P1'（命令往返）/ P2'（事件提交）
# + mlx5_core 基准 + 判据C 预登记裁决。零内核改动，纯 tracefs 采集
# （本机无 perf/bpftrace，不引入依赖）。
#
# 三阶段：
#   A  N 次（默认 12）insmod selftest=1 纯加载。加载前武装探针（G3 教训：
#      探针在 MODULE_STATE_GOING 就分离，事后补采只会采到空）；每次加载
#      单独快照——P2' 配对按"槽位在被 EQE 确认前不释放"逐加载进行，跨
#      加载混在一个缓冲里会错配。
#   B  mlx5_core 基准：kprobe mlx5_cmd_invoke 入口(p:ml5i)/返回(r:ml5r)，
#      ip link down/up 触发命令。安全闸：只碰无 IP 的 Mellanox 网口
#      （绝不碰承载管理/SSH 流量的口）；kprobe 或安全网口不可用 → 回退
#      文档参考值并在报告标注。
#   C  latency_aggregate.py 聚合：分布（nearest-rank 分位）+ 判据C 裁决。
#
# 输出（全量日志单文件）：/var/tmp/latency_report.txt
#                       /var/tmp/latency_samples.csv（逐样本）
# 临时快照：mktemp 私有目录（PRISM_LAT_WORK 可注入），成功后删除。
#
# Usage: sudo bash scripts/gate/collect_latency.sh [loads]
set -u
cd /home/zqc/code/prism

LOADS=${1:-12}
[ "$LOADS" -ge 1 ] 2>/dev/null || { echo "loads 参数须为正整数"; exit 1; }
[ -f src/mlx5_prism.ko ] || { echo "src/mlx5_prism.ko 不存在——先在仓库根跑 make"; exit 1; }

REPORT=/var/tmp/latency_report.txt
CSV=/var/tmp/latency_samples.csv
exec > >(tee "$REPORT") 2>&1

TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
[ -d "$TR/events" ] || { mount -t tracefs tracefs /sys/kernel/tracing; TR=/sys/kernel/tracing; }

if [ -n "${PRISM_LAT_WORK:-}" ]; then
	WORK=$PRISM_LAT_WORK
else
	WORK=$(mktemp -d /tmp/prism_lat.XXXXXX)
	OWN_WORK=1
fi
echo "work dir: $WORK"

T_ON=$(cat "$TR/tracing_on")
CLK=$(cat "$TR/trace_clock")
ARM=""

EVTS="prism_cmdq_issue prism_cmdq_complete prism_eq_event"
disarm() {
	for e in $EVTS; do
		echo 0 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
	done
}
cleanup() {
	disarm
	[ -n "$ARM" ] && kill "$ARM" 2>/dev/null
	for k in ml5i ml5r; do echo "-:$k" > "$TR/kprobe_events" 2>/dev/null; done
	echo "$CLK" > "$TR/trace_clock" 2>/dev/null
	echo "$T_ON" > "$TR/tracing_on" 2>/dev/null
}
trap cleanup EXIT

abort() {
	echo "!!! 中止：$*"
	echo "!!! 状态自查：lsmod | grep -E 'mlx5_prism|mlx5_core'"
	rmmod mlx5_prism 2>/dev/null
	modprobe mlx5_core 2>/dev/null
	echo "!!! 快照保留在 $WORK 供诊断"
	exit 1
}

# P2' 跨 CPU 比时间戳，local 时钟有跨核偏差——切全局时钟，结束恢复。
echo global > "$TR/trace_clock"
echo 1 > "$TR/tracing_on"

echo "### 阶段A：$LOADS 次 insmod selftest=1 纯加载采集"
modprobe -r mlx5_ib 2>/dev/null
modprobe -r mlx5_core 2>/dev/null
rmmod mlx5_prism 2>/dev/null

for n in $(seq 1 "$LOADS"); do
	echo "--- load $n/$LOADS"
	dmesg -c > /dev/null
	( while [ ! -d "$TR/events/mlx5_prism/prism_cmdq_issue" ]; do :; done
	  for e in $EVTS; do
		echo 1 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
	  done ) &
	ARM=$!
	: > "$TR/trace"
	insmod src/mlx5_prism.ko selftest=1 || { kill "$ARM" 2>/dev/null; ARM=""; abort "insmod 失败于 load $n"; }
	wait "$ARM"
	ARM=""

	# 等尾部耐心窗结束：三计数连续 3 秒不变且 issue>=60，75s 超时。
	prev=""; same=0
	for i in $(seq 75); do
		cur=$( { grep -c 'prism_cmdq_issue:' "$TR/trace"; \
			 grep -c 'prism_cmdq_complete:' "$TR/trace"; \
			 grep -c 'prism_eq_event:' "$TR/trace"; } | tr '\n' '/' )
		if [ "$cur" = "$prev" ]; then same=$((same + 1)); else same=0; fi
		prev=$cur
		[ "$same" -ge 3 ] && [ "${cur%%/*}" -ge 60 ] && break
		sleep 1
	done
	cp "$TR/trace" "$WORK/prism_load_$(printf '%02d' "$n").txt"
	disarm
	rmmod mlx5_prism || abort "rmmod 失败于 load $n"
	if dmesg | grep -E 'WARNING|Oops|BUG' >/dev/null; then
		dmesg | grep -E 'WARNING|Oops|BUG'
		abort "内核投诉于 load $n"
	fi
	sleep 1
done
echo "阶段A完成：$LOADS 份快照"

echo "### 阶段B：mlx5_core 基准（kprobe mlx5_cmd_invoke）"
KP=0
modprobe mlx5_core
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
core_ready=0
for i in $(seq 30); do
	ls /sys/kernel/debug/mlx5/*/commands >/dev/null 2>&1 && { core_ready=1; break; }
	sleep 1
done
[ "$core_ready" = 1 ] || echo "!! mlx5_core 命令目录未出现（30s）——阶段B证据会不足"

cmd_total() {
	local t=0 d n
	for d in /sys/kernel/debug/mlx5/*/commands/*; do
		n=$(cat "$d/n" 2>/dev/null)
		t=$((t + ${n:-0}))
	done
	echo "$t"
}

if echo 'p:ml5i mlx5_cmd_invoke' > "$TR/kprobe_events" 2>"$WORK/kprobe_err" && \
   echo 'r:ml5r mlx5_cmd_invoke' >> "$TR/kprobe_events" 2>>"$WORK/kprobe_err"; then
	KP=1
else
	echo "kprobe 注册失败（$(cat "$WORK/kprobe_err" 2>/dev/null)）→ 报告回退文档参考值"
fi

IFACE=""
if [ "$KP" = 1 ]; then
	for ifc in /sys/class/net/*; do
		ifc=${ifc##*/}
		[ -f "/sys/class/net/$ifc/device/vendor" ] || continue
		[ "$(cat "/sys/class/net/$ifc/device/vendor" 2>/dev/null)" = "0x15b3" ] || continue
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
	cp "$TR/trace" "$WORK/mlx5_core.txt"
	echo "mlx5_core 命令总数：$before → $(cmd_total)（触发生效证据）"
else
	[ "$KP" = 1 ] && echo "无可安全触发链路的 CX 网口（都配置了 IP）→ 回退文档参考值"
fi
for k in ml5i ml5r; do echo "-:$k" > "$TR/kprobe_events" 2>/dev/null; done
# 空捕获不算数：回退参考值口径。
if [ -f "$WORK/mlx5_core.txt" ] && ! grep -q 'ml5i:' "$WORK/mlx5_core.txt"; then
	rm -f "$WORK/mlx5_core.txt"
	echo "kprobe 捕获为空 → 回退文档参考值"
fi

echo "### 阶段C：聚合 + 判据C裁决"
python3 scripts/gate/latency_aggregate.py --work "$WORK" --csv "$CSV"
AGG=$?

[ "${OWN_WORK:-0}" = 1 ] && rm -rf "$WORK"
[ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$REPORT" "$CSV" 2>/dev/null
echo "### 完成（聚合退出码 $AGG）— $REPORT / $CSV"
exit "$AGG"
