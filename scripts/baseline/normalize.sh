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

sed -e 's/^\[[^]]*\] //' \
    -e 's/cpu=[0-9a-fx]* dma=0x[0-9a-f]*/cpu=X dma=X/g' \
    -e 's/doorbell=[0-9a-fx]*/doorbell=X/g' \
    -e 's/\[mem 0x[0-9a-f]*-0x[0-9a-f]* [^]]*\]/[mem X]/g' \
    -e 's/ *[0-9]\+ ms/ T ms/g' \
    -e 's/([0-9]\+ events total, [0-9]\+ interrupts)/(E events total, I interrupts)/' \
    -e 's/([0-9]\+ events, [0-9]\+ consumed in hardirq, [0-9]\+ interrupts)/(E events, H consumed in hardirq, I interrupts)/' \
    -e 's/DESTROY_EQ ok: eq_number=\([0-9]*\), [0-9]* events consumed/DESTROY_EQ ok: eq_number=\1, E events consumed/' \
| grep 'mlx5_prism ' \
| grep -v -e 'no EQE within' \
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
