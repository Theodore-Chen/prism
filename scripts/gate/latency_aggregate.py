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
  bp/all 双档（2026-10-01 T16 trip-1c）：T13 起 PHY monitor 周期
      ACCESS_REG 与口间探针重叠，窗口法无法剔除——样本按 opcode 打标，
      bp = 非 ACCESS_REG（探针期判据对齐口径），all = 全量入档对照。
      判据 C 裁 bp 档（bootstrap 命令流的 EQE 投递 = T10-prep 原始
      范畴），阈值零改动。
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


def pair_p2_tagged(completes, eqs, opcodes=None, max_slots=64):
    """pair_p2 的带标核心：样本为 (delta_ns, opcode)。
    opcodes[i] 为第 i 条 complete 的命令名（complete 序==issue 序假设，
    与慢样本归属同源）；无 opcodes 时标签为 None。"""
    timeline = sorted([(ts, 0, slot, i) for i, (ts, slot, _) in enumerate(completes)] +
                      [(ts, 1, vector, -1) for ts, vector, _, _ in eqs])
    pending, samples, orphan = {}, [], 0
    for ts, kind, val, idx in timeline:
        if kind == 0:
            op = opcodes[idx] if opcodes and idx < len(opcodes) else None
            pending[val] = (ts, op)
            continue
        if not val:
            continue
        for b in range(min(max_slots, val.bit_length())):
            if val & (1 << b):
                if b in pending:
                    pts, op = pending.pop(b)
                    samples.append((ts - pts, op))
                else:
                    orphan += 1
    return samples, len(pending), orphan


def pair_p2(completes, eqs, max_slots=64):
    """流式配对：completes 与 eqs 按时间线合并，complete 置 pending[slot]，
    EQE 的 vector 位逐位认领**当时**的 pending 槽位 → ns 样本。
    （run-1 教训：预载全量 completes 的静态配对在槽位复用下把早 EQE 配给
    晚 complete，产出大负值样本——EQ_DEFERRED 语义下槽位在被 EQE 确认前
    不释放，同一时刻每槽至多一个未决 complete，流式认领无歧义。）
    返回 (samples, unpaired_completes, orphan_bits)。"""
    tagged, unpaired, orphan = pair_p2_tagged(completes, eqs, None, max_slots)
    return [d for d, _ in tagged], unpaired, orphan


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


# P2'ev 排除集（2026-10-01 T16 trip-2 尸检后口径）：这些命令没有自身
# CMD EQE，混入配对只会制造伪影——
#   ACCESS_REG          T13 monitor 周期读（自发流量）+ 探针期 PPCNT/PAOS/MTMP
#   QUERY_PAGES/MANAGE_PAGES  page_queue，走 pages 向量（cmdq.c force_poll 判定）
#   CREATE_EQ/DESTROY_EQ/QUERY_EQ  force_poll：EQ 生命周期命令 EQE 时序未定义
#   ?                   自检故意错误命令（0xdead 等）
P2EV_EXCL = {"ACCESS_REG", "QUERY_PAGES", "MANAGE_PAGES",
             "CREATE_EQ", "DESTROY_EQ", "QUERY_EQ", "?"}


def first_kworker_ts(path):
    """窗口右界：第一条 kworker 上下文 trace 行的时间戳（ns）。
    双口探针串行、口 0 monitor 首 tick 在其 monitor-up+100ms（已排队
    定时器不可取消，静音后仍必发）——在此之前的事件流是口 0 独占的
    单队列串行流（口 1 尚未 probe），P2'ev 在此窗口内反向配对无歧义。
    无 kworker 行（理论不发生）返回 None → 全窗。"""
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if m and m.group("comm").strip().startswith("kworker"):
                return int(round(float(m.group("ts")) * 1e9))
    return None


def pair_p2_ev(completes, eqs, opcodes, cut_ns=None, max_slots=64):
    """P2'ev 反向配对（事件形态，T16 trip-2 口径）：complete 认领**早于
    自身**且未被认领的最新同槽位 EQE → delta = complete.ts − eq.ts =
    唤醒延迟（EQE 消费 → 等待者观察），恒正、量级 µs。
    事件模式下命令次序为 issue → EQE（硬中断）→ 唤醒 → complete，旧
    正向配对把每条 EQE 配给上一条命令的 pending 槽（T16 trip-1 三连红
    根因，全样本正号 + 61.7ms 双口混流伪影，尸检在 progress.md T16 节）。
    仅窗口内（cut_ns，None=全窗）且排除集外的 complete 参与。
    返回 (samples[(delta, opcode)], unpaired_count)。"""
    comp = []
    for i, (ts, slot, _) in enumerate(completes):
        op = opcodes[i] if i < len(opcodes) else "?"
        if op in P2EV_EXCL:
            continue
        if cut_ns is not None and ts > cut_ns:
            continue
        comp.append((ts, slot, op))
    # 时间线归并；每槽栈在任意时刻只含 ≤ 当前时刻的未认领 EQE，
    # 栈顶即"最新的早先未认领者"——complete 认领即弹栈（同戳 complete
    # 先于 EQE 排序，与正向配对同约定）
    timeline = sorted([(ts, 0, slot, op) for ts, slot, op in comp] +
                      [(ts, 1, b, None)
                       for ts, v, _, _ in eqs if v
                       and (cut_ns is None or ts <= cut_ns)
                       for b in range(min(max_slots, v.bit_length()))
                       if v & (1 << b)])
    stack, samples, unpaired = defaultdict(list), [], 0
    for ts, kind, slot, op in timeline:
        if kind == 1:
            stack[slot].append(ts)
        elif stack[slot]:
            samples.append((ts - stack[slot].pop(), op))
        else:
            unpaired += 1
    return samples, unpaired


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
    p1_all, p1_bp = [], []          # bp = probe 期非 ACCESS_REG（判据/T11 对齐口径）
    p2_all, p2_ev = [], []          # all = 旧正向配对（形态记录）；ev = 事件唤醒延迟（判据档）
    ev_unp_all = 0
    unp_all, orph_all = 0, 0
    slow_rows = []  # (lat_us, load, pos, opcode)
    type_counts = defaultdict(int)
    per_load = []   # (文件名, completes, p2) — CSV 段复用
    for path in loads:
        issues, completes, eqs = parse_prism_file(path)
        p2t, unp, orph = pair_p2_tagged(completes, eqs, issues)
        cut = first_kworker_ts(path)
        p2ev, ev_unp = pair_p2_ev(completes, eqs, issues, cut)
        n_cmd_eq = sum(1 for _, v, _, _ in eqs if v)
        for _, _, _, t in eqs:
            type_counts[t] += 1
        print(f"{os.path.basename(path)}: issue={len(issues)} "
              f"complete={len(completes)} eq_event={len(eqs)} "
              f"eqe(cmd)={n_cmd_eq} p2(all)={len(p2t)} p2ev={len(p2ev)} "
              f"ev_unpaired={ev_unp} unpaired={unp} "
              f"cut={'%.3f' % (cut / 1e9) if cut else 'none'}")
        # 慢样本 opcode 归属：bootstrap/selftest 单线程串行（issue→轮询
        # own slot→complete→下一条），complete 序 == issue 序，可按位取 opcode。
        for pos, (_, _, lat) in enumerate(completes):
            op = issues[pos] if pos < len(issues) else "?"
            if lat > 1_000_000:
                slow_rows.append((lat / 1000, os.path.basename(path), pos, op))
            p1_all.append(lat)
            if op != "ACCESS_REG":
                p1_bp.append(lat)
        p2_all += [d for d, _ in p2t]
        p2_ev += [d for d, _ in p2ev]
        ev_unp_all += ev_unp
        unp_all += unp
        orph_all += orph
        per_load.append((os.path.basename(path), completes, [d for d, _ in p2t]))

    print()
    print("P1' 命令往返（doorbell→owner 翻转观察/事件口径。bp = 探针期非"
          "ACCESS_REG——T11 判据对齐口径；T13 起 PHY monitor 周期 ACCESS_REG "
          "与口间探针重叠不可按窗口剔除，一律入 all 档对照）")
    print(dist_line("  P1'bp", dist(p1_bp)))
    print(dist_line("  P1'all", dist(p1_all)))
    if slow_rows:
        print("  慢样本归属（>1ms；complete 序==issue 序假设，逐加载确定性核对）:")
        for lat_us, src, pos, op in sorted(slow_rows, reverse=True)[:10]:
            print(f"    {lat_us:12,.1f} us  {src} pos={pos} {op}")
    print()
    print("P2'ev 事件唤醒延迟（complete − 自身 EQE，反向配对；窗口 = 首 "
          "kworker 行之前 = 口 0 单队列串行段；排除集 = 无自身 CMD EQE 的 "
          "命令[ACCESS_REG/page_queue/EQ 生命周期/自检错误]）——事件形态 "
          "（T10 后）判据档，2026-10-01 T16 trip-2 口径")
    print(dist_line("  P2'ev", dist(p2_ev)))
    if ev_unp_all:
        print(f"  （ev 未配对 {ev_unp_all} 条：排除集外 complete 无早先 "
              f"未认领同槽 EQE——多为窗口截断边界，入档不删）")
    print()
    print("P2'all 旧口径正向配对（轮询时代语义：EQE 积压后消费；事件形态下 "
          "方向反转 + 双口混流产生系统性伪影——T16 尸检结论，仅形态记录，"
          f"不作判据。未配对 {unp_all}，孤儿位 {orph_all}，EQE type 分布 "
          f"{dict(type_counts)}）")
    print(dist_line("  P2'all", dist(p2_all)))
    print()
    print("判据C（预登记，裁 P2'ev 档——事件形态下逐命令唤醒延迟，"
          "阈值沿用 T10-prep 原值零改动；2026-10-01 T16 trip-2 口径切换，"
          "依据 = progress.md T16 节尸检）：")
    print(verdict_c(p2_ev))
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
