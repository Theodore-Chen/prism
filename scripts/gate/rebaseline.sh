#!/bin/bash
# rebaseline.sh — 基线重采一条龙（有意行为变更后的标准流程；先例：
# T10-prep 编码转正、T10 事件模式转正）。
#
# 全部输出（含子脚本与门禁）落 /var/tmp/rebaseline_run.log，跑完把这一个
# 文件交给分析即可，无需拷贝终端。
#
# 流程（自动熔断，绝不盲目覆盖基线）：
#   1. t09_verify.sh 3     — 3 个新鲜 selftest=1 周期（口径与基线/门禁一致；
#                            黑匣子 /var/tmp/t09_live.log）
#   2. diff_baseline.sh      — 对比旧基线【预期 FAIL，差异应仅为已知变更】
#      差异行逐条过预期族白名单：QUERY_EQ / EQ ready / EQ up (event mode) /
#      eqe[N] / eq self-test 10/10 / DESTROY_EQ ok——任何族外行 = 行为漂移，
#      熔断退出，基线不动
#   3. 族内差异确认后按 README 流程重采双口基线 + chown 回调用用户
#   4. 重采后同周期自洽复验（必须 zero diff）
#   5. r0_gate.sh          — 新基线下 G0-G4 全门禁（~10 min）
#
# Usage: sudo bash scripts/gate/rebaseline.sh        (~15 min)

set -u
cd /home/zqc/code/prism

RUN=/var/tmp/rebaseline_run.log
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

# 私有工作目录（diff 产物落这里，白名单熔断从这里读）：/tmp 固定文件名
# 在 fs.protected_regular=2 下跨用户复用会 EACCES（2026-09-17 事故，
# progress.md 归档⑦）。
WORK=$(mktemp -d /tmp/prism_rebaseline.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
export PRISM_BASELINE_WORK="$WORK"

mv -f "$RUN" "$RUN.prev" 2>/dev/null
exec > >(tee "$RUN") 2>&1

say() { echo; echo "===== $* ====="; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; else echo "   [FAIL] $2"; fi; }

say "rebaseline start: $(date '+%F %T') HEAD=$(git rev-parse --short HEAD 2>/dev/null || echo '?')"

say "step 1/5: 3 fresh cycles (t09_verify.sh 3)"
bash scripts/gate/t09_verify.sh 3
res $? "cycles completed (WARNING/Oops 内建检查)"

say "step 2/5: diff vs OLD baseline (expected FAIL, changed lines must be known drift)"
bash scripts/baseline/diff_baseline.sh
DG=$?

UNEXPECTED=0
for PORT in 0 1; do
    D="$WORK/prism_diff_0000:08:00.$PORT.txt"
    [ -s "$D" ] || { echo "   [skip] $D empty"; continue; }
    BAD=$(grep '^[+-]' "$D" | grep -v -e '^+++' -e '^---' \
        | grep -v -e 'QUERY_EQ: fw reports' \
                  -e 'EQ ready:' \
                  -e 'EQ up (event mode):' \
                  -e 'eqe\[[0-9]*\]:' \
                  -e 'eq self-test: 10/10' \
                  -e 'DESTROY_EQ ok:')
    if [ -n "$BAD" ]; then
        echo "   [FAIL] $D 出现族外漂移行（基线不动，停止）:"
        echo "$BAD"
        UNEXPECTED=1
    else
        echo "   [PASS] $D 全部差异行在预期族内"
    fi
done
if [ "$UNEXPECTED" -ne 0 ]; then
    say "ABORT: unexpected drift — 基线未动；把 $RUN 交给分析"
    exit 1
fi

say "step 3/5: regenerate baseline (README procedure)"
LIVE=/var/tmp/t09_live.log
LAST=$(grep -n '0000:08:00.0: BAR0 mapped' "$LIVE" | tail -1 | cut -d: -f1)
[ -n "$LAST" ] || { echo "no load cycle found in $LIVE"; exit 1; }
for P in 0 1; do
    tail -n +"$LAST" "$LIVE" | bash scripts/baseline/normalize.sh \
        | grep " 0000:08:00.$P: " | sed "s/mlx5_prism 0000:08:00.$P: //" \
        > scripts/baseline/baseline_dmesg_$P.txt
    [ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" \
        scripts/baseline/baseline_dmesg_$P.txt
    echo "   baseline_dmesg_$P.txt: $(wc -l < scripts/baseline/baseline_dmesg_$P.txt) lines"
done

say "step 4/5: same-cycle self-consistency (must be zero diff)"
bash scripts/baseline/diff_baseline.sh
res $? "zero diff vs new baseline"

say "step 5/5: full gate G0-G4 under the new baseline (~10 min)"
bash scripts/gate/r0_gate.sh
GATE=$?

say "verdict"
echo "   diff(old) rc=$DG  unexpected=0  gate rc=$GATE"
if [ "$GATE" -eq 0 ]; then
    echo "   ALL GREEN — 基线重采完成，R0+T10-prep 闭环"
else
    echo "   NOT GREEN — 基线已重采；定位见上方门禁段输出"
fi
echo "log: $RUN（上一份在 $RUN.prev）"
exit 0
