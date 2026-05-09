---
description: Audit docs/Future.md against project vision; auto-apply high-confidence tier moves and surface only uncertain calls for review
disable-model-invocation: false
---

Run a visionary audit of `docs/Future.md`. The audit's purpose is to keep
`Future.md` aligned with the project vision *autonomously* — high-confidence
moves apply without user review, only uncertain calls escalate.

This command runs both manually (user-typed `/audit-future`) and automatically
(orchestrator at Phase 5 entry when `docs/Future.md` has changed since the
last audit).

## Skip-when-unchanged check

Before doing anything, check whether the audit can short-circuit. The audit
fires when `docs/Future.md` changed OR when vision principles changed
(`visionary.md`, `CLAUDE.md`) — the latter catches vision-drift, where
existing classifications become presumptively stale because the rules they
were judged against moved:

```bash
LAST_AUDIT_FILE=.fwiz-workflow/last-future-audit
if [ -f "$LAST_AUDIT_FILE" ]; then
  LAST_TS=$(cat "$LAST_AUDIT_FILE")
  F_TS=$(stat -c %Y docs/Future.md)
  V_TS=$(stat -c %Y .claude/agents/visionary.md)
  C_TS=$(stat -c %Y CLAUDE.md)
  if [ "$F_TS" -le "$LAST_TS" ] && [ "$V_TS" -le "$LAST_TS" ] && [ "$C_TS" -le "$LAST_TS" ]; then
    echo "No changes since last audit ($(date -d @$LAST_TS '+%Y-%m-%d')) — skipping."
    exit 0
  fi
fi
```

When the trigger is vision drift (visionary.md or CLAUDE.md newer than
Future.md), the visionary should expect to *re-classify previously settled
items* on this pass — they are presumptively stale. Otherwise, only newly-added
items are non-trivially new.

If unchanged, report "no changes since last audit" and stop. Do not spawn the
visionary.

## Steps

### 1. Spawn the visionary in audit mode

Brief:

> Audit mode. Read `docs/Future.md` and your own profile (`.claude/agents/visionary.md`)
> for vision principles. Classify each top-level `## N. Title` item into one of:
> in-scope, wrapper-tool, parked, killed.
>
> Default to **parked** when uncertain — never default to killed.
>
> Skip items with a `**Locked:**` line — output `LOCKED — skipped` and do not
> re-classify.
>
> For each parked or killed item, also emit a reopen trigger (concrete condition
> under which the item should be revisited).
>
> Also: scan items already classified as **parked** or living in `docs/REJECTED.md`
> for **reopen-trigger satisfaction** — if a parked/killed item's reopen
> condition has materialised (e.g. its prerequisite shipped, its dependent
> feature is now planned), propose an UPGRADE move and tag confidence.
>
> Output the classification table + per-item rationale block per the format in
> visionary.md §Audit Mode.

### 2. Receive the classification text and parse

The visionary returns a classification table with confidence levels per item.
Categorise the verdicts:

- **Auto-apply candidates**: every move with `confidence: high`. Includes:
  - New tier assignments (item had no tier; high-confidence verdict assigns one)
  - Upgrades from parked → in-scope where reopen trigger satisfied (high)
  - Moves to wrapper-tool where vision principle is explicitly named (high)
- **Review candidates**: every move with `confidence: medium` or `low`.
- **Locked-skipped**: items the visionary explicitly skipped.
- **Killed (high confidence)**: kills are auto-applied — the case-law trail
  in `REJECTED.md` makes them recoverable. The lock mechanism is the user's
  veto channel for items they want immune from auto-kill.

### 3. Apply auto-applies silently

For each high-confidence move, perform the edit:

- **Tier moves within Future.md**: under each item's heading, add or update
  `**Vision tier:** {tier}` line. Reorganise file structure into four sections
  (`## In-scope`, `## Wrapper-tool`, `## Parked`, with killed items removed).
  Preserve item numbers — they match `COMPLETED.md`.
- **Killed items**: remove from `Future.md` entirely; append to `docs/REJECTED.md`
  using the entry format. Include rationale, vision principle violated, date,
  and reopen trigger (if any).
- **Upgrades from REJECTED.md**: remove entry from `REJECTED.md`, restore to
  `Future.md` under the proposed new tier with a `**Reopened:** YYYY-MM-DD —
  <reason>` line.

Do NOT auto-apply medium- or low-confidence moves. Do NOT touch locked items.

### 4. Surface review candidates as a small batch

For each medium/low-confidence verdict, present to user as a single batch:

```
Audit complete — N high-confidence moves auto-applied silently.

The following M items need your call:

| # | Title | Proposed | Confidence | Reasoning |
|---|-------|----------|------------|-----------|
| 7 | Units / Dimensional Analysis | parked | medium | In-scope but design space large; reopen trigger: a concrete equation that fails without dim-analysis |
| ... |

Reply with item numbers + verdicts (e.g. "7 in-scope, 12 parked, 15 kill") or
"approve all" to take the visionary's proposal as-is. Items left unaddressed
remain in their current state and are re-surfaced next audit.
```

If the user explicitly classifies an item, add a `**Locked:** YYYY-MM-DD —
user override` line so future audits skip it.

If the user says "approve all", treat all medium/low verdicts as user-approved
(but do NOT lock them — only explicit overrides lock).

### 5. Update audit metadata

```bash
date +%s > .fwiz-workflow/last-future-audit
```

Append summary to `.fwiz-workflow/orchestrator-log.md`:

```
### [YYYY-MM-DDTHH:MM:SSZ] FUTURE-AUDIT
- **What**: visionary audit of docs/Future.md
- **Why**: {auto-fire at Phase 5 / user-triggered via /audit-future}
- **Result**: N high-conf auto-applied (X moves, Y kills, Z upgrades), M surfaced for review, K locked-skipped
- **Notable kills/upgrades**: {brief one-liners}
```

### 6. Report

Concise summary to the user — counts and any notable kills/upgrades. Don't dump
the full classification table unless the user asks.

## Do NOT

- Renumber items (numbering shared with `COMPLETED.md`).
- Touch items with a `**Locked:**` line.
- Auto-apply medium- or low-confidence moves.
- Spawn the visionary if the skip-when-unchanged check passes.
- Default to killed under uncertainty (visionary defaults to parked).
- Modify items in their existing high-confidence tier — re-classification
  happens only when the visionary's confidence is high AND the proposed tier
  differs from the current tier.
