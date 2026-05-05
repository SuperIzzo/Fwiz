# Fwiz Orchestrator — Operations

This file holds full text for the orchestrator's system-hygiene rules: oracle policy, background-task discipline, cycle-completion checklist, artifact placement, retention. The core profile (`fwiz-orchestrator.md`) carries TL;DR versions of the most-frequently-applied items; this file is the canonical reference.

Sibling files: `fwiz-orchestrator.md` (core), `fwiz-orchestrator-protocols.md` (conditional decision protocols).

---

## Quality Bar (tiered oracle)

**Per-cycle gate (mandatory)**: `make test && make sanitize && make analyze-fast` (cppcheck — ~1-2 min). Every cycle. No exceptions.

**Periodic full oracle (user-triggered)**: `make analyze-full` (clang-tidy — ~1-2h on this header-heavy codebase). The user runs this during natural PC idle windows (overnight, before work, weekends). The orchestrator does NOT run it per-cycle — that was the old policy and it generated unsustainable cycle-close friction (1-2h wait per cycle even on tiny diffs). The orchestrator's job is: (a) **track the debt** — at each cycle close, count cycles since last green clang-tidy and surface gently in `next-priorities.md` ("debt: N cycles unanalyzed since {commit}, full diff is M lines"); (b) **audit residuals when the user runs the batch** — read the log, grep findings (see "What to grep" below), diff against the last clang-tidy baseline, fix per-finding triage on the cumulative diff. There is **no hard-stop**, no 3-strike escalation; the debt is the principled trade-off, not an exception.

**What to grep (both fast and full)**: exit code 0 only means the tool ran — NOT warning-free. MUST grep `warning:` / `error:` / `style:` / `performance:` in `src/*.h`, `src/*.cpp` and compare to previous baseline. Report delta (before, after, new). cppcheck emits `style:` / `performance:` severity prefixes for issues clang-tidy doesn't catch (constVariableReference, shadowFunction, constVariablePointer); grepping only `warning:` / `error:` silently misses them (Cycle B `--cse` baseline). Exit code alone is a reporting failure (baseline: `046bfec`).

**Why this policy**: clang-tidy on `src/main.cpp` walks ~10k LOC of transitively-included headers; analyzer-* checks dominate the cost. The codebase is intentionally header-heavy (no link-time machinery, arena-friendly). Per-touched-file analyze doesn't help (every header edit re-checks main.cpp); splitting `tests.cpp` doesn't help (clang-tidy doesn't touch it). The only meaningful cost lever is to run clang-tidy less often, not to make it faster. Honest tiering > pretending each cycle gets the full oracle.

### Bridge task — when the user runs `make analyze-full`

Not a phase. Read the log, grep `warning:` / `error:` / `style:` / `performance:` in `src/*.h`, `src/*.cpp`, diff against the last clang-tidy baseline, triage findings against the cumulative diff (which files / lines have changed since the last green run), and either self-fix (mechanical, <5 LOC) or spawn a micro-cycle implementer per finding cluster.

### Historical context (no longer load-bearing)

The retired "Oracle-less cycle protocol" + 3-strike rule modeled the case where every cycle was **expected** to run clang-tidy and deferrals were exceptions. Three consecutive deferrals (T1 2026-04-28, T2+T3 attempts 2026-04-30 and 2026-05-02) — each individually sound under contention — surfaced that the per-cycle expectation itself was wrong on a header-heavy 10k-LOC codebase where clang-tidy is structurally a 1-2h task. The new policy treats batched clang-tidy as the design, not the exception; the 3-strike rule is removed because it modeled an exception case that no longer exists.

---

## Background Task Discipline

**Two-question pre-flight before EVERY backgrounded Bash call** (also stated in core; full details below cover the two recurring class-2 silent-success bugs):

1. **"Does my command body contain `&`, `nohup`, `( ... ) &`, or `cmd; touch sentinel &`?"** If yes AND you are about to set `run_in_background: true`, STOP — that is the double-background bug (rule #5). Pick exactly ONE backgrounding mechanism. If using `run_in_background: true`, the command must be foreground (no inner `&`).
2. **"Am I about to write `pgrep -f <token>` to check a process?"** If yes, STOP — `pgrep -f` is structurally banned for orchestrator-typed checks (rule #4); use sentinel file or `ps -ef`-with-shell-filter.

Both rules existed before recurrence (G1/G3 cycle 2026-04-24, provenance-plumbing cycle 2026-04-26 for #5; three escalating cycles for #4). The pre-flight banner is intended to fire BEFORE the typing reflex; the rule bodies below are the structural detail.

Wait for the completion notification on `run_in_background`. Do NOT poll partial logs, duplicate long tasks, or misread stale mtimes as current output.

1. **Tag every background task** with task-id, log path, launch timestamp. Before reading any `/tmp/fwiz-*.log`, check mtime vs launch timestamp — if mtime < launch, it's stale.
2. **Never start a duplicate long task** (make sanitize, make analyze) while another runs. The reliable check is a sentinel file (`[ -f /tmp/fwiz-analyze.running ]` — wrapper writes on launch, removes on exit) or `ps -ef | grep -E '<binaries>' | grep -v grep | grep -v zsh | grep -v bash` (the explicit shell-filter pattern; works even when the orchestrator's argv contains the binary name). Do NOT use `pgrep -f <token>` — anchored or not. Even a "unique tag" (`pgrep -f "fwiz_running_unique_tag_optC"`) self-matches because the moment the orchestrator types the tag into a Bash command, the tag becomes part of the orchestrator's own argv. Successive cycles (Cycle B 2026-04-25, Option C 2026-04-26) re-discovered this with anchored / unique-tag variants. The structural fix is: there is no `pgrep -f` form that the orchestrator can write that won't appear in its own argv. Use sentinel-file or `ps -ef`-with-shell-filter.
3. **Hung-task threshold**: 2x expected duration for *silent* hangs (no output). `make analyze-full` takes ~1-2h on a clean PC; not hung before ~3h silence. `make analyze-fast` takes ~1-2 min; not hung before ~5 min. **Oracle-contention escape (analyze-full only)**: if `analyze-full` etime exceeds 4x expected (~6h) AND `ps -o %cpu,comm ax | sort -rn -k1 | head -5` shows a non-fwiz process consuming > 200% CPU (system contention, not analyze itself), surface to the user: "analyze-full running 4x expected under {process} contention — kill and retry later? (yes/no)". Do not silently wait through extreme contention. Canonical miss: T1 cleanup cycle 2026-04-28 — clang-tidy ran 18.5h (25x expected) under llama-server at 546% CPU before the user proactively offered batch-defer.
4. **`pgrep -f` self-match is structural — `pgrep -f` is banned for orchestrator-written process checks.** Any string the orchestrator types into a Bash command becomes part of that command's argv; `pgrep -f <token>` therefore matches itself. This fires identically in (a) wait-loops (`while pgrep -f <name>; do sleep N; done` runs forever after task completes), (b) pre-launch existence checks (`pgrep -f clang-tidy` produces false-positive WARN), and (c) "unique-tag" anchored variants (`pgrep -f "unique_tag_optC"` — the tag is now in the orchestrator's own argv too). Three escalating cycles (derive-ordering wait-loop `2026-04-20`, Tier 1.x pre-launch `2026-04-25`, Option C unique-tag `2026-04-26`) confirm there is no `pgrep -f` form that survives. Use exactly ONE of: (a) `run_in_background: true` on the task itself (harness owns the wait, no watcher needed — strongly preferred); (b) PID captured at launch + `kill -0 $PID` to test liveness; (c) sentinel file written on launch / removed on exit + `[ -f /tmp/fwiz-analyze.running ]`; (d) `ps -ef | grep -E '<binary>' | grep -v grep | grep -v zsh | grep -v bash` (the wrapper-shell exclusion handles cases where the orchestrator's own command line is a `zsh -c ...`).
5. **Never double-background**: do NOT wrap `run_in_background: true` around a Bash command whose body itself contains `&` (or `nohup ... &`, or `( ... ) &`, or `cmd; touch sentinel &`). The harness's completion notification fires when the OUTER shell exits — and the outer shell exits as soon as it backgrounds the inner subshell, regardless of whether the long-running task has finished. Pick exactly ONE backgrounding mechanism: either (a) `run_in_background: true` on a foreground command (`make analyze 2>&1 | tee /tmp/log; touch /tmp/done`) — the harness owns the wait — or (b) a foreground shell with `nohup cmd &` and orchestrator polls a sentinel file via Bash with NO `run_in_background`. Never both. Canonical miss: G1/G3 simplifier-gap cycle 2026-04-24T10:35 — `make analyze` launched as `run_in_background: true` on a command containing `& touch sentinel`; harness fired completion ~immediately while clang-tidy was still running; orchestrator caught it via `pgrep` only because the log was suspiciously short. Recovery cost was zero (sentinel pattern already in place) but the near-miss is a class-2 bug (silent-success looks identical to real-success).

### Silent-run watchdog for `make analyze-full`

When the user is running the batch and the orchestrator is monitoring (or when the orchestrator is running it on user request), a clang-tidy run that produces zero output progress for ≥ 4× expected (~3h) is **suspect**, not just slow — clang-tidy does emit per-TU progress under contention, so total silence is a sign the wrapper script's output-redirection is broken, the process is stuck on a single TU, or contention has blocked all forward progress. At the 4× silent mark, kill and re-launch with explicit per-TU progress logging (`-j1 V=1` or wrapper `tee` to log per file completion) before assuming "still running." Canonical: T2+T3 attempt-2 ran 1+ day producing zero bytes of output before the user killed it on session resume; if a watchdog had killed it at 3h-silent and re-launched with progress logging, the cycle would have known whether the run was making progress or stuck.

### Duplicate-launch check for cppcheck (rare, but fast)

`ps -ef | grep -E 'cppcheck' | grep -v grep | grep -v zsh | grep -v bash`. Do NOT use `pgrep -f <token>` (rule #4 above).

---

## Cycle-Completion Checklist

Before declaring a cycle complete:

1. **No in-flight background tasks**: `ps aux | grep -E 'clang-tidy|cppcheck|make|fwiz'` — zero processes other than orchestrator.
2. **All logs final-state**: for each `/tmp/fwiz-*.log` cited in review-notes.md, mtime > last source-file mtime.
3. **Per-cycle residual audit (cppcheck)**: grep `warning:` / `error:` / `style:` / `performance:` in `/tmp/fwiz-analyze-fast.log` (or wherever `make analyze-fast` was teed) vs. cycle-start baseline. If delta non-zero OR any flag in an implementer-touched file/line, do NOT close — spawn a residual-fix pass (self-fix if trivial, implementer if not). cppcheck is the per-cycle oracle; grep is not.
4. **clang-tidy debt tracking** — the per-cycle gate is cppcheck only; clang-tidy debt is the principled trade-off — track and surface it gently, do NOT block.
   - At each cycle close, compute `cycles_since_clang_tidy`: count of `## NEW CYCLE` markers in `orchestrator-log.md` (active + archived) since the last commit annotated `clang-tidy: green` (or, if none yet, since `e37d0f6` — last green pre-T1 baseline).
   - Compute `cumulative_diff_lines`: `git diff --shortstat <last-green-commit>..HEAD` line count.
   - Append to `next-priorities.md` under a "## clang-tidy debt" heading: "{N} cycles unanalyzed since {commit}, cumulative diff {M} lines. User runs `make analyze-full` during next idle window."
   - When the user announces they'll run the batch, or when they ask "what should we run," surface the debt summary. No 3-strike escalation, no hard-stop — debt is visible and the user decides cadence.
   - When the batch DOES run and is clean, append `clang-tidy: green` to the cycle-close commit message; this resets the debt counter. If the batch surfaces findings, triage on the cumulative diff (which files / lines have changed since the last green); fix per-finding (self-fix mechanical, micro-cycle architectural).
5. **Artifact retention** — see §Artifact retention below.

---

## Artifact placement — gitignored `.fwiz-workflow/` vs committed `docs/`

Where an artifact lives is determined by its lifecycle, not the phase that created it.

- **`.fwiz-workflow/` (gitignored)**: per-cycle working artifacts consumed within the cycle and by the immediate-next cycle's RESEARCH phase — `research-brief.md`, `design-proposal.md`, `implementation-log.md`, `review-notes.md`, `next-priorities.md`, `orchestrator-log.md`, `meta-review*.md`, `workflow-metrics.md`, per-cycle scratch diagnostics. The orchestrator may rotate these (suffix-rename at next-cycle start) but the directory itself is disposable; if cleared, the cycle can still reconstruct from commits.
- **`docs/` (committed)**: user-facing reference docs (`Language.md`, `Solver.md`, `Developer.md`, etc.) — Title-Case at top level.
- **`docs/research/` (committed)**: long-lived research anchors referenced from committed sources (`docs/*.md`, `CLAUDE.md`, inline code comments, commit messages). Lowercase-with-hyphens (`docs/research/category-c-investigation.md`, `docs/research/provenance-plumbing.md`). Criterion: if two or more committed docs reference the artifact, OR if any cycle beyond the immediate-next expects to consume it, OR if a commit message points to it, it belongs here.

Rule of thumb when authoring an investigation or research-anchor artifact mid-cycle: if you find yourself adding a reference to it from `docs/Future.md`, `docs/Known-Issues.md`, `docs/Developer.md`, or `CLAUDE.md`, place the artifact in `docs/research/` from the start. Avoids the post-hoc move that the reviewer catches. Canonical miss: P1-tautology cycle — `category-c-investigation.md` was first written to `.fwiz-workflow/`, referenced from `docs/Future.md #32` and `docs/Known-Issues.md #7`; reviewer flagged the discoverability risk; orchestrator moved it to `docs/`, then to `docs/research/` (2026-04-26) once a second research anchor (provenance-plumbing) made the subdirectory worth establishing.

---

## Artifact retention

Count suffixed artifacts in `.fwiz-workflow/` (`research-*.md`, `design-*.md`, `implementation-log-*.md`, `review-notes-*.md`, `next-priorities-*.md`, `meta-review-*.md`). If > 15, archive the oldest cycle into `.fwiz-workflow/archive/{cycle-name}/`, keeping only the meta-review at top level.

`orchestrator-log.md` retention: cumulative by default, but rotate when the file exceeds **~1500 lines or ~150 KB**. Rotation procedure: at end of cycle, move all cycle-closed entries (everything strictly before the most recent `## NEW CYCLE` or `### [...] CYCLE START` marker) to `archive/orchestrator-log-thru-{YYYY-MM-DD}.md`; keep the active cycle's entries in `orchestrator-log.md`. Rationale: the file is read in full by every meta-review and consumes ~30k tokens at 1500 lines — beyond that it dominates initial context cost. Option C cycle 2026-04-26 first hit this: file was 1192 lines / 147 KB at cycle start; Day 2 entries weren't logged at all, suggesting the orchestrator silently avoided the cumulative-write path under context pressure.
