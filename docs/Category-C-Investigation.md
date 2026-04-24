# Category C Investigation — Derive Output Tautology Beyond sqrt^2

**Status**: open investigation (not a design or plan)
**Created**: 2026-04-20, at the close of the P1 tautology cycle
**Premise**: the P1 cycle successfully added `sqrt(x)^2 = x iff x >= 0` to builtin rewrite rules, but the predicted cascade into Category C (143/159 tautological lines) did NOT materialize. This document records what we know and what we SPECULATE, separating the two, so the next cycle investigates rather than presumes.

## What we empirically know (confirmed, not speculation)

1. **The `sqrt(x)^2 = x iff x >= 0` rule fires correctly.** 14 pre-rule occurrences of `sqrt(.*)^2` → 0 post-rule. 16 of 159 lines changed form.

2. **Line count is unchanged.** 159 → 159. Total chars: 42689 → 42399 (−0.68%).

3. **The design's "fingerprint cascade" hypothesis was wrong.** Simplified forms like `A = acos((2c²−7.518c)/(2·sqrt(c²−7.518c+16)·c))/deg` depend on `c` alone; canonical forms like `A = acos((b²+c²−16)/(2bc))/deg` depend on `b` AND `c`. They have different free-variable signatures and thus different fingerprint vectors — the existing dedup infrastructure correctly treats them as distinct candidates.

4. **Category A dup still present.** Line 5 (`A = 1/deg * acos((b²+c²−16)/(2bc))`) and Line 6 (`A = acos((b²+c²−16)/(2bc))/deg`) are still two separate lines post-M1. Formatter-artifact duplication is orthogonal to M1.

5. **The 143-line statistic from the original research brief has not been re-verified post-M1.** We don't know how many lines post-M1 contain the `acos((b²+c²−16)/(2bc))` subexpression. It may be 143 still, or slightly fewer (if sqrt^2 was nested inside that subexpression for some candidates), or different depending on parse direction.

## What we think we know but haven't measured (worth confirming)

6. Each of the 143 "Category C" lines is a *distinct functional form* — not a duplicate of another line. Enumeration produces them via legitimate different solving strategies (law-of-cosines, angle sum, law-of-sines inversion, cross-equation elimination) and each has a different tree shape. They are NOT algebraically-equivalent-but-differently-written duplicates; they are algebraically-equivalent-after-substitution-identities-we-have-not-encoded.

7. The output is "tautological" in the user-facing sense (they all compute A given the same inputs) but NOT in the simplifier sense (each is a legitimate formula relating A to free variables via different intermediate quantities).

## Speculations on why this is hard (explicitly speculative)

S1. **The real equivalence is modulo triangle identities, not algebraic identity.** Law of cosines + angle sum form a closed system: given any two angles you can derive the third; given any two sides and an angle you can derive the third side. The 143 candidates chase these circular dependencies. The simplifier CANNOT detect "A = 160 − C where C = 180 − A − B = 180 − A − 20" reduces to `A = A`, because that reduction requires substituting the formula for C that depends on A that depends on C... The fixed-point is correct (`A = A`) but the simplifier would need to do a self-consistency check, not just substitution. **Speculation**: the mathematically-complete fix requires Gröbner basis or algebraic-closure machinery — way outside fwiz's scope.

S2. **Most 143 lines are valid derivations of different "aspects" of A.** `A = asin(1.368/b)/deg` (3 leaves) is the shortest derivation — law of sines. `A = acos((b²+c²−16)/(2bc))/deg` is the law of cosines — 10+ leaves. `A = 180 − B − C = 160 − C` where `C = acos(...)/deg` — another valid route. These aren't duplicates; they're *different theorems* instantiated for this triangle. **Speculation**: a "tautology" in user perception is actually a "choice among alternative formulations" in the formal system. Users might want ONE chosen form, not elimination of alternatives.

S3. **The user-visible complaint is noise-to-signal ratio, not correctness.** 159 valid formulas is overwhelming; 3–5 chosen well would be useful. **Speculation**: the right fix may be a user-visible selector (`--derive N=3` picks the three shortest by some metric), not architectural pruning. The previous cycle already shipped `--derive N` — so the user-visible tool EXISTS. What's missing is *sensible defaults* (e.g., default to top 10 by canonicity_score unless user asks for more).

## Candidate approaches to explore (marked as SPECULATIVE)

A list of ideas, each with "what would make this right" and "what would make this wrong" — so we explore rather than presume any one is correct.

### Approach 1 — Leaf-count gate relative to canonical
**Sketch**: after ranking, reject candidates whose leaf-count > K × canonical leaf-count. E.g., K=10 → if canonical has 3 leaves, reject candidates with > 30 leaves.
**Why it might be right**: simple, cheap, correlates with user's noise-to-signal complaint. No new abstractions.
**Why it might be wrong**: arbitrary K (magic number). May discard legitimately-long-but-informative candidates (e.g., a formula that uses intermediate geometric quantities the user wants to see).
**Risk**: false-prune of useful content; user may have wanted the law-of-cosines form even though it's 3× longer than law-of-sines.
**Cost**: ~5 LOC + 3 tests. Gate K is the bike-shed.

### Approach 2 — Default `--derive N` cap
**Sketch**: default to emitting top 10 (or 20) results sorted by canonicity_score. User opts into more with `--derive 1000` or similar.
**Why it might be right**: honors existing infrastructure (--derive N shipped in prior cycle). Makes tautology a presentation issue, not a solving issue.
**Why it might be wrong**: hides valid derivations behind a flag. User may not know to increase N. Differs from SymPy/Mathematica defaults (they emit all alternatives).
**Risk**: user surprises on complex problems where the N'th result is the one they needed.
**Cost**: ~2 LOC (change default in main.cpp). No new tests — existing `--derive N` tests cover it.

### Approach 3 — Ancestor-provenance guard during expansion
**Sketch**: during `derive_recursive`, track which equations were used to reach each candidate. If a candidate's provenance includes both "used equation E_i" and "expanded variable v_j via E_i", flag as cyclic and prune.
**Why it might be right**: addresses the root cause (cyclic substitution through triangle identities). Not a filter, a check at enumeration time.
**Why it might be wrong**: significant code change. May miss cycles that emerge only after multi-step substitution.
**Risk**: correctness bug in cycle detection — mis-pruning legitimate candidates.
**Cost**: 30–60 LOC + 5–10 tests. Architectural.

### Approach 4 — Canonicity-score-based soft cull
**Sketch**: emit all candidates whose canonicity score is within M × the canonical winner's score. E.g., M=2 → if canonical is 3 leaves, emit up to 6-leaf candidates; prune the rest.
**Why it might be right**: similar to Approach 1 but scaled to problem size (M is relative, not absolute). More adaptive than fixed K.
**Why it might be wrong**: still a magic number (M). Doesn't explain what M=2 vs M=3 means mathematically.
**Risk**: same as Approach 1 but dialed down.
**Cost**: ~8 LOC + 3 tests.

### Approach 5 — Semantic equivalence via Gröbner / algebraic closure
**Sketch**: use polynomial ideal membership to prove two candidates are algebraically equal modulo the system's equations.
**Why it might be right**: mathematically complete. Would truly identify the 143 "tautological" lines as one equivalence class.
**Why it might be wrong**: wildly outside fwiz's scope (tiny fast core, no external deps). Would require a linear algebra library or a full algebraic closure implementation.
**Risk**: feature creep at the scale of "replace the solver with SymPy."
**Cost**: thousands of LOC. Almost certainly rejected by the minimalism principle.

### Approach 6 — User-selectable strategy filter
**Sketch**: `--derive --strategy=lawsines` or similar — user picks which solving strategy produces the derivation. Default strategies produce 3–5 forms; extra strategies opt-in.
**Why it might be right**: maps user intent onto the enumeration strategy. Fits fwiz's existing CLI model.
**Why it might be wrong**: exposes internal strategy enumeration as a user-facing axis. Adds spec surface.
**Risk**: lock-in to current strategy enumeration (harder to refactor later).
**Cost**: 15 LOC in main.cpp + strategy tags throughout enumerate_candidates + tests.

## Diagnostic questions the next cycle should answer FIRST

Before picking ANY approach above, the next cycle should answer:

**D1**. How many of the 159 post-M1 lines are *leaf-minimal* (meet a "this is the shortest form of its equivalence class" test)? Among the 143 suspected-Category-C lines, what fraction would any of Approaches 1/4 prune?

**D2**. What's the leaf-count distribution of the 159 lines? Plot (or list) the histogram. Bimodal (short + long, clean split) vs. unimodal (continuous) determines whether a threshold-based approach can work at all.

**D3**. Are the 143 lines actually different strategies, or are many of them the same strategy with different intermediate substitution orders? Instrument `enumerate_candidates` (gated by env var, like `fwiz_trace_solver`) to log "strategy S_i produced candidate C_j". Then we'll know if the problem is over-enumeration within one strategy or over-enumeration across strategies.

**D4**. What does the user ACTUALLY want? 3–5 compact forms, or all 159, or "top-N by readability"? This is a product question, not an implementation question, and should be clarified before designing.

**D5**. Does SymPy's `solve` or Maxima's `solve` exhibit similar over-enumeration on this reproducer? If yes, the problem is inherent to symbolic derivation; our job is presentation. If no, investigate why their dedup is stronger.

## Lessons from the P1 cycle that should carry forward

L1. **A BLOCKING criterion must be invariant-derived, not hypothesis-derived.** "Line count < 100" was predicted by a cascade hypothesis. The hypothesis was wrong. The criterion should have been "no `sqrt(...)^2` substring in output" (invariant, shipped correctly). Orchestrator should resist encoding numeric thresholds as BLOCKING unless the number is itself an invariant.

L2. **"Simplification over filtration" can fail.** The P1 hypothesis was that a simplification rule would eliminate a tautology class. Empirically the simplification was correct but not deduplicating — the simplified candidates had different free-variable signatures than canonical ones. The "simplification over filtration" principle remains valid (prefer simpler primitives) but the cycle-close check must be "did output actually shrink?" not "did we ship the rule?"

L3. **Don't presume the research brief is right.** The "143 lines" statistic needs re-verification post-M1. Future cycles should re-measure the problem before fixing it, especially after a partial fix.

## What we should NOT do in the next cycle

- Don't pre-design an architectural guard without answering D1–D5 first.
- Don't propose a magic-number threshold (K for Approach 1, M for Approach 4, N for Approach 2) without articulating what the number means mathematically.
- Don't claim "cascade" effects without empirical measurement post-fix.
- Don't assume line count is the right metric. Total chars, max line length, mean canonicity score, and "distinct functional forms" are all candidates. Pick based on user complaint, not convenience.

## Recommended shape of the next cycle

1. Diagnostic round FIRST (answer D1, D2, D3). Budget: 1 cycle of research.
2. Clarify user intent (D4) — is this a product question the user wants to answer, or should we infer?
3. Compare against SymPy/Maxima (D5) — even one reproducer test helps calibrate.
4. THEN design, with a specific approach chosen from Approaches 1–6 (or a hybrid) backed by the diagnostic data.

Explicit acceptance: the next cycle might conclude "Approach 2 (default `--derive 10`) is correct; no architectural change needed." That would be a valid outcome even though it feels anticlimactic.
