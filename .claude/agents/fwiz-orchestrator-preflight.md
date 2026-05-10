# Fwiz Orchestrator — Pre-flight Protocols

This file holds **pre-flight** protocols that fire BEFORE implementer spawn — toolchain checks, surface-contract audits, test-site flagging, domain-constraint scans. Each fires < 1/cycle on average; split off from `fwiz-orchestrator-protocols.md` to reduce initial-context cost per orchestrator spawn (the orchestrator reads this file on demand only when the trigger applies).

Sibling files: `fwiz-orchestrator.md` (core), `fwiz-orchestrator-protocols.md` (other conditional protocols), `fwiz-orchestrator-ops.md` (background tasks, artifact lifecycle, oracle policy).

---

## Instruction-vs-recent-commit collision check (session start)

Trigger: user's first instruction in a fresh session names a specific cycle slug, milestone, or feature ("Run Cycle N (slug)", "Implement {feature}").

Before doing anything else, run `git log --oneline -5` and check whether the named item appears in any of those commit subjects. If yes, STOP — the instruction is likely stale (copy-pasted from an old `next-priorities.md` recommendation that has since shipped). Surface the collision: "{item} appears to have shipped in commit {hash}. Working tree state is X. Possible meanings: (a) you want me to redo it, (b) you want the next item from the audit roadmap, (c) you're checking on its state. Which?"

Cheaper than 10+ minutes of working-tree forensics on the wrong assumption. Canonical miss: 2026-05-09 — user typed "Run Cycle 8 (T7 — libFuzzer harness)" stale from the 2026-05-07 next-priorities text; Cycle 8 had already shipped as `4252b25` two days earlier; orchestrator detected at the working-tree-inspection stage but only after several tool calls that this pre-flight would have short-circuited.

---

## Pre-flight verification — new-infrastructure cycles

Trigger: cycle introduces NEW build/runtime infrastructure (a new binary target, a new harness, a new toolchain entry point).

Run an orchestrator-side pre-flight BEFORE spawning the implementer. **Four checks**:

1. **Toolchain availability** — the required compiler/linker/library is on `$PATH` and produces the expected version.
2. **Linkage probe** — a one-liner `clang++ ... -o /tmp/dummy.x` (or equivalent) that confirms the new sanitizer/runtime/library link-step works (e.g. `-fsanitize=fuzzer` finds `libclang_rt.fuzzer-*.a` — link-test produces the *expected* symbol error, not an unexpected toolchain error).
3. **Surface-contract audit** — grep the existing source for behaviors the new harness assumes (e.g. parser uses `exit()`/`assert()`? throws? prints to stderr?).
4. **API name verification** — for every API the design brief names, grep the actual source for the symbol (the design's `load_from_string` was actually `load_string`).

The pre-flight runs at CYCLE START, AFTER the 4-field check and BEFORE implementer spawn; findings are appended to the implementer brief as corrections (not deferred to BLOCKED reports).

Validates: Cycle 8 T7 libFuzzer harness 2026-05-07 — 4-check pre-flight caught the `load_from_string` → `load_string` API mismatch in the design brief; implementer ran 0-block on the corrected brief. Without the pre-flight the implementer would have hit the typo at compile time and spent a self-fix round on it. Single canonical-positive instance — encoded because procedural (sequencing + checklist, not anecdotal), and the pattern is the analog of `Pre-flight test-site flagging` for contract-changing migrations.

---

## Pre-flight test-site flagging

Trigger: spawning the implementer for a contract-changing migration (return type, exception shape, `.value()` vs `operator*`, etc.).

Scan `src/tests.cpp` for sites whose assertion style depends on the OLD contract — tests catching `std::bad_optional_access`, relying on `operator*` throwing vs. `.value()` asserting, checking `std::isnan` via `*opt`. List these sites in the implementer brief with the exact rewrite. Without this, the implementer wastes a cycle on "harness mismatch or real bug?" (Validated: Checked<T> cycle — see `7095f95`, `e65e1fe`.)

Note: contract-changing migrations also require the critic-accepted/rejected items list to be echoed into `review-notes*.md` so the reviewer validates design fidelity (did the implementation honor each decision?), not just code quality.

---

## Pre-flight raw-token-walker scan (new-token introductions)

Trigger: cycle introduces new `TokenType` members (`LBRACKET`, `LBRACE`, etc.) AND any non-lexer source file walks tokens raw.

Run: `grep -nE 'TokenType::(LPAREN|RPAREN|LBRACKET|RBRACKET|LBRACE|RBRACE)' src/*.h | grep -v lexer.h`

Every hit is a potential consumer of the new token's symmetry that may need updating. List hits in the implementer brief with the **bracket-symmetry invariant**: "if your function tracks one bracket pair's depth, it must track all bracket pairs the lexer can emit." Without this, an implementer landing new tokens will miss raw-token-walkers that look correct in isolation but truncate / mis-parse symmetrically (e.g. a paren-only depth scanner that breaks on the first inner COMMA when a vec literal appears).

Validates: Cycle B M3 vec/mat 2026-05-10 — `parse_call_args` (`system.h:2163-2168`) tracked `LPAREN`/`RPAREN` only; reviewer caught at REVIEW with concrete reproducer `f(v=[1,2,3], result=?)` truncating at the first inner `,`. Pre-flight grep would have surfaced the consumer at IMPLEMENT spawn time.

---

## Pre-flight structural-claim verification (design synthesis)

Trigger: Final Design synthesis contains a "naturally skips / no parent-side change required / existing infrastructure handles transparently" structural claim that piggybacks on consumer behavior the design has NOT individually audited. Common shapes: "FUNC_CALL-in-lhs naturally skipped by `is_var()` checks", "additive overload preserves N call sites", "new sentinel propagates through existing flatten passes unchanged".

Run an orchestrator-side `grep -nE` for every consumer of the channel the claim relies on, BEFORE spawning the implementer. For each hit: classify as (a) **natural skip** — existing predicate/type-check makes the consumer transparent to the new shape (e.g. `is_var()` returns false for FUNC_CALL), (b) **needs explicit branch** — consumer must be updated, (c) **ambiguous** — consumer may interact, defer to implementer's discovery. Append the hit-list to the implementer brief as a "Structural-claim verification" section: "Claim X audited at design synthesis — N hits, M natural skips, K need explicit branches: {list}. Cite these when extending."

The critic-side rule "Cite a verified call site when proposing a structural change" (critic.md) is the agent-side analog; this orchestrator-side companion is the safety net when the critic's verdict is structurally right but stops at "verify the claim" rather than enumerating each consumer. Saves implementer ~15-25 LOC of defensive coding (each transparent consumer is one less guard the implementer feels obligated to add).

Validates: Cycle 2026-05-10 Future #53 — orchestrator grep'd 5 consumers of `CondClause.lhs` before declaring FUNC_CALL-in-lhs encoding safe; 3 of 5 (`to_valueset` lines 1717+1722, `compute_rewrite_groups` system.h:752) naturally skipped via existing `is_var()` checks. The 2 hits needing explicit branches (`check_condition` dispatch, `condition_to_string` output) were the ONLY ones the implementer touched; production diff came in at 51 LOC against a 70-LOC budget. Without the pre-flight, the implementer would have either over-guarded (defensive checks at every consumer) or under-guarded (missed one). One critic adjacent positive instance — encoded because procedural (sequencing + grep checklist, not anecdotal) and the pattern is the design-synthesis analog of consumer-enumeration on contract-changing migrations.

---

## Domain-sensitive test data

Trigger: design specifies numeric test points (fingerprint probes, property-based sampling, numeric-solver seeds).

Scan the user's reproducer for implicit domain constraints BEFORE the design is locked: triangle inequalities, positivity, monotonicity, branch-cut regions, unit-box constraints. If the design's test-point formula is a generic scheme (prime cycling, uniform sampling), spot-check it against the reproducer's bindings on paper — do two or three points land in-domain?

When implementer reports BLOCKED with "all candidates NaN / domain-violating at test points" as the failure pattern, the fix is a test-data change, NOT a dedup / algorithm change. Orchestrator self-fix is appropriate here even if the delta is >5 LOC, because the change is a constant-choice correction under a design invariant the domain-scan should have caught. Log the miss so the next cycle's DESIGN phase can spot it earlier. Canonical miss: derive-dedup cycle M3-6 — multiplicative prime scheme (b=10,c=6) violated triangle inequality at a=4; self-fix switched to per-variable cycling (b=2,c=3), ~5 LOC, mechanical once the domain constraint surfaced, but required triangle-inequality judgment to notice.
