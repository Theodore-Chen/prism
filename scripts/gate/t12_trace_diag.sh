#!/bin/bash
# t12_trace_diag.sh — G3 issue/complete 失配诊断（2026-09-26，rebaseline
# step5 G3 issue=61 complete=62 eq_event=28，T10 时代 84/84/22）。
#
# 已知约束（来自代码与黑匣子，先固定假设空间）：
#   - complete 只在 cmd_exec 成功路径 trace（超时/delivery 错误提前 return，
#     不 trace）→ 单加载内恒有 complete ≤ issue；NC = NI + 1 只能来自
#     "issue 记录被丢弃而对应 complete 幸存"（丢弃窗口恰好结束在 issue 与
#     其 complete 之间）——环形缓冲区覆盖的特征指纹
#   - eq_event=28 = 22+6 精确吻合（PHY 六命令全被硬中断消费）→ 命令本身
#     全部正常完成，缺的只是 trace 记录；若丢弃窗口在发命令的 CPU 的
#     per-CPU 缓冲上，而硬中断固定在另一 CPU，eq_event 可以零损失
#   - per_cpu/cpuN/stats 的 overrun 计数是丢事件的权威证据
#
# 两个证据源：
#   ① 现场抢救：G3 加载的记录可能仍在缓冲区（其后 G4 四十循环在事件
#      禁用下运行，无新写入）——先原样读出分析，不动缓冲区
#   ② 受控复现：清空后按 r0_gate G3 同款武装协议再跑一次 selftest=1，
#      记录前后 per-CPU overrun 差值
#
# 输出全落 /var/tmp/t12_trace_diag.log 单文件；黑匣子不动（沿用
# r0_gate_live.log 的时间线即可对时）。
#
# Usage: sudo bash scripts/gate/t12_trace_diag.sh     (~40 s)

set -u
cd /home/zqc/code/prism

RUN=/var/tmp/t12_trace_diag.log
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
WORK=$(mktemp -d /tmp/prism_tdiag.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

mv -f "$RUN" "$RUN.prev" 2>/dev/null
exec > >(tee "$RUN") 2>&1

TR=/sys/kernel/tracing
[ -d "$TR/events" ] || TR=/sys/kernel/debug/tracing
say() { echo; echo "===== $* ====="; }

prism_counts() { # label
        echo "   $1: issue=$(grep -c 'prism_cmdq_issue:' "$TR/trace")" \
             "complete=$(grep -c 'prism_cmdq_complete:' "$TR/trace")" \
             "eq_event=$(grep -c 'prism_eq_event:' "$TR/trace")" \
             "trace_total=$(wc -l < "$TR/trace")"
}

cpu_stats() { # label
        echo "   -- per_cpu stats $1:"
        for s in "$TR"/per_cpu/cpu*/stats; do
                echo "   $(basename "$(dirname "$s")"): $(tr '\n' ' ' < "$s")"
        done
}

pair_dump() { # outfile — dump prism records + multiset pairing analysis
        grep -E 'prism_cmdq_(issue|complete):' "$TR/trace" | \
                sed 's/^[^:]*: *//' > "$1"
        awk '/prism_cmdq_issue:/ { I++ }
             /prism_cmdq_complete:/ { C++ }
             END { printf "   records: issue=%d complete=%d %s\n", I, C,
                     (I == C ? "paired" : "MISMATCH") }' "$1"
        for side in issue complete; do
                grep "prism_cmdq_${side}:" "$1" | \
                        grep -oE 'opcode=0x[0-9a-f]+|slot=[0-9]+' | \
                        paste - - | sort | uniq -c > "$WORK/$side.c"
        done
        if diff "$WORK/issue.c" "$WORK/complete.c" > "$WORK/pair.diff"; then
                echo "   (opcode,slot) multiset: identical"
        else
                echo "   (opcode,slot) multiset diff (< issue / > complete):"
                sed 's/^/     /' "$WORK/pair.diff"
        fi
}

# ------------------------------------------------------------- ① 现场抢救
say "scene 1: salvage the buffer as-is (G3 records may still be there)"
echo "   tracing_on=$(cat "$TR/tracing_on") buffer_size_kb=$(cat "$TR/buffer_size_kb")" \
     "total_kb=$(cat "$TR/buffer_total_size_kb" 2>/dev/null) tracer=$(cat "$TR/current_tracer")"
echo "   n_cpus=$(nproc) enabled_events=$(ls "$TR/events/mlx5_prism/" 2>/dev/null | wc -l)"
cpu_stats before-anything
prism_counts "as-is counts"
if grep -q 'prism_cmdq_' "$TR/trace"; then
        echo "   -- lost/overrun markers in the live trace:"
        grep -nE 'LOST|overrun|CPU:[0-9]+' "$TR/trace" | head -10 || echo "     (none found)"
        pair_dump "$WORK/salvage.txt"
        echo "   -- full salvaged prism record list appended at the end of this log"
        cp "$WORK/salvage.txt" "$WORK/salvage.keep"
else
        echo "   (no prism records in buffer — G3 evidence already gone)"
fi

# ------------------------------------------------------------- ② 受控复现
say "scene 2: controlled reproduction (r0_gate G3 arming protocol)"
modprobe -r mlx5_ib 2>/dev/null
modprobe -r mlx5_core 2>/dev/null
rmmod mlx5_prism 2>/dev/null
echo 1 > "$TR/tracing_on"
: > "$TR/trace"
( while [ ! -d "$TR/events/mlx5_prism/prism_cmdq_issue" ]; do :; done
  for e in prism_cmdq_issue prism_cmdq_complete prism_eq_event; do
        echo 1 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
  done ) &
ARM=$!
dmesg -c > /dev/null
insmod src/mlx5_prism.ko selftest=1 || { echo "insmod failed"; kill $ARM 2>/dev/null; exit 1; }
wait $ARM
for i in $(seq 20); do
        [ "$(grep -c 'prism_cmdq_issue:' "$TR/trace")" -gt 30 ] && break
        sleep 1
done
prism_counts "fresh counts"
cpu_stats after-run
pair_dump "$WORK/fresh.txt"

for e in prism_cmdq_issue prism_cmdq_complete prism_eq_event; do
        echo 0 > "$TR/events/mlx5_prism/$e/enable" 2>/dev/null
done
rmmod mlx5_prism
modprobe mlx5_core 2>/dev/null && echo "mlx5_core restored"

if [ -n "${SUDO_UID:-}" ]; then chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" src; fi

say "append: full salvaged + fresh prism record lists"
for f in "$WORK/salvage.keep" "$WORK/fresh.txt"; do
        [ -f "$f" ] || continue
        echo "--- $f:"
        cat "$f"
done
say "done — evidence: $RUN"
