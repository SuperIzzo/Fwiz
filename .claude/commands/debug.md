---
description: Spawn the debugger agent to investigate a hang, perf issue, or mysterious failure without attempting a fix
disable-model-invocation: false
---

Investigate the failing case described in `$ARGUMENTS` using the debugger agent.

The debugger investigates — it does not fix. Its deliverable is a findings document plus any intentionally-kept env-var-gated instrumentation. All temporary hacks (prints, forced early returns, added fields) must be annotated with `// DEBUGGER_HACK:` and cleaned up before the session ends.

Follow these steps:

1. **Capture context**. If `$ARGUMENTS` is a specific CLI command (e.g. `fwiz --derive 'examples/triangle(...)' hangs`), use that as the reproducer. If it's a descriptive question (e.g. "why does X take so long"), first ask the user for a concrete reproducer command so the debugger has something specific to run.

2. **Check for prior debugger sessions** on related work: grep `.fwiz-workflow/` for `debug-*.md` files or existing `FWIZ_TRACE_*` env-var-gated instrumentation. If a prior session laid groundwork, the new debugger spawn should know about it to avoid duplicating instrumentation.

3. **Check for pending DEBUGGER_HACK cleanup** from any prior interrupted session:
   ```bash
   grep -rn "DEBUGGER_HACK" src/ .claude/
   ```
   If anything is found, flag it in the debugger's brief as "clean up the remaining hacks from the prior interrupted session, then proceed with the new investigation."

4. **Spawn the debugger agent** with:
   - The reproducer command(s)
   - Any prior findings docs found in step 2
   - Any pending hacks found in step 3 (list by file:line)
   - The failing design/research context if available (e.g. `.fwiz-workflow/design-*.md` for the related cycle)
   - Explicit charter: "Investigate only. No fixes. Document findings. Clean up every DEBUGGER_HACK before finishing."

5. **After the debugger returns**:
   - Verify `grep -rn "DEBUGGER_HACK" src/` returns nothing
   - Verify `git diff --stat` only shows intentional env-var-gated instrumentation (or nothing at all)
   - Report the findings document path to the user
   - If findings suggest a concrete fix, tell the user and ask whether to spawn an implementer round

6. **If the debugger returns INCONCLUSIVE**: do NOT auto-retry. Present the findings and open questions to the user; they decide whether to spawn another session with refined questions, escalate to a redesign, or accept the uncertainty.

Do NOT:
- Spawn the implementer from this command — `/debug` is for investigation only.
- Commit anything yourself. The debugger's tree state is the deliverable; commits are a separate decision.
- Run `make analyze` or `make sanitize` — those are implementer quality bars, not debugger concerns.

Report the final findings document path and a 3-sentence summary when done.
