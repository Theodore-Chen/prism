#!/bin/bash
# normalize.sh — shared dmesg normalizer for baseline comparison.
#
# Sourced/executed by diff_baseline.sh and by the baseline-regeneration
# procedure (see scripts/baseline/README). Reads raw dmesg lines on stdin,
# writes normalized mlx5_prism lines to stdout. Volatile values
# (timestamps, DMA addresses, timings) collapse to placeholders; the
# ~10% event-engine engagement quirk block (silent -> destroy/recreate)
# is legal behavior and is deleted, with the duplicate QUERY_EQ line
# that the recreate path produces collapsed back to one.
#
# Deliberately NOT normalized (regression anchors, must stay stable):
#   - caps[max]/caps[cur]/caps[post] hexdumps (bit-exact across 100
#     cycles on FW 14.32.1908 — any mailbox/bit-layout drift shows here)
#   - counts: boot pages=6, init pages=2299, teardown 2305/2305,
#     log_sz=5/log_stride=6, eqn=4, 10/10 self-test
#   - "EQ up (event mode)" 的 eqn/entries/budget（T10 起事件模式为默认）
#
# (T10-prep) "(E events total, I interrupts)": one MSI-X can cover
# several EQEs and the arm-time extra fires variably, so the two counts
# are aggregation-random per load — normalized.
# (T10) event-mode counts likewise: self-test "(E events, H consumed in
# hardirq, I interrupts)" and "DESTROY_EQ ok: ... E events consumed" —
# backlog aggregation makes the per-load event count 11 or 13, hardirq
# consumption always equals it; the per-EQE eqe[N] dmesg lines are gone
# (hardirq hot path stays silent; tracepoints are the observability).
# (T12) "PHY PPCNT" self-test lines: the counter values are monotonic
# run-time readings (they grow every cycle and survive driver switches),
# so every value collapses to a placeholder; the line STRUCTURE (field
# names, group tag 0x12/0x00/0x16, probe outcome wording) stays as a
# regression anchor. Scoped to PHY lines only — the eqn/log_sz anchors
# elsewhere must not be touched.
# (T12 rebaseline 2026-09-26) "eqe[N]: type=..." lines: the event-mode
# setup drains the backlog EQE best-effort (T10's drain-before-enable,
# eq.c) and prints one such line WHEN the in-flight QUERY_EQ completion
# happens to land inside the drain window — a legal timing variant that
# appears or not per load. Deleted like the other quirk lines (in
# eq_mode=poll loads every consumed EQE prints this format as well).
# (T13 2026-09-27) "PHY monitor" probe/bring-up lines (every default
# probe now): oper_status is live link state (varies with cabling), the
# mdegC reading varies with ambient, interval is normalized defensively;
# the probe OUTCOMES (accepted/rejected, len, yes/no) are quirk-profile
# anchors and stay literal.
# (T14 2026-09-27) "smoke self-test" lines (selftest=1 loads only): the
# fw-assigned object ids (pd/mkey/cqn/sqn/tdn/tisn) vary every load and
# collapse to placeholders; the line STRUCTURE — stage number, tier value,
# wqe_counter 0/1, byte_cnt=64, the PASSED verdict — are regression
# anchors and stay literal. Scoped to smoke lines only.
# (T15 2026-10-01) netdev bring-up lines (every default load now): the
# dedicated comp EQ adds "comp eq:" (cpu/dma covered by the generic
# rule) and "comp EQ up:" (eqn is fw-assigned and normalized; the
# entries=4096/vector 0/zero-mask wording stays literal as the v5.15
# create_comp_eqs-form anchor), plus "netdev prismN registered (mac ..)"
# (mac collapses; port pairing and wording stay literal).

sed -e 's/^\[[^]]*\] //' \
    -e 's/cpu=[0-9a-fx]* dma=0x[0-9a-f]*/cpu=X dma=X/g' \
    -e 's/doorbell=[0-9a-fx]*/doorbell=X/g' \
    -e 's/\[mem 0x[0-9a-f]*-0x[0-9a-f]* [^]]*\]/[mem X]/g' \
    -e 's/ *[0-9]\+ ms/ T ms/g' \
    -e 's/([0-9]\+ events total, [0-9]\+ interrupts)/(E events total, I interrupts)/' \
    -e 's/([0-9]\+ events, [0-9]\+ consumed in hardirq, [0-9]\+ interrupts)/(E events, H consumed in hardirq, I interrupts)/' \
    -e 's/DESTROY_EQ ok: eq_number=\([0-9]*\), [0-9]* events consumed/DESTROY_EQ ok: eq_number=\1, E events consumed/' \
    -e '/PHY PPCNT/ s/=\[[0-9][0-9 ]*rs [0-9][0-9]*\]/=B[rs N]/g' \
    -e '/PHY PPCNT/ s/=\[[0-9][0-9 ]*\]/=B[N]/g' \
    -e '/PHY PPCNT/ s/=[0-9]\+/=N/g' \
    -e '/PHY PPCNT/ s/rejected (-[0-9]\+)/rejected (-N)/' \
    -e '/PHY monitor/ s/interval [0-9]\+ms/interval Nms/' \
    -e '/PHY monitor/ s/oper_status=[0-9]\+/oper_status=N/' \
    -e '/PHY monitor/ s/-\{0,1\}[0-9]\+ mdegC/N mdegC/' \
    -e '/smoke self-test/ s/pd=[0-9]\+/pd=N/' \
    -e '/smoke self-test/ s/mkey=0x[0-9a-f]\+/mkey=0xX/' \
    -e '/smoke self-test/ s/cqn=[0-9]\+/cqn=N/' \
    -e '/smoke self-test/ s/sqn=[0-9]\+/sqn=N/' \
    -e '/smoke self-test/ s/tdn=[0-9]\+/tdn=N/' \
    -e '/smoke self-test/ s/tisn=[0-9]\+/tisn=N/' \
    -e '/smoke self-test/ s/byte_cnt=[0-9]\+/byte_cnt=N/' \
    -e '/smoke self-test/ s/failed (-[0-9]\+)/failed (-N)/' \
    -e '/SET_HCA_CAP ok/ s/wqe_sz82=0x[0-9a-f]\+/wqe_sz82=0xX/' \
    -e '/comp EQ up/ s/eqn=[0-9]\+/eqn=N/' \
    -e '/netdev prism/ s/(mac [0-9a-f:]*)/(mac X)/' \
    -e '/opcode 0x904/ s/command status 0x[0-9a-f]\+/command status 0xN/' \
    -e '/opcode 0x904/ s/syndrome 0x[0-9a-f]\+/syndrome 0xX/' \
| grep 'mlx5_prism ' \
| grep -v -e 'no EQE within' \
          -e 'eqe\[[0-9]*\]: type=' \
          -e 'eqe\[0\.\.3\] head:' \
          -e 'owners\[0\.\.7\]:' \
          -e 'event engine silent at' \
          -e 'DESTROY_EQ ok: eq_number=[0-9]*, 0 events consumed' \
| awk '
    # The ~10% engagement quirk makes the recreate path (extra QUERY_EQ
    # after the deleted silent block) a legal random variant: keep only
    # the first QUERY_EQ per load cycle. Cycle boundary = BAR0 mapped.
    /BAR0 mapped/  { seen_query_eq = 0 }
    /QUERY_EQ: fw reports/ {
        if (seen_query_eq) next
        seen_query_eq = 1
    }
    { print }'
