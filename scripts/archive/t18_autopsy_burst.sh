#!/bin/bash
# ======================= 已归档（T18 收官，2026-11-05） =======================
# 一次性尸检脚本，结论已入 docs/report.md 性能附录与 progress.md；内部路径
# 按当时仓库布局书写，不随重组更新，不可直接重跑。
# =============================================================================
# t18_autopsy_burst.sh — T18 停摆尸检定案实验（一次性，跑完归档）。
#
# 背景（2026-10-10，collect_datapath 两轮复现）：prism0 全速突发下 TX 在
# tx_packets=66 / cc=67 / sofar=225 / pc=228 处确定性冻结；poll 每 10ms 跑
# 但 CQ 无新 CQE；低速率单会话 4574 包（71 次 CQ 回卷）无恙——突发病理。
# 首选机理：64 项 CQ 深度被 µs 级完成延迟击穿（P−C=64 = CQ overrun）→
# 固件停摆。EQ 硬中断消费者对未知事件类型只 trace 不打 dmesg（eq.c
# else 分支）——CQ_ERROR 类 EQE 若存在，只在 tracefs 里。
#
# 判据预登记（对号入座）：
#   E1 全速突发 100k：预期冻结复现（sofar 冻结）；eq trace 捕获冻结时刻
#      事件类型分布——出现非 comp/cmd 类型（如 0x11/0x12）即固件错误状态
#      投递在案，与 overrun 机理对表。
#   E2 限速 ratep 5000 ×5000 包：预期正常完成——膝盖位于 5k 与全速之间
#      （佐证「完成延迟 × 速率 > CQ 深度」模型）。
#   两腿都采：pktgen Result、tx 计数前后差、EQE 时间线（含 comp 事件数
#      与最后事件时刻）。
#
# Usage: sudo bash scripts/gate/t18_autopsy_burst.sh
set -u
cd /home/zqc/code/prism

OUT=/var/tmp/t18_autopsy
LIVE=/var/tmp/t18_autopsy_live.log
PIN_CPU=$(( $(nproc) - 1 ))
IP1=169.254.77.1; IP2=169.254.77.2
say() { echo; echo "======== $*"; }

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -f src/mlx5_prism.ko ] || { echo "先跑 make"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
exec > >(tee "$OUT/report.txt") 2>&1
mv -f "$LIVE" "$LIVE.prev" 2>/dev/null; rm -f "$LIVE"
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!
TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
CLK=$(sed 's/.*\[\([^]]*\)\].*/\1/' "$TR/trace_clock")
KT=kpktgend_$PIN_CPU
CLEANED=0
cleanup() {
	[ "$CLEANED" = 1 ] && return 0
	CLEANED=1
	for e in prism_eq_event prism_sq_xmit prism_rx_cqe; do
		[ -e "$TR/events/mlx5_prism/$e/enable" ] && echo 0 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
	done
	echo "$CLK" > "$TR/trace_clock" 2>/dev/null
	[ -e /proc/net/pktgen/$KT ] && echo rem_device_all > "/proc/net/pktgen/$KT" 2>/dev/null
	[ -e /proc/net/pktgen/pgctrl ] && timeout 5 bash -c "echo stop > /proc/net/pktgen/pgctrl" 2>/dev/null
	modprobe -r pktgen 2>/dev/null
	ip link set prism0 down 2>/dev/null; ip link set prism1 down 2>/dev/null
	ip addr del $IP1/24 dev prism0 2>/dev/null; ip addr del $IP2/24 dev prism1 2>/dev/null
	rmmod mlx5_prism 2>/dev/null
	modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core 2>/dev/null
	modprobe mlx5_core 2>/dev/null
	kill "$LIVE_PID" 2>/dev/null
	[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT" "$LIVE" 2>/dev/null
}
trap cleanup EXIT
trap 'echo; echo "!!! SIGINT——清理后退出"; cleanup; exit 130' INT
abort() { echo "!!! 中止：$*"; exit 1; }

modprobe pktgen || abort "modprobe pktgen"
for f in pgctrl pg_control; do [ -e "/proc/net/pktgen/$f" ] && PGCTRL="/proc/net/pktgen/$f" && break; done
[ -n "${PGCTRL:-}" ] || abort "pktgen 控制文件未发现"
[ -e "/proc/net/pktgen/$KT" ] || abort "$KT 不存在"

# ---- prism 会话 ----
rmmod mlx5_prism 2>/dev/null
modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core 2>/dev/null
dmesg -c >/dev/null
insmod src/mlx5_prism.ko || abort "insmod 失败"
for i in $(seq 150); do [ -d /sys/class/net/prism0 ] && break; sleep 0.1; done
[ -d /sys/class/net/prism1 ] || abort "netdev 未出现"
TXDEV=prism0; RXDEV=prism1
PEER_MAC=$(cat /sys/class/net/$RXDEV/address)
ip link set $TXDEV up; ip link set $RXDEV up
ip addr add $IP1/24 dev $TXDEV 2>/dev/null; ip addr add $IP2/24 dev $RXDEV 2>/dev/null
sysctl -w net.ipv4.conf.$TXDEV.accept_local=1 >/dev/null 2>&1
sysctl -w net.ipv4.conf.$RXDEV.accept_local=1 >/dev/null 2>&1
for i in $(seq 15); do
	[ "$(cat /sys/class/net/$TXDEV/carrier 2>/dev/null)" = 1 ] && [ "$(cat /sys/class/net/$RXDEV/carrier 2>/dev/null)" = 1 ] && break
	sleep 1
done
echo "carrier ok（等待 ${i}s）TX=$TXDEV PEER=$PEER_MAC"

# 同步 start 后台写 + 冻结探测（collect_datapath start_and_wait 简化版）
run_tier() { # name count ratep(0=全速)
	local nm=$1 cnt=$2 rate=$3 t0 stpid n=0 cur last=-1 frozen=0
	echo "rem_device_all" > "/proc/net/pktgen/$KT" || abort "rem 被拒"
	echo "add_device $TXDEV" > "/proc/net/pktgen/$KT" || abort "add 被拒"
	for kv in "count $cnt" "pkt_size 64" "delay 0" "clone_skb 0" "dst_mac $PEER_MAC" "dst $IP2" "src_min $IP1"; do
		echo "$kv" > "/proc/net/pktgen/$TXDEV" || abort "pg_set '$kv' 被拒"
	done
	[ "$rate" != 0 ] && { echo "ratep $rate" > "/proc/net/pktgen/$TXDEV" || abort "ratep 被拒"; }
	cat "/proc/net/pktgen/$TXDEV" > "$OUT/${nm}_cfg.txt"
	local txb=$(cat /sys/class/net/$TXDEV/statistics/tx_packets)
	t0=$(date +%s.%N)
	( timeout 280 bash -c "echo start > '$PGCTRL'" ) &
	stpid=$!
	while :; do
		grep -qE '^Result: OK: [0-9]+\(c' "/proc/net/pktgen/$TXDEV" 2>/dev/null && break
		kill -0 "$stpid" 2>/dev/null || break
		sleep 0.1
		n=$((n + 1))
		if [ $((n % 200)) = 0 ]; then
			cur=$(timeout 2 awk '/^Current:/{getline; print; exit}' "/proc/net/pktgen/$TXDEV" 2>/dev/null | grep -oP 'pkts-sofar: \K[0-9]+')
			cur=${cur:-0}
			echo "   [$((n / 10))s] sofar=$cur tx_pkts=$(cat /sys/class/net/$TXDEV/statistics/tx_packets)"
			[ "$cur" = "$last" ] && frozen=$((frozen + 1)) || frozen=0
			last=$cur
			[ "$frozen" -ge 2 ] && { echo "   !! 冻结于 sofar=$cur/$cnt"; break; }
		fi
		[ "$n" -ge 2800 ] && { echo "   !! 280s 超时"; break; }
	done
	kill "$stpid" 2>/dev/null; wait "$stpid" 2>/dev/null
	grep -qE '^Result: OK: [0-9]+\(c' "/proc/net/pktgen/$TXDEV" 2>/dev/null || timeout 5 bash -c "echo stop > '$PGCTRL'" 2>/dev/null
	local el=$(awk "BEGIN{printf \"%.1f\", $(date +%s.%N) - $t0}")
	echo "$nm: elapsed=${el}s sofar_final=$(grep -oP 'usec, \K[0-9]+' "/proc/net/pktgen/$TXDEV" || echo NA) tx_packets=$(( $(cat /sys/class/net/$TXDEV/statistics/tx_packets) - txb ))"
	cp "/proc/net/pktgen/$TXDEV" "$OUT/${nm}_result.txt"
	cp "$TR/trace" "$OUT/${nm}_trace.txt"
	: > "$TR/trace"
}

echo global > "$TR/trace_clock"
say "E1：全速突发 100k（预期冻结复现 + EQE 证据）"
echo 1 > "$TR/events/mlx5_prism/prism_eq_event/enable"
echo 1 > "$TR/events/mlx5_prism/prism_sq_xmit/enable"
echo 1 > "$TR/tracing_on"
: > "$TR/trace"
run_tier e1-burst 100000 0

say "E2：限速 5kpps ×5000（预期完成——膝盖佐证）"
: > "$TR/trace"
run_tier e2-rate5k 5000 5000

echo 0 > "$TR/tracing_on"
echo 0 > "$TR/events/mlx5_prism/prism_eq_event/enable"
echo 0 > "$TR/events/mlx5_prism/prism_sq_xmit/enable"

say "EQE 类型分布（两腿 trace 汇总）"
for f in "$OUT"/e*_trace.txt; do
	echo "-- $(basename "$f")"
	grep -oP 'prism_eq_event: eqn=\d+ type=0x\K[0-9a-f]+' "$f" | sort | uniq -c | sed 's/^/   type 0x/'
	echo "   eqe 总数: $(grep -c 'prism_eq_event' "$f")  sq_xmit 总数: $(grep -c 'prism_sq_xmit' "$f")"
	echo "   最后 3 条 eqe:"; grep 'prism_eq_event' "$f" | tail -3 | sed 's/^/     /'
done

say "完成：$OUT（trace/result/cfg 永久保留）"
