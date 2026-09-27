#!/bin/bash
# [已归档 2026-09-26] T12 已完成（J1-J6 硬件全绿 + rebaseline 收口 8/8）。
# 判据针对 T12 转正窗口预登记，勿跑后改；PHY 自检行为已沉淀进基线
# （39 行/口）与 r0_gate，交叉对账口径见 DESIGN §8.1/§10.1 C4。
#
# t12_verify.sh — T12 PHY PPCNT 硬件验证 + ethtool 交叉对账（2026-09-26）。
#
# 判据预登记（跑之前固化，跑完只对号入座）：
#   J1  双口 ACCESS_REG(PPCNT, grp=0x12, local_port=1) status=0（insmod
#       成功且 dmesg 有 0x12 行即证明——读失败会 fail probe）；tslc 非零
#       非全F（非全F由驱动侧 image 门卫保证，此处核 tslc>0）
#   J2  双口 grp=0x00 行存在（tx_ok/rx_ok/sym_err_carrier 解析成功）
#   J3  ethtool 对账（prism 读取时间点晚于 mlx5_core 采集点）：
#         tx_packets_phy       ↔ PPCNT(0x00) tx_ok
#         rx_packets_phy       ↔ PPCNT(0x00) rx_ok
#         rx_symbol_err_phy    ↔ PPCNT(0x00) sym_err_carrier
#         link_down_events_phy ↔ PPCNT(0x12) link_down
#       分支一（预期）：V_prism >= V_mlx5 —— 计数器跨驱动加载单调存活
#       分支二（预案）：任一锚点 V_prism < V_mlx5 → 疑似驱动切换清零，
#         立即二次加载 prism，要求同驱动内 V2 >= V1（单调性），结论记档案
#   J4  双口 grp 0x16 探测行在案（三种结局之一：rejected / accepted but
#       all-zero / accepted live）——pre-PCAM 预期前两种，均不算失败
#   J5  行为保持：(a) 默认 probe（无 selftest）dmesg 零 PHY 行；
#       (b) t09_verify.sh 20 循环全绿；(c) diff_baseline 差异全部落在
#       'PHY PPCNT(' 行族内（新自检行），族外漂移 = FAIL
#   J6  非全 0/全 F 的裁决：全 F 已被驱动门卫排除；链路 up 且 prism 误差/
#       FEC 计数全零时，核对 ethtool --show-fec —— 同零 → PASS-with-note
#       （链路极干净的真零）；ethtool 非零而 prism 全零 → FAIL；
#       链路 down → 以 J1 tslc>0 + ethtool 同名零值对账为准
#
# 输出（含 t09 二十循环日志尾与基线 diff）全部落 /var/tmp/t12_run.log
# 单文件；黑匣子：/var/tmp/t12_live.log（dmesg 逐行 sync）。
#
# Usage: sudo bash scripts/gate/t12_verify.sh     (~8 min)

set -u
cd /home/zqc/code/prism

RUN=/var/tmp/t12_run.log
LIVE=/var/tmp/t12_live.log
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

WORK=$(mktemp -d /tmp/prism_t12.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
export PRISM_BASELINE_WORK="$WORK"

mv -f "$RUN" "$RUN.prev" 2>/dev/null
exec > >(tee "$RUN") 2>&1

mv -f "$LIVE" "$LIVE.prev" 2>/dev/null
stdbuf -oL dmesg -w | while IFS= read -r line; do
        printf '%s\n' "$line" >> "$LIVE"
        sync
done &
LIVE_PID=$!

say() { echo; echo "===== $* ====="; }
res() { if [ "$1" -eq 0 ]; then echo "   [PASS] $2"; else echo "   [FAIL] $2"; RC=1; fi; }
RC=0

BDFS="0000:08:00.0 0000:08:00.1"
command -v ethtool >/dev/null || { echo "ethtool missing — cross-check impossible"; exit 1; }

bdf2net() {
        local bdf=$1 n
        for n in /sys/class/net/*; do
                [ "$(basename "$(readlink -f "$n/device")")" = "$bdf" ] && {
                        basename "$n"; return 0; }
        done
        return 1
}

et_field() { awk -v f="$1" '$1 == f":" {print $2}' "$2"; }
pr_field() { sed -n "s/.*$2=\([0-9]\+\).*/\1/p" "$1" | head -1; }
pr_field0() { local v; v=$(pr_field "$@"); echo "${v:-0}"; }

# sum the decimal numbers inside an "fld=[a b c d rs e]" bracket
pr_bracket_sum() { sed -n "s/.*$2=\[\([0-9 ]*rs [0-9]*\)\].*/\1/p" "$1" \
        | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+$/) s += $i} END {print s + 0}'; }

# ---------------------------------------------------------------- Phase A
say "Phase A: mlx5_core ethtool snapshot"
modprobe -r mlx5_prism 2>/dev/null
modprobe -r mlx5_ib 2>/dev/null
modprobe mlx5_core || { echo "mlx5_core failed to load"; exit 1; }
sleep 5
for BDF in $BDFS; do
        P=${BDF##*.}
        NET=$(bdf2net "$BDF") || { echo "   no netdev for $BDF"; RC=1; continue; }
        ethtool "$NET" | grep -E "Link detected|Speed:" > "$WORK/link_$P.txt"
        # two samples 2 s apart, keep the later (stats work refreshes ~1 s)
        ethtool -S "$NET" | grep "_phy" > "$WORK/etht_$P.txt"
        sleep 2
        ethtool -S "$NET" | grep "_phy" > "$WORK/etht_$P.txt"
        ethtool --show-fec "$NET" > "$WORK/fec_$P.txt" 2>&1
        echo "  $BDF -> $NET (link: $(awk '/Link detected/ {print $3}' "$WORK/link_$P.txt"))"
        grep -E "tx_packets_phy|rx_packets_phy|rx_symbol_err_phy|link_down_events_phy" \
                "$WORK/etht_$P.txt" | sed 's/^/    /'
done
modprobe -r mlx5_ib && modprobe -r mlx5_core && echo "mlx5_core unloaded"

# ---------------------------------------------------------------- Phase B
say "Phase B: prism selftest=1 PPCNT readback"
dmesg -c > /dev/null
insmod src/mlx5_prism.ko selftest=1 && echo "=== INSMOD OK ==="
sleep 1
dmesg | grep mlx5_prism > "$WORK/prism_dmesg.txt"
rmmod mlx5_prism && echo "=== RMMOD OK ==="
for BDF in $BDFS; do
        grep "$BDF: PHY PPCNT" "$WORK/prism_dmesg.txt" | sed 's/mlx5_prism //;s/^/  /'
done

# ---------------------------------------------------------------- Phase C
say "Phase C: J1/J2/J4 (per port)"
for BDF in $BDFS; do
        P=${BDF##*.}
        grep "$BDF: PHY PPCNT(0x12)" "$WORK/prism_dmesg.txt" > "$WORK/p12_$P.txt"
        grep "$BDF: PHY PPCNT(0x00)" "$WORK/prism_dmesg.txt" > "$WORK/p00_$P.txt"
        grep "$BDF: PHY PPCNT(0x16)" "$WORK/prism_dmesg.txt" > "$WORK/p16_$P.txt"

        TSLC=$(pr_field "$WORK/p12_$P.txt" tslc)
        [ -s "$WORK/p12_$P.txt" ] && [ -n "$TSLC" ] && [ "$TSLC" -gt 0 ]
        res $? "J1 $BDF: grp 0x12 ok, tslc=$TSLC"

        [ -s "$WORK/p00_$P.txt" ]
        res $? "J2 $BDF: grp 0x00 ok (tx=$(pr_field "$WORK/p00_$P.txt" tx_ok) rx=$(pr_field "$WORK/p00_$P.txt" rx_ok) sym_err=$(pr_field "$WORK/p00_$P.txt" sym_err_carrier))"

        [ -s "$WORK/p16_$P.txt" ]
        res $? "J4 $BDF: 0x16 probe on record"
        sed 's/^/       /' "$WORK/p16_$P.txt"
done

say "Phase C: J3 ethtool cross-accounting"
J3_BRANCH=1
check_anchor() { # et_name prism_file prism_field port
        local p m
        p=$(pr_field "$2" "$3")
        m=$(et_field "$1" "$WORK/etht_$4.txt")
        if [ -z "$p" ] || [ -z "$m" ]; then
                echo "   [FAIL] $1: missing side (prism='$p' ethtool='$m')"
                RC=1
                return
        fi
        if [ "$p" -lt "$m" ]; then
                echo "   [note] $1: prism=$p < ethtool=$m — branch-2 candidate"
                J3_BRANCH=2
        else
                echo "   [PASS] $1: prism=$p >= ethtool=$m"
        fi
}
for BDF in $BDFS; do
        P=${BDF##*.}
        echo "  -- $BDF"
        check_anchor tx_packets_phy "$WORK/p00_$P.txt" tx_ok "$P"
        check_anchor rx_packets_phy "$WORK/p00_$P.txt" rx_ok "$P"
        check_anchor rx_symbol_err_phy "$WORK/p00_$P.txt" sym_err_carrier "$P"
        check_anchor link_down_events_phy "$WORK/p12_$P.txt" link_down "$P"
done
if [ "$J3_BRANCH" -eq 1 ]; then
        echo "   [PASS] J3 branch 1: counters survive the driver switch (every anchor V_prism >= V_mlx5)"
else
        say "J3 branch 2: suspected counter reset on driver switch — re-sample prism"
        dmesg -c > /dev/null
        insmod src/mlx5_prism.ko selftest=1
        sleep 1
        dmesg | grep mlx5_prism > "$WORK/prism_dmesg2.txt"
        rmmod mlx5_prism
        MONO=0
        for BDF in $BDFS; do
                P=${BDF##*.}
                grep "$BDF: PHY PPCNT(0x00)" "$WORK/prism_dmesg2.txt" > "$WORK/p00b_$P.txt"
                grep "$BDF: PHY PPCNT(0x12)" "$WORK/prism_dmesg2.txt" > "$WORK/p12b_$P.txt"
                for pair in "tx_ok:p00" "rx_ok:p00" "link_down:p12"; do
                        fld=${pair%%:*}; base=${pair##*:}
                        a=$(pr_field "$WORK/${base}_$P.txt" "$fld")
                        b=$(pr_field "$WORK/${base}b_$P.txt" "$fld")
                        if [ -n "$a" ] && [ -n "$b" ] && [ "$b" -ge "$a" ]; then
                                echo "   [PASS] $BDF $fld: sample2=$b >= sample1=$a"
                        else
                                echo "   [FAIL] $BDF $fld: sample2=$b vs sample1=$a"
                                MONO=1
                        fi
                done
        done
        if [ "$MONO" -eq 0 ]; then
                echo "   [PASS] J3 branch 2: same-driver monotonicity holds; switch-reset finding archived"
        else
                RC=1
        fi
fi

say "Phase C: J6 zero/FEC sanity"
for BDF in $BDFS; do
        P=${BDF##*.}
        LINK=$(awk '/Link detected/ {print $3}' "$WORK/link_$P.txt")
        SUM=$(pr_field0 "$WORK/p12_$P.txt" sym_err)
        SUM=$((SUM + $(pr_field0 "$WORK/p12_$P.txt" sync_hdr)))
        SUM=$((SUM + $(pr_bracket_sum "$WORK/p12_$P.txt" fec_corr)))
        SUM=$((SUM + $(pr_bracket_sum "$WORK/p12_$P.txt" fec_unc)))
        if [ "${SUM:-0}" -gt 0 ]; then
                echo "   [PASS] $BDF: error/FEC counters nonzero (sum=$SUM)"
        elif [ "$LINK" = "yes" ]; then
                if grep -Eq "(corrected|Uncorrectable).*[1-9][0-9]*" "$WORK/fec_$P.txt"; then
                        echo "   [FAIL] $BDF: prism all-zero but ethtool --show-fec shows nonzero"
                        RC=1
                else
                        echo "   [PASS-with-note] $BDF: link up, all error counters zero, ethtool agrees — clean link is a true zero"
                fi
        else
                echo "   [PASS-with-note] $BDF: link down — zero counters expected; liveness rests on J1 tslc"
        fi
done

# ---------------------------------------------------------------- Phase D
say "Phase D: J5 behaviour preservation"
dmesg -c > /dev/null
insmod src/mlx5_prism.ko
sleep 1
PHY_DEFAULT=$(dmesg | grep -c "PHY PPCNT" || true)
rmmod mlx5_prism
[ "$PHY_DEFAULT" -eq 0 ]
res $? "J5a default probe: zero PHY lines in dmesg ($PHY_DEFAULT seen)"

say "J5b: t09_verify 20 cycles"
bash scripts/gate/t09_verify.sh 20 | tail -4
T09LOG=/tmp/t09_run.log
INS=$(grep -c "=== INSMOD OK ===" "$T09LOG" || true)
RMS=$(grep -c "=== RMMOD OK ===" "$T09LOG" || true)
COMPLAINT=$(grep -c "kernel complaint detected" "$T09LOG" || true)
[ "$INS" -eq 20 ] && [ "$RMS" -eq 20 ] && [ "$COMPLAINT" -eq 0 ]
res $? "J5b 20-cycle stress: insmod=$INS rmmod=$RMS complaints=$COMPLAINT (log tail above; full log appended at end)"

say "J5c: baseline diff (expected drift = PHY PPCNT family only)"
bash scripts/baseline/diff_baseline.sh
BAD=0
for P in 0 1; do
        D="$WORK/prism_diff_0000:08:00.$P.txt"
        [ -s "$D" ] || { echo "   [skip] $D empty"; continue; }
        OUT=$(grep '^[+-]' "$D" | grep -v -e '^+++' -e '^---' | grep -v 'PHY PPCNT')
        if [ -n "$OUT" ]; then
                echo "   [FAIL] $D family-external drift:"
                echo "$OUT"
                BAD=1
        else
                echo "   [PASS] $D: drift confined to the PHY PPCNT family"
        fi
done
res "$BAD" "J5c baseline diff: no family-external drift (diff_baseline's own nonzero exit is expected — the baseline predates the PHY lines)"

say "append: full t09 20-cycle log (single-file evidence rule)"
cat /tmp/t09_run.log

kill $LIVE_PID 2>/dev/null
chmod 644 "$RUN" "$LIVE"
say "RESULT: $([ "$RC" -eq 0 ] && echo ALL GREEN || echo FAILURES PRESENT) — evidence: $RUN (blackbox: $LIVE)"
exit "$RC"
