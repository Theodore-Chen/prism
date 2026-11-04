#!/bin/bash
# collect_datapath.sh — T18 数据面基准底座：四臂 pps/带宽/时延/CPU 采集
# （prism 基线 / core 钉同几何 / 线速换算 / RX 端到端时延分布）。
# 零驱动改动，纯采集；本机无 perf/bpftrace（T11 裁决，tracefs 直采）。
#
# ============ 口径声明（跑之前固化） ============
# 判据（数据齐全性判定，非性能阈值——对号入座，不事后改）：
#   D1 四臂数据齐：prism 基线 pps（64B/1514B 双档）、core 钉同几何 pps
#      双档、线速百分比表、RX 时延分布（两低速率档各 n≥500 配对样本）。
#   D2 core 钉几何确认在案：ethtool -l（combined=1）与 ethtool -k
#      （rx/tx/sg/tso/gso/gro 全 off）双口回读输出存档且值符合。
#   D3 高负载与低负载两形态分开报告；10ms 看门狗地板（DESIGN §6.4）在
#      低负载时延分布中可见或有注记。
#   PASS = D1 ∧ D2 ∧ D3。
# trip-4 修订（2026-11-04，楔死定案）：prism 臂 = 限速梯基线（楔死边界以
#   内）+ 边界入档；全速滞滞机理（距离触发静默停产，doorbell 家族三门
#   反证）移交 T24/T25。core 臂不受影响仍为全速两遍法。
# 数值入档不设阈值：本任务只建事实底座，性能门禁留给 T20 按实测分布
#   预登记（v1「目标先于基线」教训制度化，task.json T18 context）。
# 拓扑（T15 判决沿用）：accept_local 单机双口对拉，TX=port0→RX=port1，
#   169.254.77.1/.2——拓扑税：协议栈路径计入两端，绝对数与裸口有差异。
# 负载生成：pktgen 主档（kpktgend_0 单线程单流，clone_skb 0 单 SKB 复用
#   ——5.15 语义：每包仍走完整 ndo_start_xmit，建包成本不入驱动路径；
#   两遍法：100k/50k 校准趟估 pps → 主趟按 ~5s 窗口自适应钳幅；count
#   上限同时钳 ~120s 运行时）。iperf3 TCP 背景档自动探测，缺席如实记缺
#   （mpstat 同理，CPU 占用走 /proc/softirqs NET_TX/NET_RX 差分）。
# pktgen 5.15 proc 接口（trip-1 系列修正，2026-10-09/10）：
#   trip-1a 控制文件 = pgctrl（v5.15 无 pg/pg_control——那是更新内核的名）；
#   trip-1b 线程天生每核一绑（kthread_bind），亲和用 kpktgend_<CPU> 本尊，
#   taskset 移不动（首跑 EINVAL）；trip-1c 参数名以源码为准 dst_mac/src_mac
#   （v5.15 文档示例 dstmac 与源码脱节——未知 token 落 "No such parameter"
#   -EINVAL，且静默默认值 count=0=无限发送，教训：参数写入一律严格校验）。
#   其余 token：clone_skb/ratep(pps)/delay(ns)/count/pkt_size，per-device
#   文件 = /proc/net/pktgen/<dev>；trip-1d rem_device_all/add_device 是线程
#   命令（写 kpktgend_N），pgctrl 只认 stop/start/reset（源码 514-547）；
#   trip-1e pgctrl start 是同步接口（写进程阻塞到 run 结束——
#   pktgen_wait_thread_run），start 写必须后台化+前台轮询 Result。
#   xmit_mode 默认 M_START_XMIT 直连 hard xmit（qdisc 不入路径——驱动每包
#   成本口径）。
# 包长档：64B / 1514B（pkt_size 含以太头）；线速锚 25GbE：64B 在线 84B
#   ≈ 37.2Mpps、1514B ≈ 2.0Mpps。
# 时延配对：prism_sq_xmit(len) ↔ prism_rx_cqe(byte_cnt) 固定包长档下
#   相等，FIFO 时间序配对；trace_clock=global（T11 惯例，采后恢复）。
# 亲和：负载线程 = kpktgend_CPU$(nproc-1) 本尊（天生绑核），两臂同核（入档）。
# 会话切换：J15 惯例互斥加载（prism↔mlx5_core）；结束态 = core modprobe
#   循环复位（ethtool 设置归零）+ pktgen 卸载。
# 手册项（外部执行）：r0_gate 8/8 复跑不受影响（零驱动改动）。
#
# 输出（全量日志单文件）：/var/tmp/datapath_report.txt（.prev 滚动）
# 快照：/var/tmp/datapath/（永久保留，T11 教训：快照是判据原始证据）
# 黑匣子：/var/tmp/datapath_live.log（dmesg -w 逐行 sync）
#
# Usage: sudo bash scripts/gate/collect_datapath.sh
set -u
cd /home/zqc/code/prism

# ---- 固定口径参数 ----
SZ_LO_HI_64="500000 10000000 100000"     # 主趟下限/上限/校准量
SZ_LO_HI_1514="200000 2000000 50000"
LR64=37200000; LR1514=2000000
LAT_SZ=64
LAT_TIERS="100:8 1000:3"                 # ratepps:秒（+20% count 余量）
IP1=169.254.77.1; IP2=169.254.77.2
PIN_CPU=$(( $(nproc) - 1 ))
LIVE=/var/tmp/datapath_live.log
REPORT=/var/tmp/datapath_report.txt
OUT=/var/tmp/datapath

say() { echo; echo "======== $*"; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; else echo "   [FAIL] $2"; fi
         [ "$1" -eq 0 ] || DFAIL=$((DFAIL + 1)); }
now() { date +%s.%N; }

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
[ -f src/mlx5_prism.ko ] || { echo "src/mlx5_prism.ko 不存在——先在仓库根跑 make"; exit 1; }
for t in ethtool taskset python3 awk; do
	command -v "$t" >/dev/null || { echo "缺少工具：$t"; exit 1; }
done

mkdir -p "$OUT"
# 复跑卫生：清上一轮 run 产物（lspci 每轮重采；历史轮证据在 datapath_report.prev）
rm -f "$OUT"/cell_*.txt "$OUT"/lat_*.txt "$OUT"/latsnap_*.txt \
      "$OUT"/arm4_samples.csv "$OUT"/summary.txt "$OUT"/core_geom_*.txt
mv -f "$REPORT" "$REPORT.prev" 2>/dev/null
# fs.protected_regular=2：root 的 tee(O_CREAT) 打不开 sticky 目录里他人
# 属主的旧报告——先删（root unlink 不受限），末尾 chown 交还调用者。
rm -f "$REPORT"
exec > >(tee "$REPORT") 2>&1
mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
rm -f "$LIVE"
# 黑匣子：dmesg -w 逐行 sync 落盘（/tmp 是 tmpfs 断电即失，DESIGN §11）
stdbuf -oL dmesg -w | while IFS= read -r l; do printf '%s\n' "$l" >> "$LIVE"; sync; done &
LIVE_PID=$!

TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
[ -d "$TR/events" ] || { mount -t tracefs tracefs /sys/kernel/tracing; TR=/sys/kernel/tracing; }
T_ON=$(cat "$TR/tracing_on")
# trace_clock 文件内容是菜单（当前项带中括号）——只取当前项，恢复才合法
CLK=$(sed 's/.*\[\([^]]*\)\].*/\1/' "$TR/trace_clock")

INIT_CORE=$(lsmod | grep -c '^mlx5_core ')
INIT_PKTGEN=$(lsmod | grep -c '^pktgen ')
DFAIL=0
CLEANED=0
cleanup() {
	[ "$CLEANED" = 1 ] && return 0
	CLEANED=1
	for e in prism_sq_xmit prism_rx_cqe; do
		[ -e "$TR/events/mlx5_prism/$e/enable" ] && echo 0 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
	done
	echo "$CLK" > "$TR/trace_clock" 2>/dev/null
	echo "$T_ON" > "$TR/tracing_on" 2>/dev/null
	# 机器恢复：prism 让位、core 复位（modprobe 循环 = ethtool 几何归零）
	# rem_device_all 走线程文件（trip-1d）；清理路径只尽力不强停
	[ -n "${KT:-}" ] && [ -e "/proc/net/pktgen/$KT" ] && \
		echo rem_device_all > "/proc/net/pktgen/$KT" 2>/dev/null
	[ -n "${PGCTRL:-}" ] && timeout 5 bash -c "echo stop > '$PGCTRL'" 2>/dev/null
	[ "$INIT_PKTGEN" = 0 ] && modprobe -r pktgen 2>/dev/null
	rmmod mlx5_prism 2>/dev/null
	if [ "$INIT_CORE" != 0 ]; then
		# udev 会在驱动重绑时复活 mlx5_ib 使 core 卸载失败（trip-3 实测）
		modprobe -r mlx5_ib 2>/dev/null
		modprobe -r mlx5_core 2>/dev/null
		if ! lsmod | grep -q '^mlx5_core '; then
			modprobe mlx5_core 2>/dev/null
			# 循环失败的兜底：残留拓扑 IP 精确清除（网口名可能已变，按 15b3 扫）
			for nd in /sys/class/net/*; do
				nd=${nd##*/}
				[ -f "/sys/class/net/$nd/device/vendor" ] || continue
				[ "$(cat "/sys/class/net/$nd/device/vendor" 2>/dev/null)" = "0x15b3" ] || continue
				ip addr del $IP1/24 dev "$nd" 2>/dev/null
				ip addr del $IP2/24 dev "$nd" 2>/dev/null
			done
		fi
	fi
	# 黑匣子最后停：teardown/恢复段的 dmesg 也要在案
	kill "$LIVE_PID" 2>/dev/null
	[ -n "${SUDO_UID:-}" ] && chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$REPORT" "$OUT" "$LIVE" 2>/dev/null
}
trap cleanup EXIT
trap 'echo; echo "!!! SIGINT——清理后退出（快照在案）"; cleanup; exit 130' INT
trap 'echo; echo "!!! SIGTERM——清理后退出"; cleanup; exit 143' TERM
abort() {
	echo "!!! 中止：$*"
	echo "!!! 状态自查：lsmod | grep -E 'mlx5_prism|mlx5_core|pktgen'"
	echo "!!! 快照保留在 $OUT 供诊断"
	exit 1
}

# ---- pktgen proc 接口 ----
PGCTRL=""	# preflight 发现：pgctrl（5.15）或 pg_control（新内核）
KT=""	# 负载线程本尊：kpktgend_<PIN_CPU>（天生绑核，taskset 移不动）
# 严格写：proc 写失败立即中止（trip-1c 教训——静默默认值引发无限发送）
pg_ctrl() { echo "$1" > "$PGCTRL" || abort "pgctrl 写入被拒：$1"; }
pg_set() { echo "$2" > "/proc/net/pktgen/$1" || abort "pg_set $1 '$2' 被拒（token/值非法——对照脚本头 trip-1c 名单）"; }

# ---- 快照与差分助手 ----
link_snap() { # dev outfile
	local d=$1 f=$2 s
	{ for s in tx_packets rx_packets tx_errors tx_dropped rx_dropped; do
		echo "$s $(cat "/sys/class/net/$d/statistics/$s" 2>/dev/null || echo NA)"
	  done
	  echo "-- ip -s -s link"
	  ip -s -s link show dev "$d" 2>/dev/null
	} > "$f"
}
stat_diff() { awk 'NR==FNR{m[$1]=$2;next} ($1 in m){print $1" +"$2-m[$1]}' "$1" "$2"; }
si_snap() { grep -E '(NET_TX|NET_RX):' /proc/softirqs > "$1"; }
si_diff() { awk 'NR==FNR{for(i=2;i<=NF;i++)a[substr($1,1,length($1)-1)]+=$i;next}
	{for(i=2;i<=NF;i++)b[substr($1,1,length($1)-1)]+=$i}
	END{printf "NET_TX +%d NET_RX +%d\n",b["NET_TX"]-a["NET_TX"],b["NET_RX"]-a["NET_RX"]}' "$1" "$2"; }

# trip-1e（2026-10-10）：v5.15 的 pgctrl start 是同步接口——写进程阻塞到
# run 结束才返回（pktgen_run_all_threads→pktgen_wait_thread_run），前台直写
# 必挂。改为：start 写后台监护跑，前台轮询设备 Result 完成态 + sofar 进度
# （每 20s 诊断入报告）；sofar 连续两窗冻结（≥40s 零进展）判停滞提前收口。
start_and_wait() { # count → LAST_STATUS=ok/invalid
	local tmo=$(( $1 / 1000 + 180 )); [ "$tmo" -gt 480 ] && tmo=480
	local n=0 cur lastsofar=-1 frozen=0 st STSRC=""
	( timeout "$tmo" bash -c "echo start > '$PGCTRL'" ) &
	local stpid=$!
	until timeout 2 grep -qE '^Result: OK: [0-9]+\(c' "/proc/net/pktgen/$TXDEV" 2>/dev/null; do
		kill -0 "$stpid" 2>/dev/null || break	# 后台写退出 = run 结束/失败
		sleep 0.1
		n=$((n + 1))
		if [ $((n % 200)) = 0 ]; then
			st=$(timeout 2 awk '/^Current:/{getline; print; exit}' "/proc/net/pktgen/$TXDEV" 2>/dev/null)
			echo "   [$((n / 10))s] $st  txq=$(cat /sys/class/net/$TXDEV/queues/tx-0/state 2>/dev/null)  tx_pkts=$(cat /sys/class/net/$TXDEV/statistics/tx_packets 2>/dev/null)"
			cur=$(echo "$st" | grep -oP 'pkts-sofar: \K[0-9]+')
			cur=${cur:-0}
			if [ "$cur" = "$lastsofar" ]; then frozen=$((frozen + 1)); else frozen=0; fi
			lastsofar=$cur
			if [ "$frozen" -ge 2 ]; then
				echo "   !! sofar 冻结于 $cur/$1 两窗——判停滞，提前收口（快照在案）"
				STSRC=frozen
				break
			fi
		fi
		if [ "$n" -ge $((tmo * 10)) ]; then echo "   !! ${tmo}s 超时未见完成（快照在案）"; STSRC=timeout; break; fi
	done
	# 状态判定必须先于 kill：TERM 会使写内核走 signal 路径→pktgen_stop→
	# 写出完成态 Result（部分统计），事后 grep 会误判 ok（trip-3b）
	case "${STSRC:-}" in
	frozen|timeout)
		LAST_STATUS=invalid ;;
	*)
		# until 正常退出（Result 已现）或后台写自行退出（run 结束）→ 再核一次
		if timeout 2 grep -qE '^Result: OK: [0-9]+\(c' "/proc/net/pktgen/$TXDEV" 2>/dev/null; then
			LAST_STATUS=ok
		else
			LAST_STATUS=invalid
		fi ;;
	esac
	# 直接收割（TERM 唤醒写内的 msleep_interruptible），不等内部 timeout 到期
	kill "$stpid" 2>/dev/null
	wait "$stpid" 2>/dev/null
	[ "$LAST_STATUS" = invalid ] && timeout 5 bash -c "echo stop > '$PGCTRL'" 2>/dev/null
}

# 单趟 pktgen 负载：配置→快照→start→等 Result→快照→差分。
# 产出全局：LAST_EL/LAST_RXD/LAST_TXD/LAST_PG/LAST_TXERR/LAST_TXDRP/LAST_SI/LAST_STATUS
run_packet_cell() { # name sz count [rate=0 全速]
	local nm=$1 sz=$2 cnt=$3 rate=${4:-0} t0 t1
	echo "--- cell $nm: pkt_size=$sz count=$cnt"
	# rem_device_all 是线程命令（kpktgend_N）——pgctrl 只认 stop/start/reset
	echo "rem_device_all" > "/proc/net/pktgen/$KT" || abort "rem_device_all → $KT 被拒"
	echo "add_device $TXDEV" > "/proc/net/pktgen/$KT" || abort "add_device $TXDEV → $KT 失败"
	pg_set "$TXDEV" "count $cnt"
	pg_set "$TXDEV" "pkt_size $sz"
	if [ "$rate" != 0 ]; then
		pg_set "$TXDEV" "ratep $rate"   # 限速梯（trip-4：楔死边界以内）
	else
		pg_set "$TXDEV" "delay 0"
	fi
	pg_set "$TXDEV" "clone_skb 0"
	pg_set "$TXDEV" "dst_mac $PEER_MAC"
	pg_set "$TXDEV" "dst $IP2" 2>/dev/null
	pg_set "$TXDEV" "src_min $IP1" 2>/dev/null
	cat "/proc/net/pktgen/$TXDEV" > "$OUT/cell_${nm}_cfg.txt"
	link_snap "$TXDEV" "$OUT/cell_${nm}_txb.txt"
	link_snap "$RXDEV" "$OUT/cell_${nm}_rxb.txt"
	si_snap "$OUT/cell_${nm}_sib.txt"
	grep -iE "prism|mlx5|enp8" /proc/interrupts > "$OUT/cell_${nm}_irqb.txt" 2>/dev/null
	t0=$(now)
	start_and_wait "$cnt"
	t1=$(now)
	LAST_EL=$(awk "BEGIN{printf \"%.3f\", $t1-$t0}")
	cat "/proc/net/pktgen/$TXDEV" > "$OUT/cell_${nm}_result.txt"
	link_snap "$TXDEV" "$OUT/cell_${nm}_txa.txt"
	link_snap "$RXDEV" "$OUT/cell_${nm}_rxa.txt"
	si_snap "$OUT/cell_${nm}_sia.txt"
	grep -iE "prism|mlx5|enp8" /proc/interrupts > "$OUT/cell_${nm}_irqa.txt" 2>/dev/null
	LAST_RXD=$(stat_diff "$OUT/cell_${nm}_rxb.txt" "$OUT/cell_${nm}_rxa.txt" | awk '$1=="rx_packets"{print $2}' | tr -d +)
	LAST_TXD=$(stat_diff "$OUT/cell_${nm}_txb.txt" "$OUT/cell_${nm}_txa.txt" | awk '$1=="tx_packets"{print $2}' | tr -d +)
	LAST_TXERR=$(stat_diff "$OUT/cell_${nm}_txb.txt" "$OUT/cell_${nm}_txa.txt" | awk '$1=="tx_errors"{print $2}' | tr -d +)
	LAST_TXDRP=$(stat_diff "$OUT/cell_${nm}_txb.txt" "$OUT/cell_${nm}_txa.txt" | awk '$1=="tx_dropped"{print $2}' | tr -d +)
	LAST_PG=$(grep '^Result:' "$OUT/cell_${nm}_result.txt" | grep -oP 'usec, \K[0-9]+' || echo NA)
	LAST_PGPPS=$(grep -oP '\d+(?=pps)' "$OUT/cell_${nm}_result.txt" | head -1 || echo NA)
	LAST_SI=$(si_diff "$OUT/cell_${nm}_sib.txt" "$OUT/cell_${nm}_sia.txt")
	echo "   elapsed=${LAST_EL}s  rx=+$LAST_RXD tx=+$LAST_TXD pktgen=$LAST_PG(自报${LAST_PGPPS}pps)  tx_errors=+$LAST_TXERR tx_dropped=+$LAST_TXDRP  $LAST_SI"
}

declare -A ELAPSED PPS_RX PPS_TX PGSENT DROPS STATUS LADDER
# trip-4 收官形态（2026-11-04 楔死定案后）：prism 突发全速必然滞滞——固件
# 在「产出−已发布 ci」距离 ≥ CQ 深度时静默停产（64→66 / 512→514 标度律，
# 三门 doorbell 反证在案），事件路径重设计属 T24/T25。prism 基线改为限速
# 梯：档位由低到高，每档 ~4s；首个冻结档 = 楔死边界（其后队列已停，更高
# 档无意义，break）；最高完成档 = 该包长基线 pps（bug 受限，如实入档）。
prism_cell_load() { # key tag sz
	local key=$1 tag=$2 sz=$3 r cnt best=0 bestrx=0 besttx=0 ladder="" i
	for r in $PRISM_RATES; do
		# 档间复位：上一档可能楔死（队列停摆传染）——down/up 重建环与队列
		ip link set "$TXDEV" down; sleep 1; ip link set "$TXDEV" up
		for i in $(seq 15); do
			[ "$(cat /sys/class/net/$TXDEV/carrier 2>/dev/null)" = 1 ] && break
			sleep 1
		done
		cnt=$(( r * 4 ))
		run_packet_cell "${tag}-r${r}" "$sz" "$cnt" "$r"
		local rpps
		rpps=$(awk "BEGIN{printf \"%.0f\", ${LAST_RXD:-0} / ${LAST_EL:-1}}")
		ladder="$ladder ${r}:${LAST_STATUS}/txe=${LAST_TXERR:-NA}/rxpps=${rpps}"
		# 基线档 = 零 TX 丢弃的最高档（数据质量限定，非性能阈值——数值
		# 入档不设阈值原则；丢弃档位照记入 ladder，供 T22/T24/T25 引用）
		if [ "$LAST_STATUS" = ok ] && [ "${LAST_TXERR:-1}" = 0 ]; then
			best=$r
			bestrx=$rpps
			besttx=$(awk "BEGIN{printf \"%.0f\", ${LAST_TXD:-0} / $LAST_EL}")
			ELAPSED[$key]=$LAST_EL; PGSENT[$key]=${LAST_PG:-NA}
			DROPS[$key]="tx_errors=+0 tx_dropped=+0"
		elif [ "$LAST_STATUS" != ok ]; then
			echo "   楔死边界 ≈ ${r}pps（该档冻结——机理见脚本头）"
		fi
		sleep 2
	done
	if [ "$best" != 0 ]; then
		STATUS[$key]=ok; PPS_RX[$key]=$bestrx; PPS_TX[$key]=$besttx
	else
		STATUS[$key]=invalid; ELAPSED[$key]=0; PPS_RX[$key]=0; PPS_TX[$key]=0; PGSENT[$key]=0
	fi
	LADDER[$key]="$ladder"
	echo "   梯结果：$ladder"
	echo "   基线档 = ${best}pps（零 TX 丢弃最高档）：delivered ${bestrx}pps"
}

# 两遍法 cell（core 臂专用——无楔死）：校准趟 → 主趟（~5s 窗口自适应）
cell_load() { # key tag sz lo hi cal
	local key=$1 tag=$2 sz=$3 lo=$4 hi=$5 cal=$6 est main
	run_packet_cell "${tag}-cal" "$sz" "$cal"
	if [ "$LAST_STATUS" != ok ]; then STATUS[$key]=invalid; ELAPSED[$key]=0; PPS_RX[$key]=0; PPS_TX[$key]=0; PGSENT[$key]=0; DROPS[$key]=NA; return; fi
	est=$(awk "BEGIN{printf \"%.0f\", $cal / $LAST_EL}")
	main=$(awk -v e=$est -v lo=$lo -v hi=$hi 'BEGIN{c=e*5; if(c<lo)c=lo; if(c>hi)c=hi; if(c>e*120)c=e*120; printf "%.0f", c}')
	echo "   校准 pps≈$est → 主趟 count=$main"
	sleep 2
	run_packet_cell "$tag" "$sz" "$main"
	ELAPSED[$key]=$LAST_EL; STATUS[$key]=$LAST_STATUS
	PPS_RX[$key]=$(awk "BEGIN{printf \"%.0f\", ${LAST_RXD:-0} / $LAST_EL}")
	PPS_TX[$key]=$(awk "BEGIN{printf \"%.0f\", ${LAST_TXD:-0} / $LAST_EL}")
	PGSENT[$key]=${LAST_PG:-NA}
	DROPS[$key]="tx_errors=+${LAST_TXERR:-NA} tx_dropped=+${LAST_TXDRP:-NA}"
}

topo_up() { # txdev rxdev —— T15 判决：accept_local 单机双口对拉
	local a=$1 b=$2 i c0=0 c1=0
	ip link set "$a" up; ip link set "$b" up
	ip addr add $IP1/24 dev "$a" 2>/dev/null
	ip addr add $IP2/24 dev "$b" 2>/dev/null
	ip link set "$a" arp on 2>/dev/null; ip link set "$b" arp on 2>/dev/null
	sysctl -w "net.ipv4.conf.$a.accept_local=1" >/dev/null 2>&1
	sysctl -w "net.ipv4.conf.$b.accept_local=1" >/dev/null 2>&1
	for i in $(seq 15); do	# carrier 有界等待（PAOS 竞态观察，非门禁）
		c0=$(cat "/sys/class/net/$a/carrier" 2>/dev/null)
		c1=$(cat "/sys/class/net/$b/carrier" 2>/dev/null)
		[ "$c0" = 1 ] && [ "$c1" = 1 ] && break
		sleep 1
	done
	echo "   carrier: $a=$c0 $b=$c1（等待 ${i}s）"
}
topo_down() { # 降链路 + 清本拓扑 IP（trip-3：残留 IP 会被安全闸拒之门外）
	ip link set "$1" down 2>/dev/null; ip link set "$2" down 2>/dev/null
	ip addr del $IP1/24 dev "$1" 2>/dev/null
	ip addr del $IP2/24 dev "$2" 2>/dev/null
}

kern_check() { dmesg | grep -E 'WARNING|Oops|BUG' >/dev/null && { dmesg | grep -E 'WARNING|Oops|BUG'; abort "内核投诉（$1）"; }; return 0; }

echo "=== T18 数据面基准采集 $(date '+%F %T') kernel $(uname -r) ==="
echo "可选工具探测：iperf3=$(command -v iperf3 || echo 缺席-记缺)  mpstat=$(command -v mpstat || echo 缺席-记缺)"
echo "负载线程亲和：kpktgend_$PIN_CPU（两臂同核）"

say "阶段 0 preflight：安全闸 + pktgen 就绪"
CX_IFS=""
for nd in /sys/class/net/*; do
	nd=${nd##*/}
	[ -f "/sys/class/net/$nd/device/vendor" ] || continue
	[ "$(cat "/sys/class/net/$nd/device/vendor" 2>/dev/null)" = "0x15b3" ] && CX_IFS="$CX_IFS $nd"
done
CX_IFS=${CX_IFS# }
[ "$(echo $CX_IFS | wc -w)" = "2" ] || abort "15b3 网口数异常：'$CX_IFS'（预期两口 0000:08:00.0/.1）"
RESIDUE=0
for ifc in $CX_IFS; do
	# 安全闸：只碰无 IP 的 Mellanox 网口——绝不碰承载管理/SSH 流量的口。
	# 窄域例外（trip-3）：精确匹配本脚本拓扑残留（169.254.77.1/.2 且口为
	# DOWN）→ 自清后放行；任何其他地址形态照旧拒。
	a4=$(ip -4 addr show dev "$ifc" 2>/dev/null | grep -c 'inet ')
	a6=$(ip -6 addr show dev "$ifc" scope global 2>/dev/null | grep -c inet6)
	own=$(ip -4 addr show dev "$ifc" 2>/dev/null | grep -c "inet $IP1/24\|inet $IP2/24")
	st=$(cat "/sys/class/net/$ifc/operstate" 2>/dev/null)
	if [ "$a4" != 0 ] || [ "$a6" != 0 ]; then
		if [ "$a4" = "$own" ] && [ "$a6" = 0 ] && [ "$st" = "down" ]; then
			echo "清除上轮拓扑残留：$ifc ${IP1}/${IP2}（口 DOWN，窄域自清）"
			ip addr del $IP1/24 dev "$ifc" 2>/dev/null
			ip addr del $IP2/24 dev "$ifc" 2>/dev/null
			RESIDUE=$((RESIDUE + 1))
		else
			abort "$ifc 配有非本拓扑 IP（v4=$a4 own=$own v6=$a6 state=$st）——安全闸拒绝对其灌流"
		fi
	fi
done
[ "$RESIDUE" != 0 ] && echo "安全闸：残留清除 ×$RESIDUE（详见上行），继续"
lspci -k -s 0000:08:00.0 > "$OUT/lspci_0.txt"; lspci -k -s 0000:08:00.1 > "$OUT/lspci_1.txt"
echo "安全闸通过：CX 口 = $CX_IFS（无 IP）"
modprobe pktgen || abort "modprobe pktgen 失败——主档负载生成器不可用"
for i in $(seq 50); do [ -d /proc/net/pktgen ] && break; sleep 0.1; done
[ -d /proc/net/pktgen ] || abort "/proc/net/pktgen 未出现"
for f in pgctrl pg_control; do
	[ -e "/proc/net/pktgen/$f" ] && PGCTRL="/proc/net/pktgen/$f" && break
done
[ -n "$PGCTRL" ] || abort "pktgen 控制文件未发现（pgctrl/pg_control 均不在——内核接口异常，快照在案）"
echo "pktgen 控制文件：$PGCTRL"
KT=kpktgend_$PIN_CPU
[ -e "/proc/net/pktgen/$KT" ] || abort "线程 $KT 不存在（$PIN_CPU 离线？ls /proc/net/pktgen/ 在案）"
echo "负载线程：$KT（天生绑 CPU$PIN_CPU，5.15 每核一绑不可迁移）"

# ============ 阶段 A：prism 会话 ×2（trip-2 重构） ============
# trip-2（2026-10-10，停摆尸检判别实验）：首跑实测臂 1 突发下 TX 在 ~67 个
# 完成后停摆（tx_pkts 冻结、poll ci=67 冻结、sofar 冻结、队列停止未唤醒——
# 黑匣子 840 行 poll 证据）。臂 4 低速率改在新鲜会话先跑：若低速率也冻结于
# 累计 ~64 包 → SW owner 奇偶回卷 bug（任何流量 >64 包楔死）；若无限跑 →
# 突发 overrun 病理（低负载路径健康）。两态都是判据证据，数值入档。
prism_session() { # 装载 + 拓扑；TXDEV/RXDEV/PEER_MAC 就绪
	rmmod mlx5_prism 2>/dev/null
	modprobe -r mlx5_ib 2>/dev/null; modprobe -r mlx5_core 2>/dev/null
	dmesg -c >/dev/null
	insmod src/mlx5_prism.ko || abort "insmod mlx5_prism 失败"
	local i
	for i in $(seq 150); do [ -d /sys/class/net/prism0 ] && [ -d /sys/class/net/prism1 ] && break; sleep 0.1; done
	[ -d /sys/class/net/prism1 ] || abort "prism0/1 netdev 未出现（15s）"
	kern_check "prism 加载"
	TXDEV=prism0; RXDEV=prism1
	PEER_MAC=$(cat /sys/class/net/$RXDEV/address)
	echo "TX=$TXDEV RX=$RXDEV PEER_MAC=$PEER_MAC"
	topo_up "$TXDEV" "$RXDEV"
}

say "阶段 A-1：prism 会话 #1（J15 反向切换）——臂 4 低负载先跑（判别实验）"
prism_session

say "臂 4：RX 端到端时延分布（低负载形态，trace_clock=global）"
echo global > "$TR/trace_clock"
LATOK_100=1; LATOK_1000=1
for tier in $LAT_TIERS; do
	rate=${tier%%:*}; dur=${tier##*:}
	cnt=$(( rate * dur * 12 / 10 ))
	echo "--- lat tier ${rate}pps × ${dur}s（count=$cnt, pkt_size=$LAT_SZ）"
	echo "rem_device_all" > "/proc/net/pktgen/$KT" || abort "rem_device_all → $KT 被拒"
	echo "add_device $TXDEV" > "/proc/net/pktgen/$KT" || abort "add_device $TXDEV → $KT 失败"
	pg_set "$TXDEV" "count $cnt"
	pg_set "$TXDEV" "pkt_size $LAT_SZ"
	pg_set "$TXDEV" "clone_skb 0"
	pg_set "$TXDEV" "dst_mac $PEER_MAC"
	pg_set "$TXDEV" "dst $IP2" 2>/dev/null
	pg_set "$TXDEV" "src_min $IP1" 2>/dev/null
	pg_set "$TXDEV" "ratep $rate"   # 5.15 pps 档（源码 1122 = delay 1e9/N）；实测速率仍以计数器为准
	echo "   速率控制：ratep $rate（5.15 pps 档）"
	echo 0 > "$TR/tracing_on"
	echo 1 > "$TR/events/mlx5_prism/prism_sq_xmit/enable"
	echo 1 > "$TR/events/mlx5_prism/prism_rx_cqe/enable"
	: > "$TR/trace"
	echo 1 > "$TR/tracing_on"
	link_snap "$RXDEV" "$OUT/latsnap_${rate}pps_rxb.txt"
	t0=$(now)
	start_and_wait "$cnt"
	LATEL=$(awk "BEGIN{printf \"%.1f\", $(now) - $t0}")
	echo 0 > "$TR/tracing_on"
	echo 0 > "$TR/events/mlx5_prism/prism_sq_xmit/enable"
	echo 0 > "$TR/events/mlx5_prism/prism_rx_cqe/enable"
	cp "$TR/trace" "$OUT/lat_${rate}pps.txt"
	link_snap "$RXDEV" "$OUT/latsnap_${rate}pps_rxa.txt"
	RXD=$(stat_diff "$OUT/latsnap_${rate}pps_rxb.txt" "$OUT/latsnap_${rate}pps_rxa.txt" | awk '$1=="rx_packets"{print $2}' | tr -d +)
	ACHV=$(awk "BEGIN{printf \"%.0f\", ${RXD:-0} / $LATEL}")
	echo "   elapsed=${LATEL}s rx=+${RXD:-0} → 实测 ${ACHV}pps（请求 ${rate}pps）"
	if [ "$ACHV" -gt $(( rate * 5 )) ] 2>/dev/null; then
		echo "   !! 实测速率超请求 5 倍——非低负载形态，tier 记 INVALID（快照在案）"
		eval "LATOK_${rate}=0"
	fi
	grep -q 'LOST EVENTS' "$OUT/lat_${rate}pps.txt" && echo "   !! trace 缓冲丢事件——快照在案"
	sleep 1
done
echo "$CLK" > "$TR/trace_clock" 2>/dev/null || echo "   注：trace_clock 恢复为 $CLK 失败（EINVAL）——现状保留，黑匣子在案"
topo_down "$TXDEV" "$RXDEV"
rmmod mlx5_prism || abort "rmmod mlx5_prism 失败"
kern_check "臂 4 会话收尾"

say "阶段 A-2：prism 会话 #2（新鲜环/CQ）——臂 1 高负载"
prism_session
PRISM_RATES="2000 10000 20000 35000"
prism_cell_load prism-64    a1-64    64
prism_cell_load prism-1514  a1-1514  1514
kern_check "臂 1"
topo_down "$TXDEV" "$RXDEV"
rmmod mlx5_prism || abort "rmmod mlx5_prism 失败"
kern_check "阶段 A 收尾"

# ============ 阶段 B：core 会话（臂 2 钉同几何对照） ============
say "阶段 B：core 会话（J15 切换 + 钉同几何）"
modprobe mlx5_core || abort "modprobe mlx5_core 失败"
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
core_ready=0
for i in $(seq 30); do
	ls /sys/kernel/debug/mlx5/*/commands >/dev/null 2>&1 && { core_ready=1; break; }
	sleep 1
done
[ "$core_ready" = 1 ] || echo "!! mlx5_core 命令目录 30s 未出现——继续（几何钉扎不受影响）"
CORE_IFS=""
for nd in /sys/class/net/*; do
	nd=${nd##*/}
	[ -f "/sys/class/net/$nd/device/vendor" ] || continue
	[ "$(cat "/sys/class/net/$nd/device/vendor" 2>/dev/null)" = "0x15b3" ] && CORE_IFS="$CORE_IFS $nd"
done
CORE_IFS=$(echo $CORE_IFS | tr ' ' '\n' | sort | tr '\n' ' '); CORE_IFS=${CORE_IFS% }
[ "$(echo $CORE_IFS | wc -w)" = 2 ] || abort "core 网口数异常：'$CORE_IFS'"
CTXDEV=$(echo $CORE_IFS | awk '{print $1}'); CRXDEV=$(echo $CORE_IFS | awk '{print $2}')
echo "core TX=$CTXDEV RX=$CRXDEV"
echo "-- 钉同几何：combined 1 + 六 offload 全关（唯一变量 = 驱动每包路径）"
for ifc in $CTXDEV $CRXDEV; do
	ethtool -L "$ifc" combined 1 2>&1 | sed 's/^/   -L /'
	ethtool -K "$ifc" rx off tx off sg off tso off gso off gro off 2>&1 | sed 's/^/   -K /'
	ethtool -l "$ifc" > "$OUT/core_geom_l_${ifc}.txt" 2>&1
	ethtool -k "$ifc" > "$OUT/core_geom_k_${ifc}.txt" 2>&1
done
TXDEV=$CTXDEV; RXDEV=$CRXDEV
PEER_MAC=$(cat /sys/class/net/$RXDEV/address)
topo_up "$TXDEV" "$RXDEV"
set -- $SZ_LO_HI_64;    cell_load core-64    a2-64    64    $1 $2 $3
set -- $SZ_LO_HI_1514;  cell_load core-1514  a2-1514  1514  $1 $2 $3
if command -v iperf3 >/dev/null; then
	echo "--- TCP 背景档：iperf3 -t 10（$IP1 → $IP2）"
	iperf3 -s -B $IP2 -1 > "$OUT/iperf3_server.txt" 2>&1 &
	IPSV=$!; sleep 1
	iperf3 -c $IP2 -B $IP1 -t 10 > "$OUT/iperf3_client.txt" 2>&1
	sed -n '/\[SUM\]/p;/receiver/p;/sender/p' "$OUT/iperf3_client.txt" | tail -3
	wait $IPSV 2>/dev/null
else
	echo "--- TCP 背景档：iperf3 缺席——记缺（pktgen 已覆盖 pps 主档）"
	echo "iperf3 missing" > "$OUT/iperf3_client.txt"
fi
topo_down "$TXDEV" "$RXDEV"

# ============ 阶段 C：聚合（臂 3 线速换算 + 臂 4 分布 + 判据对号） ============
say "臂 3：线速换算（pps ÷ 线速锚）"
printf '%-12s %6s %14s %14s %10s %10s\n' arm sz pps_rx pps_tx "64B%" "1514B%"
for arm in prism core; do
	for sz in 64 1514; do
		k=$arm-$sz
		lr=$([ "$sz" = 64 ] && echo $LR64 || echo $LR1514)
		pct=$(awk "BEGIN{printf \"%.3f\", ${PPS_RX[$k]:-0} / $lr * 100}")
		printf '%-12s %6s %14s %14s %10s %10s\n' "$arm" "$sz" "${PPS_RX[$k]:-NA}" "${PPS_TX[$k]:-NA}" \
			"$([ "$sz" = 64 ] && echo "$pct%" || echo -)" "$([ "$sz" = 1514 ] && echo "$pct%" || echo -)"
	done
done

say "臂 4：时延聚合（datapath_aggregate.py）"
AGGOUT=$(python3 scripts/gate/datapath_aggregate.py --work "$OUT" --csv "$OUT/arm4_samples.csv")
echo "$AGGOUT"
N100=$(echo "$AGGOUT" | grep '^lat_100pps' | grep -oP 'paired=\K[0-9]+' || echo 0)
N1000=$(echo "$AGGOUT" | grep '^lat_1000pps' | grep -oP 'paired=\K[0-9]+' || echo 0)

say "判据对号（预登记于脚本头部，数值入档不设阈值）"
d1=0
for k in prism-64 prism-1514 core-64 core-1514; do
	[ "${STATUS[$k]:-}" = ok ] && [ "${PPS_RX[$k]:-0}" -gt 0 ] 2>/dev/null || { d1=1; echo "   D1 缺口：cell $k status=${STATUS[$k]:-missing} pps_rx=${PPS_RX[$k]:-NA}"; }
done
[ "$N100" -ge 500 ] && [ "$N1000" -ge 500 ] 2>/dev/null || { d1=1; echo "   D1 缺口：lat paired n(100pps)=$N100 n(1000pps)=$N1000（需各≥500）"; }
[ "$LATOK_100" = 1 ] && [ "$LATOK_1000" = 1 ] || { d1=1; echo "   D1 缺口：低负载档形态 INVALID（实测速率超请求 5 倍）"; }
res $d1 "D1 四臂数据齐（pps 双档×双臂 + 线速表 + 时延两档 n≥500 且形态合法）"
d2=0
for ifc in $CTXDEV $CRXDEV; do
	comb=$(sed -n '/Current hardware settings/,$p' "$OUT/core_geom_l_${ifc}.txt" | awk '/^Combined:/{print $2}')
	[ "$comb" = 1 ] || { d2=1; echo "   D2：$ifc combined=$comb（需 1）"; }
	for feat in rx-checksumming tx-checksumming scatter-gather tcp-segmentation-offload generic-segmentation-offload generic-receive-offload; do
		grep -qE "^$feat:[[:space:]]+off" "$OUT/core_geom_k_${ifc}.txt" || { d2=1; echo "   D2：$ifc $feat 非 off"; }
	done
done
res $d2 "D2 core 钉几何确认在案（combined=1 + 六 offload 全 off，双口回读存档）"
echo "   D3 注记：臂 1/2 为高负载形态（饱和即丢数据入档）；臂 4 为低负载形态，10ms 看门狗地板可见性见上方「≥8ms 样本占比」行（DESIGN §6.4，数值入档不设阈值）"
res 0 "D3 高/低负载两形态分开报告 + 10ms 地板可见性注记"

{
	echo "date $(date '+%F %T')"
	echo "kernel $(uname -r)"
	echo "topology accept_local-loopback TX->RX single-stream"
	echo "pktgen_cpu $PIN_CPU clone_skb 0 two-pass-cal control=$PGCTRL"
	for arm in prism core; do
		for sz in 64 1514; do
			k=$arm-$sz
			echo "pps $arm $sz ${PPS_RX[$k]:-NA} tx_pps=${PPS_TX[$k]:-NA} pktgen=${PGSENT[$k]:-NA} elapsed=${ELAPSED[$k]:-NA} status=${STATUS[$k]:-NA} drops=${DROPS[$k]:-NA} ladder=${LADDER[$k]:--}"
		done
	done
	echo "lat prism 100pps paired=$N100"
	echo "lat prism 1000pps paired=$N1000"
} > "$OUT/summary.txt"
echo; echo "=== 完成：$REPORT / 快照 $OUT（永久保留）/ 黑匣子 $LIVE ==="
[ "$DFAIL" -eq 0 ] && echo "=== 判据 D1-D3：ALL PASSED ===" || echo "=== 判据：${DFAIL} 项 FAIL（快照在案，走尸检不定罪） ==="
exit "$DFAIL"
