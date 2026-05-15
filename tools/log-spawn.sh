#!/usr/bin/env bash
# log-spawn.sh — append a timestamped orchestrator-log entry mechanically.
#
# Solves the bundled-close pattern (6 recurrences across 5 cycle shapes, 4
# prose anchors that empirically failed). Converts the "every spawn + every
# return gets its own entry" discipline from prose to procedure.
#
# Usage from the orchestrator:
#
#     tools/log-spawn.sh SPAWN  reviewer  "review-notes draft for cycle X"
#     tools/log-spawn.sh RETURN reviewer  "SHIP-WITH-FOLLOWUP, 4 findings"
#     tools/log-spawn.sh BASH   "make test 2>&1 | tail -3"
#     tools/log-spawn.sh PHASE  "REVIEW open"
#
# The first arg is the action tag; the rest are appended as the description.
# Appends to .fwiz-workflow/orchestrator-log.md with ISO-8601 timestamp.

set -euo pipefail

LOG=.fwiz-workflow/orchestrator-log.md

if [[ ! -f "$LOG" ]]; then
    echo "log-spawn: $LOG not found (run from project root)" >&2
    exit 1
fi

if [[ $# -lt 2 ]]; then
    echo "usage: log-spawn.sh <ACTION> <description...>" >&2
    echo "  ACTION ∈ {SPAWN, RETURN, BASH, PHASE, NOTE}" >&2
    exit 1
fi

ACTION="$1"
shift
TS="$(date -Iseconds)"

printf '\n### [%s] %s\n- %s\n' "$TS" "$ACTION" "$*" >> "$LOG"
