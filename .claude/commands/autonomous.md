---
description: Enter autonomous mode — orchestrator continues without user intervention until the goal is reached or a safety brake fires
disable-model-invocation: false
---

Enter **autonomous mode**: the orchestrator continues cycle after cycle without
user intervention until the goal in `$ARGUMENTS` is met (or a safety brake fires).

## Goal interpretation

`$ARGUMENTS` is the goal description in plain language. Examples:

- `/autonomous implement #21 nested formula calls`
- `/autonomous ship the rational propagation in evaluate (Future.md #10)`
- `/autonomous green clang-tidy on src/system.h`

If the user added an explicit completion criterion (e.g. `... — done when X`),
parse it as the criterion. Otherwise, the criterion is `reflector-judged` and
the log-arc reflector evaluates fitness each cycle.

## Steps

### 1. Confirm there's no active autonomous mode already

```bash
if [ -f .fwiz-workflow/autonomous-mode.md ]; then
  STATUS=$(grep '^mode:' .fwiz-workflow/autonomous-mode.md | head -1 | awk '{print $2}')
  if [ "$STATUS" = "active" ]; then
    echo "Autonomous mode is already active. Use /halt-autonomous first."
    exit 1
  fi
fi
```

### 2. Generate the completion criterion (if not user-provided)

If `$ARGUMENTS` does not contain `— done when` or an explicit criterion clause,
spend a short amount of reasoning on what completion looks like for this goal.
Pick the most concrete, mechanically-checkable criterion that captures "done."
Common shapes:

- For Future.md items: `Future.md #N moves to COMPLETED.md AND make test passes`
- For cleanup goals: `make analyze-fast clean AND grep for the targeted pattern returns 0`
- For perf goals: `<benchmark> < <target>`
- Default fallback: `reflector-judged` (reflector evaluates per cycle)

Pick a criterion that minimises ambiguity; don't be clever. If you can't find
a concrete one, default to `reflector-judged` and note that explicitly.

### 3. Determine `allowed_dispositions`

Default for most goals: `[continue, new-cycle keep-context, new-cycle clear-context, new-arc]`.

Special cases:

- If goal is "tiny / single feature": exclude `new-arc`. Implementing one Future.md item shouldn't replan the whole roadmap.
- If goal is "explore / research": exclude `clear-context`. Conversation history matters more for exploratory work.

Default `max_cycles`:

- Single feature: 10
- Cleanup pass: 15
- Larger goal (multi-feature): 20

These are safety brakes, not expected lifetimes.

### 4. Write `.fwiz-workflow/autonomous-mode.md`

```yaml
mode: active
goal: <full goal description from $ARGUMENTS>
goal_completion: <criterion — concrete or "reflector-judged">
started: <ISO-8601 UTC>
cycles_so_far: 0
max_cycles: <N>
allowed_dispositions:
  - continue
  - new-cycle keep-context
  - new-cycle clear-context
  - new-arc  # remove if excluded
notes: <one line on what type of goal and why these dispositions>
```

### 5. Log to orchestrator-log.md

```
## [<ISO timestamp>] AUTONOMOUS-START

- **What**: autonomous mode entered
- **Goal**: <goal>
- **Completion criterion**: <criterion>
- **Allowed dispositions**: <list>
- **Max cycles**: <N>
```

### 6. Confirm to user

Concise message:

```
Autonomous mode active.

Goal: <goal>
Completion: <criterion>
Allowed actions: <list>
Safety brake: <max_cycles> cycles

The orchestrator will continue cycle-by-cycle without further input. To halt
early, run /halt-autonomous (or just type anything — any user input exits
autonomous mode).
```

### 7. Begin Phase 1 of the next cycle

Use the goal description as the user brief and start the workflow as if the
user had asked for the goal directly. The reflector takes over at Phase 6.

## Do NOT

- Do NOT enter autonomous mode if one is already active (use /halt-autonomous first).
- Do NOT default to `reflector-judged` if a concrete criterion is reasonable — concrete is always preferable.
- Do NOT include destructive dispositions for low-stakes goals.
- Do NOT skip the autonomous-mode.md file — it's the single source of truth that the reflector reads each cycle.
