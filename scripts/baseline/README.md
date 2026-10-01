# scripts/baseline — R0 重构冲刺的行为保持基线

## 文件

- `baseline_dmesg_0.txt` / `baseline_dmesg_1.txt` — 双口各自的归一化基线模板
  （单口 68 行：probe 引导序列 + EQ 自检 + netdev 引导 + teardown 关键行）。
  **两口不可互换**：caps hexdump 在 function 标识字节处有真实差异
  （.0 口与 .1 口各差 1 字节），但各自跨压测周期完全确定。
  现行基线为 **T15 重采版（2026-10-01，r0_gate 8 passed 0 failed）**：
  T14 版 64 行 + netdev 四行族——`comp eq:`（专用完成 EQ 分配）、
  `comp EQ up:`（eqn 归一）、`netdev prismN registered (mac X)`、
  comp EQ 的 `DESTROY_EQ ok: eq_number=5` 拆除行。
- `normalize.sh` — 归一化逻辑（唯一存放点，基线生成与对比共用）。
- `diff_baseline.sh` — 对比门禁：取日志中最后一个完整周期（锚点 = 最后一次
  .0 口 BAR0 mapped），双口分别与基线 diff；临时文件走 mktemp 私有目录
  （`PRISM_BASELINE_WORK` 可注入——fs.protected_regular 教训，progress.md
  归档⑦）。

## 用法（每步 R0.x 重构后的验收关口）

```bash
sudo bash scripts/gate/t09_verify.sh 1                 # 跑一个新鲜周期
sudo bash scripts/baseline/diff_baseline.sh           # 对比（默认读 /var/tmp/t09_live.log）
# 或直接指定日志：bash scripts/baseline/diff_baseline.sh <dmesg-file>
```

双口均 `[PASS] zero diff` 才算通过。任何差异 = 行为漂移：停止、二分、上机重测。

## 归一化白名单（时延/地址允许差异，计数/状态/字节不允许）

- 时间戳、cpu/dma/doorbell 地址、mem 区间、毫秒数字 → 占位符
- **~10% 激活怪癖的合法变体**：静默→重建块（no EQE / 环 dump / silent at /
  DESTROY_EQ 0 events）删除，重建产生的第二个 QUERY_EQ 折叠
- 刻意不归一（回归锚点）：caps[max]/[cur]/[post] hexdump、boot=6/init=2299/
  teardown 2305/2305、log_sz=5/stride=6、eqn=4、10/10、eqe vector 位图
- （T12 起）`PHY PPCNT` 自检行：计数器数值跨周期单调递增（且跨驱动加载
  存活），全部归一为占位符；行结构（字段名/组号 0x12/0x00/0x16/探测结论
  措辞）保留为锚点

## 基线再生成（仅当"有意"改变行为并重验后）

```bash
# 从一份已知良好的 live.log 取最后完整周期：
LIVE=/var/tmp/t09_live.log
LAST=$(grep -n '0000:08:00.0: BAR0 mapped' $LIVE | tail -1 | cut -d: -f1)
for P in 0 1; do
    tail -n +$LAST $LIVE | bash scripts/baseline/normalize.sh \
        | grep " 0000:08:00.$P: " | sed "s/mlx5_prism 0000:08:00.$P: //" \
        > scripts/baseline/baseline_dmesg_$P.txt
done
```

## 门禁自检记录（R0.1，2026-09-15）

基线来源：T09 收官压测（HEAD 24c9d80 代码，100 循环，零 WARNING/Oops，
黑匣子 /var/tmp/t09_live.log 10609 行）。三重验证：
① 同周期自洽 PASS；② 中间周期（不同激活怪癖变体）PASS；
③ 负对照（篡改 init pages 2299→2298）精确 FAIL。
开发中还抓到双口 caps 字节差并据此改用分口基线。
