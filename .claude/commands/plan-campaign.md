---
description: Run the plan-ideator + plan-critic pair to generate a multi-cycle campaign and update docs/ROADMAP.md
disable-model-invocation: false
---

Generate a new arc for `docs/ROADMAP.md`. Spawns the plan-ideator (divergent
campaign generation) followed by the plan-critic (selection), then archives the
old roadmap and writes the new one.

This command runs both:

- **Auto** — when the log-arc-reflector emits `new-arc` at Phase 6.
- **Manual** — user-typed `/plan-campaign [optional seed text]`. The seed text
  (e.g. "focus on perf for the next month") is passed to the ideator as a soft
  constraint.

## Steps

### 1. Capture context

Read `docs/ROADMAP.md` to understand the current state (active arc if any,
queued arcs). Note the current `<!-- generation: N -->` value; the new
generation will be N+1.

If `$ARGUMENTS` contains seed text, capture it for the ideator brief.

### 2. Spawn plan-ideator

Brief:

> Generate divergent multi-cycle campaign plans for fwiz. Read your full input
> set per your profile. Produce 3-5 genuinely different campaign shapes per
> the divergence rules. Return your output as text — do not modify any
> artifacts.
>
> [If seed text provided:] User seed: "<seed>". Treat as a strong but not
> absolute input. Generate at least one campaign that takes the seed seriously
> AND at least one that treats it as a constraint to challenge.

### 3. Spawn plan-critic

Brief:

> Select one campaign from the plan-ideator's divergent set. Apply your selection
> axes; be opinionated. Return your output as text — do not modify any artifacts.
>
> Plan-ideator output:
>
> <full ideator text>

### 4. Archive the old roadmap

Before writing the new one, snapshot the current `docs/ROADMAP.md` to the archive:

```bash
mkdir -p .fwiz-workflow/roadmap-archive
ARCHIVE_NAME="$(date +%Y-%m-%d)-gen$(grep -oE 'generation: [0-9]+' docs/ROADMAP.md | head -1 | awk '{print $2}').md"
cp docs/ROADMAP.md ".fwiz-workflow/roadmap-archive/$ARCHIVE_NAME"
```

If `docs/ROADMAP.md` is at generation 0 (empty scaffold), still archive it for
auditability — the empty-state snapshot is itself history.

### 5. Compose the new ROADMAP.md

From the plan-critic's selected winner:

- Set the active arc to the winner's name, theme, milestones, vision alignment, why-chosen.
- Set the queued arcs section to the runner-up (and any prior queued arcs that haven't been chosen and are still relevant — re-evaluate against the critic's verdict).
- Move any prior active arc to "Completed arcs" with outcome notes if it shipped, or to queued / dropped if it didn't.
- Increment `<!-- generation: N -->` to N+1.
- Update `<!-- last-updated -->` and `<!-- selected-by-cycle -->`.
- Append to "Generation log" a one-liner: `Generation N+1 (cycle X): <winner name> selected — <why-this-one summary>.`

Write the result to `docs/ROADMAP.md`.

### 6. Surface to user (interactive mode) or apply (autonomous mode)

Detect mode:

```bash
if [ -f .fwiz-workflow/autonomous-mode.md ] && grep -q '^mode: active' .fwiz-workflow/autonomous-mode.md; then
  AUTONOMOUS=1
else
  AUTONOMOUS=0
fi
```

**Interactive mode:** present the new active arc to the user with the critic's
reasoning. Ask: "Approve, swap to runner-up, or re-run ideator with adjusted
seed?" Apply per the user's choice. If user swaps to runner-up, update
ROADMAP.md again.

**Autonomous mode:** if critic's confidence is **high**, the new arc takes
effect silently; the orchestrator's next cycle uses the new arc as its
strategic anchor. If confidence is **medium** or **low**, exit autonomous mode
and ping the user — autonomous shouldn't be picking arcs on weak evidence.

### 7. Log

Append to `.fwiz-workflow/orchestrator-log.md`:

```
## [<ISO timestamp>] PLAN-CAMPAIGN

- **What**: ran plan-ideator + plan-critic pair; ROADMAP.md generation incremented
- **Trigger**: <reflector new-arc verdict | user /plan-campaign>
- **Seed (if any)**: <seed>
- **Winner**: <campaign name>
- **Critic confidence**: <high/medium/low>
- **Mode**: <interactive | autonomous>
- **User action (interactive)**: <approved | swapped to runner-up | re-ran>
```

## Do NOT

- Do NOT spawn the ideator and critic in parallel. The critic depends on the
  ideator's full output; sequential is required.
- Do NOT skip the archive step. Prior generations are signal — the next ideator
  run reads them to avoid re-proposing identical plans.
- Do NOT auto-apply a low-confidence critic verdict in any mode. Low confidence
  always escalates to user (or exits autonomous).
- Do NOT modify `docs/Future.md`, `docs/REJECTED.md`, or any item-level artifact
  from this command. The roadmap describes which items get worked on across
  cycles; it doesn't change the items themselves.
