#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""latency_aggregate.py 的合成 trace 单测（宿主机，无 root/硬件）.

用手工构造、答案已知的 tracefs 文本喂给解析/配对/分位/裁决四层，
断言精确数值。跑法：python3 scripts/test/latency_aggr_test.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'gate'))
import latency_aggregate as la  # noqa: E402

FAIL = []


def check(name, got, want):
    if got != want:
        FAIL.append(f"{name}: got {got!r}, want {want!r}")
        print(f"  FAIL {name}: got {got!r}, want {want!r}")
    else:
        print(f"  ok   {name}")


def line(ts, body):
    return f"           bash-1234    [003] .....  {ts:.6f}: {body}\n"


COMPLETE = ("prism_cmdq_complete: opcode=0x{op:x}[{name}] slot={slot} "
            "delivery=0x0 status=0x0 syndrome=0x000000 lat={lat} ns err=0")


def complete(ts, slot, lat, op=0x103, name="QUERY_PAGES"):
    return line(ts, COMPLETE.format(op=op, name=name, slot=slot, lat=lat))


def eqev(ts, vector):
    return line(ts, f"prism_eq_event: eqn=4 type=0x0a vector=0x{vector:08x} cons=7")


def test_parse(tmp):
    path = os.path.join(tmp, "prism_load_01.txt")
    with open(path, "w") as f:
        f.write("# tracer: nop\n#\n# header lines ignored\n")
        f.write(line(1.0, "prism_cmdq_issue: opcode=0x0103[QUERY_PAGES] op_mod=0x1 slot=0 token=1"))
        f.write(complete(1.001, 0, 1500))
        f.write(line(1.002, "prism_cmdq_issue: opcode=0x0802[ALLOC_UAR] op_mod=0x0 slot=1 token=2 (page)"))
        # 未知 opcode 名是 "?"（names.h default），正则必须放过
        f.write(complete(1.003, 1, 2500, op=0x999, name="?"))
        f.write(eqev(1.5, 0x3))
        f.write(line(1.6, "prism_eq_event: eqn=4 type=0x0e vector=0x00000000 cons=8"))
    issues, completes, eqs = la.parse_prism_file(path)
    check("issue opcode 序列", issues, ["QUERY_PAGES", "ALLOC_UAR"])
    check("complete 数", len(completes), 2)
    check("complete[0]", completes[0], (1_001_000_000, 0, 1500))
    check("eq 数（含非 CMD）", len(eqs), 2)
    check("eq 解析(含 eqn/type)", eqs[0], (1_500_000_000, 0x3, 4, 0x0a))
    return path


def test_pair_p2():
    completes = [(1_000_000_000, 0, 1000), (2_000_000_000, 1, 2000),
                 (2_500_000_000, 2, 3000)]
    eqs = [(1_500_000_000, 0x1, 4, 0x0a), (3_500_000_000, 0x6, 4, 0x0a)]
    samples, unpaired, orphan = la.pair_p2(completes, eqs)
    # bit0@1.5s−1.0s=0.5s；bit1@3.5−2.0=1.5s；bit2@3.5−2.5=1.0s
    check("P2' 样本(ms)", sorted(s // 1_000_000 for s in samples), [500, 1000, 1500])
    check("未配对 complete", unpaired, 0)
    check("孤儿位", orphan, 0)
    # 位图引用无 pending 槽位 → 孤儿，不产出样本
    _, unpaired, orphan = la.pair_p2(completes, [(4_000_000_000, 0x8, 4, 0x0a)])
    check("孤儿位计数", orphan, 1)
    check("未配对=3", unpaired, 3)


def test_pair_p2_streaming():
    """run-1 事故回归：槽位复用 + 交替 EQE，静态预载配对把早 EQE 配给
    晚 complete 产出大负值；流式配对必须按当时 pending 归属。"""
    completes = [(1_000_000_000, 0, 100), (2_000_000_000, 0, 100),
                 (3_000_000_000, 0, 100)]
    eqs = [(1_500_000_000, 0x1, 4, 0x0a), (2_500_000_000, 0x1, 4, 0x0a),
           (3_500_000_000, 0x1, 4, 0x0a)]
    samples, unpaired, orphan = la.pair_p2(completes, eqs)
    check("槽位复用流式配对(ms)", sorted(s // 1_000_000 for s in samples),
          [500, 500, 500])
    check("复用后无未配对", unpaired, 0)
    check("复用后无孤儿", orphan, 0)
    # EQE 先于任何 complete（异常序）：孤儿而非负样本
    samples, _, orphan = la.pair_p2([], [(1_000_000_000, 0x2, 4, 0x0a)])
    check("EQE 先行→零样本", samples, [])
    check("EQE 先行→孤儿", orphan, 1)
    # 同 µs 时间戳：complete 排在 EQE 前（sort 键 kind），产出 0 而非负
    samples, _, _ = la.pair_p2([(1_000_000_000, 0, 1)],
                               [(1_000_000_000, 0x1, 4, 0x0a)])
    check("同戳 complete 优先", samples, [0])


def test_pair_kprobes(tmp):
    path = os.path.join(tmp, "mlx5_core.txt")
    with open(path, "w") as f:
        f.write(line(10.000000, "ml5i: (mlx5_cmd_invoke+0x0/0x50)"))
        f.write(line(10.005000, "ml5r: (mlx5_cmd_invoke+0x0/0x50 <- mlx5_cmd_exec+0x5/0x10)"))
        f.write(line(10.010000, "ml5i: (mlx5_cmd_invoke+0x0/0x50)"))
        # 同 pid 嵌套：LIFO 配对（内层 1ms，外层 10.030−10.010=20ms）
        f.write(line(10.020000, "ml5i: (mlx5_cmd_invoke+0x0/0x50)"))
        f.write(line(10.021000, "ml5r: (mlx5_cmd_invoke+0x0/0x50 <- mlx5_cmd_exec+0x5/0x10)"))
        f.write(line(10.030000, "ml5r: (mlx5_cmd_invoke+0x0/0x50 <- mlx5_cmd_exec+0x5/0x10)"))
    samples = la.pair_kprobes(path)
    check("kprobe 配对(ms)", sorted(s // 1_000_000 for s in samples), [1, 5, 20])


def test_dist():
    d = la.dist(list(range(1, 11)))  # 1..10
    check("n", d["n"], 10)
    check("min", d["min"], 1)
    check("avg", d["avg"], 5.5)
    check("p50 nearest-rank", d["p50"], 5)
    check("p90 nearest-rank", d["p90"], 9)
    check("p99 nearest-rank", d["p99"], 10)
    check("max", d["max"], 10)
    check("空样本", la.dist([]), None)


def test_verdict():
    ms = 1_000_000
    v = la.verdict_c([1 * ms] * 100)
    check("长尾消失分支", "长尾消失" in v, True)
    v = la.verdict_c([1 * ms] * 100 + [600 * ms])
    check("长尾仍在分支", "长尾仍在" in v, True)
    v = la.verdict_c([1 * ms] * 100 + [50 * ms] * 5)  # max=50ms, p99≈50ms
    check("中间形态分支", "中间形态" in v, True)
    # 高位样本须 ≥1% 才进 p99（nearest-rank）：99 低 + 2 高 → p99=99ms
    v = la.verdict_c([1 * ms] * 99 + [99 * ms] * 2)
    check("p99 超阈值→中间", "中间形态" in v, True)
    check("无样本不裁决", "不裁决" in la.verdict_c([]), True)


def main():
    with tempfile.TemporaryDirectory(prefix="prism_lat_test.") as tmp:
        print("== parse")
        test_parse(tmp)
        print("== pair_p2")
        test_pair_p2()
        print("== pair_p2_streaming（run-1 回归）")
        test_pair_p2_streaming()
        print("== pair_kprobes")
        test_pair_kprobes(tmp)
        print("== dist")
        test_dist()
        print("== verdict_c")
        test_verdict()
    if FAIL:
        print(f"\nFAIL: {len(FAIL)} 项")
        return 1
    print("\nALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
