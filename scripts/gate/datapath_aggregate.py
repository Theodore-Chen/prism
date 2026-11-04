#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""T18 臂4 聚合器 — prism_sq_xmit → prism_rx_cqe 端到端时延配对（也可独立运行）.

输入：collect_datapath.sh 阶段 A 产的 trace 快照（--file）或快照目录
（--work，处理全部 lat_*.txt）。

口径（与 task.json T18 / DESIGN §6.4 对齐）：
  事件  prism_sq_xmit(len ihs form pc)  触发于 doorbell 后（TX 口，net.c:1008）
        prism_rx_cqe(byte_cnt wqe_id)    触发于 CQE 消费（RX 口，net.c:788）
  配对  两 tracepoint 无全局序号；固定包长档下 byte_cnt==len（64/1514 均
        ≥60，不受固件 42→60 补齐怪癖影响）→ rx 认领同长度最早未认领 tx
        （FIFO）。trace_clock=global（T11 惯例），单流低速率（≤1000pps，
        间距 ≥1ms）下无歧义。
  窗口  超过 1s 未被认领的 pending tx 判 stale 丢弃（计 unpaired_tx）；
        rx 无同长度 pending 计 unpaired_rx；负样本（时钟粒度边界）单列
        不入分布。
  分布  nearest-rank 分位（latency_aggregate.py 同款）。
  10ms 地板可见性  DESIGN §6.4：空闲 CQ 到达从不产生事件 → 低负载时延
        由 netdev_arm_ms=10 看门狗界定 → 样本应呈 0-10ms 均匀形态；
        share>=8ms 与 p99/max 一并输出，供 D3 注记（数值入档不设阈值）。

判据预登记：本聚合器只产分布与形态注记，不做 PASS/FAIL 阈值裁决
（T20 才按实测分布立门禁）。
"""
import argparse
import csv
import glob
import math
import os
import re
import sys
from collections import defaultdict, deque

# tracefs 行前缀: comm-pid [cpu] 上下文标志 ts: ...（latency_aggregate.py 同款）
LINE = re.compile(r'^(?P<comm>.+)-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]'
                  r'\s+\S+\s+(?P<ts>\d+\.\d+):\s+(?P<rest>\S+: .*)$')
SQXMIT = re.compile(r'^prism_sq_xmit: len=(?P<len>\d+) ihs=\d+ '
                    r'form=\S+ pc=\d+')
RXCQE = re.compile(r'^prism_rx_cqe: byte_cnt=(?P<bc>\d+) wqe_id=\d+')

STALE_NS = 1_000_000_000  # 1s 未认领判 stale
FLOOR_NS = 8_000_000      # 地板可见性观察窗（≥8ms 样本占比）


def parse_datapath_file(path):
    """返回 (txs, rxs)；txs=[(ts_ns,len)] rxs=[(ts_ns,byte_cnt)]，按行序。"""
    txs, rxs = [], []
    with open(path) as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            ts_ns = int(round(float(m.group('ts')) * 1e9))
            rest = m.group('rest')
            if (t := SQXMIT.match(rest)):
                txs.append((ts_ns, int(t.group('len'))))
            elif (r := RXCQE.match(rest)):
                rxs.append((ts_ns, int(r.group('bc'))))
    return txs, rxs


def pair_datapath(txs, rxs, stale_ns=STALE_NS):
    """FIFO 按 len 配对 → (samples_ns, unpaired_tx, unpaired_rx, negative)。

    事件流时间序合并：tx 入 pending[len] 队尾；rx 认领队首（最早未认领）。
    队首过老（stale）先丢弃再认领——避免长残留吞掉新样本。"""
    pending = defaultdict(deque)
    samples, un_tx, un_rx, neg = [], 0, 0, 0
    timeline = sorted([(ts, 0, v) for ts, v in txs] +
                      [(ts, 1, v) for ts, v in rxs])
    for ts, kind, val in timeline:
        q = pending[val]
        if kind == 0:
            q.append(ts)
            continue
        while q and ts - q[0] > stale_ns:
            q.popleft()
            un_tx += 1
        if not q:
            un_rx += 1
            continue
        d = ts - q.popleft()
        if d < 0:  # global 时钟粒度边界，单列不入分布
            neg += 1
        else:
            samples.append(d)
    un_tx += sum(len(q) for q in pending.values())
    return samples, un_tx, un_rx, neg


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


def aggregate_file(path):
    """单快照聚合：返回 (samples_ns, 摘要行)。"""
    txs, rxs = parse_datapath_file(path)
    samples, un_tx, un_rx, neg = pair_datapath(txs, rxs)
    d = dist(samples)
    floor_share = (sum(1 for x in samples if x >= FLOOR_NS) / len(samples)
                   if samples else 0.0)
    name = os.path.basename(path)
    summary = [f"{name}: tx={len(txs)} rx={len(rxs)} paired={len(samples)} "
               f"unpaired_tx={un_tx} unpaired_rx={un_rx} negative={neg}"]
    summary.append("  " + dist_line("dist", d))
    summary.append(f"  10ms 地板可见性：≥8ms 样本占比 {floor_share:.1%}"
                   f"（看门狗形态注记，数值入档不设阈值）")
    if d is not None and un_rx + un_tx > len(samples):
        summary.append("  !! 配对缺口过半——快照在案，走尸检不定罪")
    return samples, "\n".join(summary)


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--file', help='单个 trace 快照')
    g.add_argument('--work', help='快照目录（处理全部 lat_*.txt）')
    g.add_argument('--selftest', action='store_true',
                   help='合成 trace 自测（免 root/硬件）')
    ap.add_argument('--csv', help='逐样本输出（snapshot,latency_us）')
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    paths = ([args.file] if args.file
             else sorted(glob.glob(os.path.join(args.work, 'lat_*.txt'))))
    if not paths:
        print("!! 无输入快照", file=sys.stderr)
        return 1

    print("=== T18 臂4 端到端时延聚合 ===")
    rows = []
    for p in paths:
        samples, summary = aggregate_file(p)
        print(summary)
        rows.extend((os.path.basename(p), s / 1000) for s in samples)
    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['snapshot', 'latency_us'])
            w.writerows(rows)
        print(f"csv: {args.csv}（{len(rows)} 行）")
    return 0


# ---- 合成 trace 自测（bash 侧零依赖，跑在宿主机无 root） ----

def synth(pairs, base=1000.0):
    """pairs=[(tx_off_us, rx_off_us, plen)] → 合成 trace 行文本。"""
    lines = []
    for txo, rxo, plen in pairs:
        if txo is not None:
            lines.append(f"kpktgend_0-123 [4] ..... {base + txo / 1e6:.6f}: "
                         f"prism_sq_xmit: len={plen} ihs=0 "
                         f"form=hdr-inline+ptr pc=5")
        if rxo is not None:
            lines.append(f"prism_napi-456 [5] ..... {base + rxo / 1e6:.6f}: "
                         f"prism_rx_cqe: byte_cnt={plen} wqe_id=3")
    lines.sort(key=lambda l: float(l.split(': ')[0].split()[-1]))
    return "\n".join(lines) + "\n"


def selftest():
    import tempfile
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f"  [{ 'PASS' if cond else 'FAIL' }] {name}")
        ok = ok and cond

    print("== datapath_aggregate selftest ==")

    # 1) 常规逐包配对：delta 100/200/300µs
    t = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
    t.write(synth([(0, 100, 64), (1000, 1200, 64), (2000, 2300, 64)]))
    t.close()
    s, un_tx, un_rx, neg = pair_datapath(*parse_datapath_file(t.name))
    check("逐包配对 n=3 且 delta 为 100/200/300µs",
          s == [100_000, 200_000, 300_000] and un_tx == un_rx == neg == 0)

    # 2) 看门狗批形态：3 个 tx 批量被一次 poll 消费（FIFO 保序）
    t2 = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
    t2.write(synth([(0, 5000, 1514), (1000, 5000, 1514), (2000, 5000, 1514)]))
    t2.close()
    s2, *_ = pair_datapath(*parse_datapath_file(t2.name))
    check("同刻批量 rx FIFO 认领（delta 5ms/4ms/3ms）",
          s2 == [5_000_000, 4_000_000, 3_000_000])

    # 3) 乱序长度混流：len 隔离，互不串门（三对全配：64×2 + 1514×1）
    t3 = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
    t3.write(synth([(0, 50, 64), (10, 5000, 1514), (1000, 1050, 64)]))
    t3.close()
    s3, un3_tx, un3_rx, _ = pair_datapath(*parse_datapath_file(t3.name))
    check("len 隔离（64 档 50µs×2、1514 档 4990µs，零漏配）",
          s3 == [50_000, 50_000, 4_990_000] and un3_tx + un3_rx == 0)

    # 4) 地板形态：tx 间距 10ms > 最大 delta，rx 单调无重排 → delta
    #    = (i%10)ms 均匀 500 样本，p99<10.5ms、≥8ms 占比≈20%
    t4 = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
    t4.write(synth([(i * 10_000, i * 10_000 + (i % 10) * 1000, 64)
                    for i in range(500)]))
    t4.close()
    s4, summary4 = aggregate_file(t4.name)
    d4 = dist(s4)
    share4 = sum(1 for x in s4 if x >= FLOOR_NS) / len(s4)
    check("地板形态 p99<10.5ms 且 ≥8ms 占比≈20%",
          d4['p99'] < 10_500_000 and abs(share4 - 0.2) < 0.03)

    for p in (t.name, t2.name, t3.name, t4.name):
        os.unlink(p)
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
