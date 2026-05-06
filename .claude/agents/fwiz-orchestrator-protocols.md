# Fwiz Orchestrator — Conditional Protocols

This file holds decision protocols that fire on specific triggers, not every cycle. The orchestrator reads it on demand from `fwiz-orchestrator.md`. Each section names its trigger; if the trigger does not apply this cycle, the section is irrelevant.

Sibling files: `fwiz-orchestrator.md` (core), `fwiz-orchestrator-ops.md` (background tasks, artifact lifecycle, oracle policy).

---

## Design-time protocols

### Brief intake — cleanup cycles only

Trigger: brief matches "cleanup" / "warnings" / "tech debt" / "lint" (case-insensitive).

Confirm the brief carries the four pieces below before spawning researchers. If missing, ask ONE question per gap — don't interview.

- **Target delta** — concrete ("167 warnings → 0", "sizeof(Expr) 56 → 48")
- **Explicit skip list with rationale** — what stays unchanged and why
- **Per-category acceptance** — what "done" looks like per category
- **Future.md reopen trigger for deferrals** — any deferred category gets an entry

Fires ONLY for cleanup cycles; feature cycles stay open-ended. (Validated in the warnings-cleanup cycle — see `750fe35`, `0da63ea`.)

### Brief grep-assertion self-consistency check

Trigger: brief (master-plan or freshly authored) contains both a count assertion AND a `grep -v <annotation_token>` filter aiming at zero, AND a stated annotation placement ("immediately preceding declaration", "trailing comment on the declaration line", etc.).

Before locking the brief (or as part of the master-plan-execution intake when reusing a pre-written brief), simulate the chosen annotation shape against the filter on a single example. If the declaration line itself contains the filter token (e.g. `std::function`), an annotation placed on the **preceding line** leaves the declaration line unfiltered — count and filter become mutually unsatisfiable, forcing the implementer to choose one constraint and silently violate another. The satisfiable shapes are: (a) trailing comment on the declaration line itself (filter scrubs the same line), or (b) different filter token (`// keep:` instead of `// std::function:`). Pick at brief-author time, not at implementer time. Canonical miss: Cycle 5 S3 std::function triage 2026-05-06 — brief stipulated "annotation immediately preceding declaration" + `grep -v '// std::function:'` returns 0 + count ≤ 8; the three-way contradiction was un-resolvable under the stated annotation placement; implementer recognized it and silently chose trailing-comment style. Outcome shipped clean but the brief carried an unresolved spec contradiction.

### Stale-diagnostic protocol — when reusing prior-cycle data instead of fresh research

Trigger: brief proposes "skip RESEARCH because D{N} from the prior cycle answers the question."

STOP. If the prior cycle changed ANY code in the codepath D{N} measured, D{N} is presumptively stale. Two valid paths: (a) spawn a one-step researcher with the narrow brief "re-run D{N}'s probe post-{prior commit}, report numbers, no other research needed" (~5 min wall-clock); or (b) inline-self-run the original probe and write a one-paragraph freshness check at the top of any thin research brief you author for the next cycle. Do NOT advance to DESIGN with stale-data-derived motivation citing the prior cycle's numbers as if still applicable. Canonical miss: Tier 2 cycle 2026-04-25 — orchestrator wrote a thin research brief reusing D3's pre-Tier-1 measurements; planner+critic+visionary APPROVED; implementer measured fresh and the entire design was a structural no-op (Tier 1 had absorbed what D3 measured). Three design agent spawns wasted; one orchestrator-side `bin/fwiz` re-run before authoring the thin brief would have caught it.

### Cascade forecast for type-qualifier migrations

Trigger: design widens a pointer/reference qualifier on a shared overload (`ExprPtr` → `const Expr*`, `T&` → `const T&`, etc.).

Simulate on a throwaway copy + run cppcheck before approving. If > 10 new caller-site warnings, split into two milestones (widening + cascade cleanup); if ≤ 10, note the cascade-count in the implementer brief. (See `aaf1bbb` / `ffe173e`.)

### Autonomous DESIGN — skipping planner/critic/visionary

Trigger: orchestrator considering executing a design without the 3-agent DESIGN phase.

Allowed **only when ALL** of: (a) scope under ~100 LOC across ≤ 3 files; (b) user has explicitly constrained the architectural shape *in this brief* (not inferred from a prior cycle); (c) no `recognize_*` heuristic, no new magic number/threshold, no new public API surface, no new filter/bound/tolerance being invented.

If ANY heuristic threshold is being picked (`max_den=12`, `|p| ≤ 12`, power-of-10 rule, ε tolerance), spawn the **critic** — under-motivated magic numbers are its job. When skipping, log the decision with explicit justification against (a), (b), (c). "Well-scoped" is not a justification.

### Master-plan execution — skipping RESEARCH and DESIGN

Trigger: user's brief explicitly invokes a per-cycle execution slug from a multi-cycle audit roadmap or master plan (e.g. "Run Cycle N from `.fwiz-workflow/design-X-cycles.md`"), AND the referenced brief contains the standard precision fields (target delta, exact sites, skip list, per-category acceptance, lock mechanism, verification command, LOC delta, reopen trigger, Stop-and-Ship Criteria).

Skip RESEARCH and DESIGN cleanly and go directly to IMPLEMENT. This is the legitimate Phase 2B execution path — design was already done at master-plan time. Required logging: at CYCLE START, an explicit "Phase plan: RESEARCH skip — design brief IS the research; DESIGN skip — already written at lines L–M of design-file" entry. If any of the standard precision fields is missing OR the brief invents a new architectural surface (new primitive, new threshold, new abstraction), do NOT skip — fall back to full RESEARCH/DESIGN. Canonical: Cycles 1 and 2 of the C++ Best Practices Audit 2026-05-05 — both ran clean as master-plan execution, ~75 min and ~80 min wall-clock respectively, zero design-loaded surprises; the brief's precision fully substituted for fresh design rounds.

---

## Implementer-coordination protocols

### Pre-flight test-site flagging

Trigger: spawning the implementer for a contract-changing migration (return type, exception shape, `.value()` vs `operator*`, etc.).

Scan `src/tests.cpp` for sites whose assertion style depends on the OLD contract — tests catching `std::bad_optional_access`, relying on `operator*` throwing vs. `.value()` asserting, checking `std::isnan` via `*opt`. List these sites in the implementer brief with the exact rewrite. Without this, the implementer wastes a cycle on "harness mismatch or real bug?" (Validated: Checked<T> cycle — see `7095f95`, `e65e1fe`.)

Note: contract-changing migrations also require the critic-accepted/rejected items list to be echoed into `review-notes*.md` so the reviewer validates design fidelity (did the implementation honor each decision?), not just code quality.

### Domain-sensitive test data

Trigger: design specifies numeric test points (fingerprint probes, property-based sampling, numeric-solver seeds).

Scan the user's reproducer for implicit domain constraints BEFORE the design is locked: triangle inequalities, positivity, monotonicity, branch-cut regions, unit-box constraints. If the design's test-point formula is a generic scheme (prime cycling, uniform sampling), spot-check it against the reproducer's bindings on paper — do two or three points land in-domain?

When implementer reports BLOCKED with "all candidates NaN / domain-violating at test points" as the failure pattern, the fix is a test-data change, NOT a dedup / algorithm change. Orchestrator self-fix is appropriate here even if the delta is >5 LOC, because the change is a constant-choice correction under a design invariant the domain-scan should have caught. Log the miss so the next cycle's DESIGN phase can spot it earlier. Canonical miss: derive-dedup cycle M3-6 — multiplicative prime scheme (b=10,c=6) violated triangle inequality at a=4; self-fix switched to per-variable cycling (b=2,c=3), ~5 LOC, mechanical once the domain constraint surfaced, but required triangle-inequality judgment to notice.

---

## Recovery protocols

### Single-BLOCK recovery — inline revisit vs critic-visionary respawn

Trigger: implementer returns BLOCKED **once** with a thorough diagnostic that already names the design assumption it invalidated.

The recovery path is NOT a debugger round (only 1 block) and NOT always a fresh critic+visionary spawn. Choose:

- **Inline orchestrator revisit** — appropriate when (a) the implementer's diagnostic already identifies the failing design hypothesis; (b) the fix is a scope shrink (drop a flag, drop a filter, drop a CLI mode), not a scope widen; (c) no new architectural decision is required; (d) the user is available to approve the revised spec in one round-trip. Orchestrator appends a "Revised M{n} after implementer block" section to `design-proposal.md` documenting the original-vs-revised spec and the cycle evidence that forced the change. Then re-spawn a fresh implementer with the revised section as the only design context. Canonical: derive-ordering cycle 2026-04-19T23:55 — sentinel-suppression dropped, discriminator-flip kept, Defect A fix added as cleanup bonus.
- **Mini critic+visionary respawn** — appropriate when the diagnostic reveals a new architectural question (new primitive, new abstraction, new bound/threshold), or when the user's original Q&A was based on an incorrect model of the bucket/population/class the planner proposed to filter. If the revised direction will re-introduce any of the planner-rejected alternatives, the critic should hear it.

Default when uncertain: inline revisit first (faster, 1 round-trip). If user pushes back or the revised spec still has architectural ambiguity, escalate to critic+visionary. Log which path you chose and why.

### Diagnostic rounds — debugger after 2x BLOCKED

Trigger: implementer returns BLOCKED **twice** on the same design.

Do NOT spin a third attempt — the design's model is likely wrong. Spawn the **debugger** agent (`.claude/agents/debugger.md` or `/debug`): it instruments, traces, writes findings, cleans every `DEBUGGER_HACK`. It does NOT fix. After it returns: (1) `grep -rn "DEBUGGER_HACK" src/` returns nothing; (2) `git diff --stat` shows only intentional env-var-gated instrumentation (or nothing); (3) if findings invalidate a design assumption, send a **mini design revisit** (critic + visionary on the revised question — not a full redesign); (4) spawn a fresh implementer round. Canonical trigger: triangle-hang cycle (`da3ee21`) — 2 BLOCKED → debugger round (promoted `FWIZ_TRACE_SOLVER`) → ship.

### Hypothesis-failure decision protocol

Trigger: implementer reports "shipped the spec correctly, all invariant-based BLOCKING criteria pass, but a metric-based BLOCKING criterion (line count, ratio, `drops to ~N`) did not hit its predicted target."

The cycle is not blocked — the rule / change / migration is correct; the *prediction* about its downstream effect was wrong. Decision tree:

1. **Are all invariant-based BLOCKING criteria (tests pass, sanitize clean, analyze clean, structural assertions hold) met?** If NO → standard BLOCKED handling (scope shrink or diagnostic round). If YES → continue.
2. **Was the failed metric-based criterion hypothesis-derived (per Phase 2 Stop-and-Ship Criteria rule)?** If it names a cascade, propagation, or "because X will cause Y downstream," yes. If NO (genuinely structural but miscounted) → treat as implementation bug, re-spawn implementer. If YES → continue.
3. **Ship the cycle**: all structural correctness criteria passed; the hypothesis failure is a negative result worth documenting.
   - Flag the lapse to the meta-reviewer: the criterion should not have been BLOCKING.
   - Write the negative finding into `next-priorities.md` with the empirical evidence (pre/post numbers, why the cascade didn't materialize).
   - If the residual surfaces a deeper architectural question the hypothesis was trying to address, open a research-anchor doc in `docs/research/` for the next cycle (see `fwiz-orchestrator-ops.md` §Artifact placement).
   - Do NOT amend the original design post-hoc to make the failed BLOCKING look like DESIRABLE — leave the artifact as evidence. The meta-reviewer edits the agent profiles to prevent the class of mistake.

Canonical: P1-tautology cycle `3bcccbd` — all invariant criteria passed (sqrt^2 substring absent, tests pass, sanitize clean); metric criterion `line count < 100` was cascade-derived and failed (159 → 159); orchestrator shipped under the invariant set, logged the negative result to `next-priorities.md` and `docs/research/category-c-investigation.md`, flagged the BLOCKING-tagging lapse to the meta-reviewer, and the cascade prediction's failure became the research motivation for the next cycle.

### Ad-hoc meta-review (mid-cycle agent failure)

Trigger: an agent produces unexpected or low-quality output mid-cycle (implementer fails 3 times, critic produces nonsense, etc.).

Spawn **meta-reviewer** immediately with: "The {agent} was given {context summary}. It produced {output summary}. Diagnose and recommend a profile fix." Do NOT wait for end of cycle.

---

## Workflow-special protocols

### Phase overlap — next-cycle research during current-cycle REVIEW

Trigger: `make analyze-full` running in background on user request (typical ~1-2h wait), AND user surfaces a natural scope-scoping question for the next cycle ("are there more X? let's plan the next cycle on that").

Note: under the tiered-oracle policy this overlap window is now **rare** (per-cycle gate is `analyze-fast` ~1-2 min — no overlap window). It applies only when the user explicitly runs `analyze-full` mid-session and the orchestrator is idle waiting for it. Permitted overlap:

- **Allowed**: running reproducers, categorizing output, writing a research brief to `.fwiz-workflow/research-brief.md` (rotating the previous one to `research-brief-<prev-scope>.md` first).
- **Not allowed**: spawning planner/critic/visionary for the next cycle while the current cycle's review is open — design phase must wait for the current cycle to CLOSE and the user to approve the research.
- **Not allowed**: writing `next-priorities.md` for the next cycle before current cycle's review completes. The review may produce SHIP-DESIRABLE items that belong in next-priorities.

Canonical: derive-ordering cycle 2026-04-20T00:50 — user asked "check if there are more tautological entries" mid-REVIEW (analyze still running). Orchestrator ran the reproducer, captured 159-line output, wrote 6-category research brief. When review completed, next-priorities.md referenced the already-written brief cleanly. ~30 min wall-clock saved vs serial; zero risk of cross-phase context contamination because RESEARCH is strictly read-only on the current cycle's artifacts.

### Follow-up micro-cycles

Trigger: cycle ships with a compromise on SHIP-DESIRABLE behavior.

The follow-up is a named **micro-cycle**: tiny research artifact (often <1 page) answering a specific question from the ship commit; no planner/critic/visionary round unless the fix is architectural; one implementer spawn with one or more reviewer-pre-spec'd narrow targets bundled; commit separately, referencing the ship commit.

**Reviewer elision in micro-cycles** is permitted ONLY when ALL of: (a) every item was reviewer-pre-spec'd in the prior cycle's review-notes (no new design surface this cycle); (b) total LOC ≤ ~80; (c) behavioral changes (non-doc, non-test items) have reviewer-equivalent diligence baked into the implementer's verification (grep-proof of invariants, RGR on testable items). If any of (a)/(b)/(c) fails, spawn the reviewer. Default for full cycles remains: reviewer always fires. Canonical: polish-pass cycle `fe8e91e` — 7 items all from prior review-notes, ~80 LOC, item 5 (dirty-flag) verified by grep that `equations` is never cleared/erased/resized.

### Multi-cycle audit roadmap — per-cycle artifact archival

Trigger: this cycle is part of a multi-cycle audit roadmap (a `.fwiz-workflow/design-*-cycles.md` file exists with Cycle N+1 listed).

At META-REVIEW close archive `implementation-log.md`, `review-notes.md`, `next-priorities.md` to `.fwiz-workflow/archive/<roadmap-name>-cycle{N}/` so Cycle N+1 starts with empty active files. The harness blocks file truncation (correct under single-cycle assumptions); per-cycle archival inside a known multi-cycle roadmap is the correct cleanup pattern. Roadmap-name = the design file's middle slug (e.g. `design-cpp-best-practices-cycles.md` → archive dir `archive/cpp-best-practices-cycle{N}/`). Canonical: cpp-best-practices audit cycle 1 close, 2026-05-05 — meta-review surfaced the multi-cycle accumulation pressure (4 active files growing across 8 planned cycles); per-cycle archival keeps each cycle's per-file context bounded. Does NOT apply to single-cycle work — archive at retention threshold per `fwiz-orchestrator-ops.md` §Artifact retention.
