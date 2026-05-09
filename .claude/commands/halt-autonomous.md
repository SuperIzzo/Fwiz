---
description: Exit autonomous mode — the orchestrator returns to interactive mode at the next cycle close
disable-model-invocation: false
---

Exit **autonomous mode**. The orchestrator returns to recommending verdicts and
waiting for user confirmation at the next cycle close.

## Steps

### 1. Read autonomous-mode.md

```bash
if [ ! -f .fwiz-workflow/autonomous-mode.md ]; then
  echo "Autonomous mode is not active — nothing to halt."
  exit 0
fi
STATUS=$(grep '^mode:' .fwiz-workflow/autonomous-mode.md | head -1 | awk '{print $2}')
if [ "$STATUS" != "active" ]; then
  echo "Autonomous mode is in state '$STATUS' — already halted/complete."
  exit 0
fi
```

### 2. Update mode and stamp halt

Edit `.fwiz-workflow/autonomous-mode.md`:

```yaml
mode: halted
halted_at: <ISO-8601 UTC>
halt_reason: user-requested via /halt-autonomous
cycles_so_far: <unchanged>
```

### 3. Log to orchestrator-log.md

```
## [<ISO timestamp>] AUTONOMOUS-HALT

- **What**: autonomous mode halted by user
- **Cycles so far**: <K>
- **Goal status**: <progressing | met | unknown — read from latest reflection.md>
```

### 4. Report to user

Concise:

```
Autonomous mode halted.

Cycles run: <K>
Goal status: <progressing | met | unknown>
Last verdict: <pull from reflection.md>

Returning to interactive mode. The next cycle close will surface the reflector's
recommendation for your approval.
```

## Do NOT

- Do NOT delete `autonomous-mode.md` — leave it on disk with `mode: halted` so
  the run is auditable later.
- Do NOT modify `reflection.md` or `reflector-track-record.md` — those are the
  reflector's append-only logs.
