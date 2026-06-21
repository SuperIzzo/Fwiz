#!/usr/bin/env bash
# metrics-append.sh — append a per-cycle entry to workflow-metrics.md mechanically.
#
# Solves the workflow-metrics ledger drift: the ledger froze at counter=21 for
# ~24 cycles because the per-cycle append was a prose instruction with no
# checklist step and no helper invoking it. This converts "append a cycle
# entry at close" from prose to procedure — the same fix shape that
# log-spawn.sh applied to the orchestrator-log bundled-close drift.
#
# Invoked at Phase-6 cycle close (see fwiz-orchestrator-ops.md
# §Cycle-Completion Checklist step 6). Two modes:
#
#   # 1. Append a per-cycle header + one-line summary, and BUMP the counter.
#   tools/metrics-append.sh CYCLE "gen-5 3c — Future #7b FULL; 3805->3831 tests"
#
#   # 2. Bump the counter only (when the full table is authored separately).
#   tools/metrics-append.sh BUMP
#
# The counter line MUST match: '**counter = N**' near the top of the file.
# The script reads N, increments it, rewrites the line, and (CYCLE mode)
# appends a dated '## Cycle N' stub the meta-reviewer fills out at compact time.

set -euo pipefail

METRICS=.fwiz-workflow/workflow-metrics.md

if [[ ! -f "$METRICS" ]]; then
    echo "metrics-append: $METRICS not found (run from project root)" >&2
    exit 1
fi

if [[ $# -lt 1 ]]; then
    echo "usage: metrics-append.sh CYCLE <summary...> | BUMP" >&2
    exit 1
fi

MODE="$1"; shift || true

# Extract the current counter value from the canonical '**counter = N**' line.
CUR="$(grep -oE '\*\*counter = [0-9]+\*\*' "$METRICS" | head -1 | grep -oE '[0-9]+' || true)"
if [[ -z "${CUR:-}" ]]; then
    echo "metrics-append: could not find '**counter = N**' line in $METRICS" >&2
    exit 1
fi
NEXT=$((CUR + 1))

# Rewrite the counter line in place (portable sed -i form for GNU sed).
sed -i "s/\*\*counter = ${CUR}\*\*/**counter = ${NEXT}**/" "$METRICS"

case "$MODE" in
    CYCLE)
        TS="$(date +%Y-%m-%d)"
        printf '\n## Cycle %s (%s): %s\n\n_Stub — meta-reviewer fills the quantitative table + file-size baselines at the next compact pass._\n' \
            "$NEXT" "$TS" "$*" >> "$METRICS"
        echo "metrics-append: counter ${CUR} -> ${NEXT}; appended Cycle ${NEXT} stub."
        ;;
    BUMP)
        echo "metrics-append: counter ${CUR} -> ${NEXT} (no stub appended)."
        ;;
    *)
        echo "metrics-append: unknown mode '$MODE' (expected CYCLE or BUMP)" >&2
        exit 1
        ;;
esac
