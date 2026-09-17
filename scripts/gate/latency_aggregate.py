#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""T11 延迟基线聚合器 — collect_latency.sh 的分析段（也可独立运行）.

输入（--work DIR）：
  prism_load_NN.txt  每次 insmod selftest=1 的 tracefs 快照
  mlx5_core.txt      mlx5_cmd_invoke kprobe 快照（缺失则输出回退注记）

口径（与 DESIGN.md §10.1 对齐）：
  P1'  cmdq_complete tracepoint 的 lat= 字段，内核 ktime 实测
      （doorbell 写入 → slot owner 翻转被观察到，轮询实现）。
  P2'  eq_event 时间戳 − 配对 cmdq_complete 时间戳。配对依据 EQE
      vector 位图 bit i ↔ slot i；槽位在被 EQE 确认前不释放，单次
      加载内配对唯一，故逐文件（逐加载）配对。
  分位数 nearest-rank：sorted[ceil(p*n)-1]。
"""
import argparse
import csv
import glob
import math
import os
import re
import sys
from collections import defaultdict

# tracefs 行前缀: comm-pid [cpu] 上下文标志 ts: ...
LINE = re.compile(r'^(?P<comm>.+)-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]'
                  r'\s+\S+\s+(?P<ts>\d+\.\d+):\s+(?P<rest>\S+: .*)$')
COMPLETE = re.compile(r'^prism_cmdq_complete: opcode=0x[0-9a-f]+\[[^\]]+\]'
                      r' slot=(?P<slot>\d+) delivery=0x[0-9a-f]+'
                      r' status=0x[0-9a-f]+ syndrome=0x[0-9a-f]+'
                      r' lat=(?P<lat>\d+) ns err=-?\d+')
EQEV = re.compile(r'^prism_eq_event: eqn=(?P<eqn>\d+) type=0x(?P<type>[0-9a-f]+)'
                  r' vector=0x(?P<vector>[0-9a-f]+) cons=\d+')
ISSUE = re.compile(r'^prism_cmdq_issue: opcode=0x[0-9a-f]+\[(?P<opcode>[^\]]+)\]')
KPROBE = re.compile(r'^(?P<ev>ml5i|ml5r): ')


def parse_prism_file(path):
    """返回 (issues, completes, eqs)；issues=[opcode 名]，completes=
    [(ts_ns,slot,lat_ns)]，eqs=[(ts_ns,vector,eqn,type)]，均按文件顺序。"""
    issues, completes, eqs = [], [], []
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            ts_ns = int(round(float(m.group('ts')) * 1e9))
            rest = m.group('rest')
            if (i := ISSUE.match(rest)):
                issues.append(i.group('opcode'))
            elif (c := COMPLETE.match(rest)):
                completes.append((ts_ns, int(c.group('slot')),
                                  int(c.group('lat'))))
            elif (e := EQEV.match(rest)):
                eqs.append((ts_ns, int(e.group('vector'), 16),
                            int(e.group('eqn')), int(e.group('type'), 16)))
    return issues, completes, eqs


def pair_p2(completes, eqs, max_slots=64):
    """流式配对：completes 与 eqs 按时间线合并，complete 置 pending[slot]，
    EQE 的 vector 位逐位认领**当时**的 pending 槽位 → ns 样本。
    （run-1 教训：预载全量 completes 的静态配对在槽位复用下把早 EQE 配给
    晚 complete，产出大负值样本——EQ_DEFERRED 语义下槽位在被 EQE 确认前
    不释放，同一时刻每槽至多一个未决 complete，流式认领无歧义。）
    返回 (samples, unpaired_completes, orphan_bits)。"""
    timeline = sorted([(ts, 0, slot) for ts, slot, _ in completes] +
                      [(ts, 1, vector) for ts, vector, _, _ in eqs])
    pending, samples, orphan = {}, [], 0
    for ts, kind, val in timeline:
        if kind == 0:
            pending[val] = ts
            continue
        if not val:
            continue
        for b in range(min(max_slots, val.bit_length())):
            if val & (1 << b):
                if b in pending:
                    samples.append(ts - pending.pop(b))
                else:
                    orphan += 1
    return samples, len(pending), orphan


def pair_kprobes(path):
    """ml5i 入口 / ml5r 返回按 pid 栈式配对 → ns 样本。"""
    stack, samples = defaultdict(list), []
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            k = KPROBE.match(m.group('rest'))
            if not k:
                continue
            ts_ns = int(round(float(m.group('ts')) * 1e9))
            pid = m.group('pid')
            if k.group('ev') == 'ml5i':
                stack[pid].append(ts_ns)
            elif stack[pid]:
                samples.append(ts_ns - stack[pid].pop())
    return samples


def dist(ns_samples):
    """n/min/avg/p50/p90/p99/max（ns），nearest-rank 分位。"""
    if not ns_samples:
        return None
    s = sorted(ns_samples)
    n = len(s)

    def pct(p):
        return s[max(0, math.ceil(p / 100 * n) - 1)]

    return dict(n=n, min=s[0], avg=sum(s) / n, p50=pct(50),
                p90=pct(90), p99=pct(99), max=s[-1])


def fmt_us(v):
    return f"{v / 1000:,.1f}"


def dist_line(tag, d):
    if d is None:
        return f"{tag}: 无样本"
    return (f"{tag}: n={d['n']}  min={fmt_us(d['min'])}us  "
            f"avg={fmt_us(d['avg'])}us  p50={fmt_us(d['p50'])}us  "
            f"p90={fmt_us(d['p90'])}us  p99={fmt_us(d['p99'])}us  "
            f"max={fmt_us(d['max'])}us")


# 判据 C 预登记阈值（T10-prep 遗留：commit 长尾是否随环尺寸消失）
P99_TAIL_US, MAX_TAIL_US, TAIL_US = 10_000, 100_000, 500_000


def verdict_c(p2_ns):
    """预登记裁决：长尾消失 / 长尾仍在 / 中间形态。"""
    if not p2_ns:
        return "判据C：无 P2' 样本——采集或配对失败，不裁决"
    d = dist(p2_ns)
    n_tail = sum(1 for x in p2_ns if x > TAIL_US * 1000)
    v = (f"判据C（预登记）：n={d['n']}，p99={fmt_us(d['p99'])}us，"
         f"max={fmt_us(d['max'])}us，>500ms 样本 {n_tail} 条\n")
    if d['p99'] < P99_TAIL_US * 1000 and d['max'] < MAX_TAIL_US * 1000:
        v += "  → 长尾消失：4096 深环下不复存在，T10 直接上事件模式"
    elif n_tail:
        v += "  → 长尾仍在：与环尺寸解耦，怪癖档案恢复该条目"
    else:
        v += "  → 中间形态：如实记录，留给 T10 事件模式再判"
    if d['n'] < 50:
        v += "\n  （注：样本量 <50，结论偏弱）"
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', required=True,
                    help='collect_latency.sh 的工作目录')
    ap.add_argument('--csv', help='逐样本输出（source,kind,latency_us）')
    args = ap.parse_args()

    loads = sorted(glob.glob(os.path.join(args.work, 'prism_load_*.txt')))
    if not loads:
        print("!! work 目录无 prism_load_*.txt —— 采集失败", file=sys.stderr)
        return 1

    print("=== T11 延迟基线聚合 ===")
    print(f"loads: {len(loads)}  work: {args.work}")
    p1_all, p2_all, unp_all, orph_all = [], [], 0, 0
    slow_rows = []  # (lat_us, load, pos, opcode)
    type_counts = defaultdict(int)
    per_load = []   # (文件名, completes, p2 样本) — CSV 段复用
    for path in loads:
        issues, completes, eqs = parse_prism_file(path)
        p2, unp, orph = pair_p2(completes, eqs)
        n_cmd_eq = sum(1 for _, v, _, _ in eqs if v)
        for _, _, _, t in eqs:
            type_counts[t] += 1
        print(f"{os.path.basename(path)}: issue={len(issues)} "
              f"complete={len(completes)} eq_event={len(eqs)} "
              f"eqe(cmd)={n_cmd_eq} p2={len(p2)} unpaired={unp}")
        # 慢样本 opcode 归属：bootstrap/selftest 单线程串行（issue→轮询
        # own slot→complete→下一条），complete 序 == issue 序，可按位取 opcode。
        for pos, (_, _, lat) in enumerate(completes):
            if lat > 1_000_000:
                op = issues[pos] if pos < len(issues) else "?"
                slow_rows.append((lat / 1000, os.path.basename(path), pos, op))
        p1_all += [lat for _, _, lat in completes]
        p2_all += p2
        unp_all += unp
        orph_all += orph
        per_load.append((os.path.basename(path), completes, p2))

    print()
    print("P1' 命令往返（doorbell→owner 翻转观察，轮询口径，"
          "DESIGN §10.1 参考值 8-15us 为事件模式口径，仅数量级对比）")
    print(dist_line("  P1'", dist(p1_all)))
    if slow_rows:
        print("  慢样本归属（>1ms；complete 序==issue 序假设，逐加载确定性核对）:")
        for lat_us, src, pos, op in sorted(slow_rows, reverse=True)[:10]:
            print(f"    {lat_us:12,.1f} us  {src} pos={pos} {op}")
    print()
    print("P2' 事件提交（eq_event − 配对 cmdq_complete；流式配对，"
          f"未配对 complete {unp_all} 条为尾部正常丢弃，孤儿位 {orph_all} 个；"
          f"EQE type 分布 {dict(type_counts)}）")
    print(dist_line("  P2'", dist(p2_all)))
    print()
    print(verdict_c(p2_all))
    print()

    core = os.path.join(args.work, 'mlx5_core.txt')
    if os.path.exists(core):
        core_s = pair_kprobes(core)
        print("mlx5_core 基准（kretprobe 口径 = mlx5_cmd_invoke 函数驻留时间，"
              "同步调用者含固件往返；与 P1' 相近但不严格等价）")
        print(dist_line("  core", dist(core_s)))
    else:
        print("mlx5_core 基准：kprobe 采集不可用，回退参考值 "
              "~8-15us（DESIGN §10.1 事件模式口径）——如实标注口径差异")

    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['source', 'kind', 'latency_us'])
            for src, completes, p2 in per_load:
                for _, _, lat in completes:
                    w.writerow([src.replace('.txt', ''), 'P1',
                                round(lat / 1000, 3)])
                for s in p2:
                    w.writerow([src.replace('.txt', ''), 'P2',
                                round(s / 1000, 3)])
            if os.path.exists(core):
                for s in pair_kprobes(core):
                    w.writerow(['mlx5_core', 'kprobe', round(s / 1000, 3)])
        print(f"\ncsv: {args.csv}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
