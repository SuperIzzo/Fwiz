---
description: Run the blind-spot critic in full-sweep mode — test every eligible function in the codebase, not just the cycle's diff
disable-model-invocation: false
---

Run the blind-spot critic against the **entire codebase**, not just the cycle's
diff. Use this when you want a comprehensive readability audit — typically
periodically (every several cycles) or after a large structural change.

For per-cycle diff-based testing, the orchestrator fires the blind-spot critic
automatically at Phase 6 prelude. This command is for the user-triggered full
sweep.

## Steps

1. **Spawn the blind-spot-critic agent** with the brief:

   > Run in full-sweep mode. Test every eligible function in `src/*.h` and
   > `src/*.cpp` per the eligibility rules in your profile. Do NOT sample —
   > test all eligible functions. Output the same summary as a cycle run.

2. **Wait for completion** — full sweeps are larger than per-cycle runs. Expect
   the agent to test many functions; each function spawns Haiku graders at three
   tiers, two evaluators per tier. Cost is bounded but real.

3. **Surface the summary to the user** — counts, top offenders, refactors filed,
   rules extracted, trend notes.

4. **Update `.fwiz-workflow/last-blind-spot-commit`** with the current `HEAD`
   commit hash, so the next per-cycle run starts its diff from this point and
   doesn't re-test functions the sweep just covered:

   ```bash
   git rev-parse HEAD > .fwiz-workflow/last-blind-spot-commit
   ```

5. **Log the sweep** in `.fwiz-workflow/orchestrator-log.md`:

   ```
   ### [YYYY-MM-DDTHH:MM:SSZ] BLIND-SPOT-SWEEP
   - **What**: full-codebase blind-spot critic
   - **Why**: user-triggered via /blind-spot-sweep
   - **Result**: N functions tested, M failed, K refactors filed, L rules extracted
   ```

## Do NOT

- Do NOT auto-apply any refactor proposals — they go into `docs/Future.md` for
  the visionary audit and the implementer to act on in future cycles.
- Do NOT skip the post-run commit-hash update. Without it, the next per-cycle
  run will redo a chunk of the sweep's work.
- Do NOT modify source files. The blind-spot critic's writes are confined to
  `docs/Future.md`, `docs/Code-Style.md`, and `.fwiz-workflow/blind-spot-scores.md`.
