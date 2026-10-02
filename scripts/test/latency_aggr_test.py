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


def test_pair_p2_tagged_bp():
    """T16 trip-1c：配对样本带 opcode 标签，bp 档排除 ACCESS_REG
    （T13 monitor 周期读与探针期 ACCESS_REG 同 opcode，一并排除）。"""
    completes = [(1_000_000_000, 0, 100), (2_000_000_000, 1, 200),
                 (3_000_000_000, 2, 300)]
    eqs = [(1_500_000_000, 0x1, 4, 0x0a), (3_500_000_000, 0x6, 4, 0x0a)]
    ops = ["INIT_HCA", "ACCESS_REG", "QUERY_PAGES"]
    tagged, unpaired, orphan = la.pair_p2_tagged(completes, eqs, ops)
    # slot0↔EQE1（+0.5s，INIT_HCA）；slot1↔EQE2 bit1（+1.5s，ACCESS_REG）；
    # slot2↔EQE2 bit2（3.5s−3.0s=+0.5s，QUERY_PAGES）
    check("tagged 样本", sorted(tagged),
          sorted([(500_000_000, "INIT_HCA"), (1_500_000_000, "ACCESS_REG"),
                  (500_000_000, "QUERY_PAGES")]))
    check("tagged unpaired", unpaired, 0)
    bp = [d for d, op in tagged if op != "ACCESS_REG"]
    check("bp 档排除 ACCESS_REG", sorted(bp),
          sorted([500_000_000, 500_000_000]))
    # 无 opcodes → 标签 None（pair_p2 垫片路径）
    plain, _, _ = la.pair_p2_tagged(completes, eqs)
    check("无标签路径", sorted(d for d, _ in plain),
          sorted([500_000_000, 500_000_000, 1_500_000_000]))


def test_pair_p2_ev():
    """T16 trip-2：事件形态反向配对（EQE 消费→等待者观察的唤醒延迟）。
    事件模式命令次序 = issue → EQE → 唤醒 → complete，complete 认领早于
    自身的最新同槽未认领 EQE；排除集（无自身 CMD EQE 的命令）不参与。"""
    us = 1_000_000
    # 事件形态序列（单队列）：i1 e1 c1 i2 e2 c2 …
    completes = [(10 * us, 0, 0), (20 * us, 0, 0), (30 * us, 1, 0)]
    eqs = [(9 * us, 0x1, 4, 0x0a), (19 * us, 0x1, 4, 0x0a), (29 * us, 0x2, 4, 0x0a)]
    ops = ["INIT_HCA", "ALLOC_UAR", "SET_HCA_CAP2"]
    s, unp = la.pair_p2_ev(completes, eqs, ops)
    check("ev 各配自身 EQE", sorted(d for d, _ in s),
          sorted([1 * us, 1 * us, 1 * us]))
    check("ev 标签", sorted(op for _, op in s),
          sorted(["INIT_HCA", "ALLOC_UAR", "SET_HCA_CAP2"]))
    check("ev unpaired", unp, 0)
    # 排除集：ACCESS_REG/page_queue/EQ 生命周期/? 不参与配对
    completes2 = [(10 * us, 0, 0), (20 * us, 0, 0), (30 * us, 0, 0)]
    eqs2 = [(9 * us, 0x1, 4, 0x0a), (19 * us, 0x1, 4, 0x0a), (29 * us, 0x1, 4, 0x0a)]
    ops2 = ["ACCESS_REG", "QUERY_PAGES", "INIT_HCA"]
    s2, unp2 = la.pair_p2_ev(completes2, eqs2, ops2)
    check("ev 排除集只剩 INIT_HCA", [(d // us, op) for d, op in sorted(s2)],
          [(1, "INIT_HCA")])
    check("ev 排除后无 unpaired", unp2, 0)
    # 窗口截断：cut 之后的 complete 不配，cut 之后的 EQE 不可用
    ops3 = ["INIT_HCA", "ALLOC_UAR", "SET_ISSI"]  # 均在排除集外
    s3, unp3 = la.pair_p2_ev(completes2, eqs2, ops3, cut_ns=15 * us)
    check("ev 窗口截断", [(d // us, op) for d, op in sorted(s3)],
          [(1, "INIT_HCA")])  # ALLOC_UAR/SET_ISSI 在 cut 后被截除
    # 无自身 EQE 的命令会认领**上一条命令**晚到的 EQE（陈旧认领伪影，
    # 轮询时代形态——claim 到 9µs 假样本）：c@10 无早先 EQE → unpaired，
    # c@20 认领 e@11、c@30 认领 e@21
    eqs_after = [(11 * us, 0x1, 4, 0x0a), (21 * us, 0x1, 4, 0x0a),
                 (31 * us, 0x1, 4, 0x0a)]
    s4, unp4 = la.pair_p2_ev(completes2, eqs_after, ["INIT_HCA"] * 3)
    check("ev EQE 晚到：陈旧认领形态", (len(s4), unp4,
                                        sorted(d // us for d, _ in s4)),
          (2, 1, [9, 9]))
    # 全部 EQE 晚于全部 complete：全 unpaired（唤醒链不存在的诚实输出）
    eqs_late = [(40 * us, 0x1, 4, 0x0a), (41 * us, 0x1, 4, 0x0a)]
    s5, unp5 = la.pair_p2_ev(completes2, eqs_late, ["INIT_HCA"] * 3)
    check("ev EQE 全在后 → 全 unpaired", (len(s5), unp5), (0, 3))


def test_first_kworker_ts(tmp):
    path = os.path.join(tmp, "kw.txt")
    with open(path, "w") as f:
        f.write(line(1.0, "prism_cmdq_issue: opcode=0x0104[ENABLE_HCA] op_mod=0x0 slot=0 token=1"))
        f.write(line(2.0, "prism_cmdq_complete: opcode=0x0104[ENABLE_HCA] slot=0 delivery=0x0 status=0x0 syndrome=0x000000 lat=1 ns err=0"))
        # kworker 上下文（monitor 首 tick）——窗口右界
        f.write(line(3.0, "prism_cmdq_issue: opcode=0x0805[ACCESS_REG] op_mod=0x1 slot=0 token=2"))
    # line() 的 comm 固定为 bash——手写一条 kworker 行
    raw = open(path).read().splitlines()
    with open(path, "w") as f:
        f.write("\n".join(raw[:2]) + "\n")
        f.write("  kworker/u48:0-123 [010] d.... 3.000000: prism_cmdq_issue: opcode=0x0805[ACCESS_REG] op_mod=0x1 slot=0 token=2\n")
        f.write("\n".join(raw[2:]) + "\n")
    check("first_kworker_ts", la.first_kworker_ts(path), 3_000_000_000)
    none_path = os.path.join(tmp, "kw_none.txt")
    with open(none_path, "w") as f:
        f.write(line(1.0, "prism_cmdq_issue: opcode=0x0104[ENABLE_HCA] op_mod=0x0 slot=0 token=1"))
    check("无 kworker 返回 None", la.first_kworker_ts(none_path), None)


def test_main_smoke(tmp):
    """main() 全路径冒烟（run-2 教训：函数级全绿但 main 里 eqs 四元组
    解包漏改，只有跑过 main 才能抓到）。"""
    import subprocess
    import glob
    work = os.path.join(tmp, "work")
    os.makedirs(work)
    with open(os.path.join(work, "prism_load_01.txt"), "w") as f:
        f.write(complete(1.0, 0, 72000))
        f.write(eqev(1.5, 0x1))
    csvout = os.path.join(tmp, "out.csv")
    agg = os.path.join(os.path.dirname(__file__), "..", "gate",
                       "latency_aggregate.py")
    r = subprocess.run([sys.executable, agg, "--work", work,
                        "--csv", csvout],
                       capture_output=True, text=True)
    check("main() 退出码", r.returncode, 0)
    check("main() 无异常", "Traceback" in r.stderr, False)
    check("main() 报告含 P1'", "P1'" in r.stdout, True)
    check("main() 报告含判据C", "判据C" in r.stdout, True)
    rows = open(csvout).read().strip().splitlines()
    check("CSV 行数(头+P1+P2)", len(rows), 3)


def main():
    with tempfile.TemporaryDirectory(prefix="prism_lat_test.") as tmp:
        print("== parse")
        test_parse(tmp)
        print("== pair_p2")
        test_pair_p2()
        print("== pair_p2_streaming（run-1 回归）")
        test_pair_p2_streaming()
        print("== pair_p2_tagged bp 档（T16 trip-1c）")
        test_pair_p2_tagged_bp()
        print("== pair_p2_ev 事件唤醒反向配对（T16 trip-2）")
        test_pair_p2_ev()
        print("== first_kworker_ts 窗口右界（T16 trip-2）")
        test_first_kworker_ts(tmp)
        print("== main 冒烟（run-2 回归）")
        test_main_smoke(tmp)
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
