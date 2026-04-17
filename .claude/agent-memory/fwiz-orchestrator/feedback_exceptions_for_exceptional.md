---
name: Exceptions only for truly exceptional cases
description: try/catch is reserved for exceptional/unrecoverable conditions, NOT for expected failure cases that are part of normal control flow. In fwiz most "failures" are expected.
type: feedback
---

Rule: **Exceptions (try/catch) are only for genuinely exceptional/unrecoverable conditions. If failure is a normal, expected outcome of the operation, do not use exceptions — return `std::optional<T>` or an explicit error enum instead.**

**Why:** The user's framing: in other software, dividing by zero is typically a bug — user error or software error — and an exception is appropriate. In fwiz, dividing by zero happens during normal numeric probing and candidate enumeration; it is an expected negative result, not an error. Treating expected failures as exceptions carries two costs:

1. **Semantic dishonesty** — the reader has to infer "oh, this isn't actually exceptional, it's just a probe that failed." `std::optional` says exactly what it means.
2. **Performance** — exception unwinding is ~160 µs per throw (benchmarked, research-milestone-E.md). On the numeric grid scan that runs 200+ times per solve, even 1% probe failure rate means exception overhead dominates the solver.

**How to apply:**

- When designing a new function that can fail, first ask: **is the failure expected in normal use, or is it a bug/misuse?**
  - Expected failure → return `std::optional<T>` (or a sentinel, or an out-param + bool).
  - Genuinely exceptional → throw. Examples: out-of-memory, invariant violation, a parser hitting a malformed token the caller promised was valid.
- When reviewing existing code using `try { X } catch (...) { }`, ask: is the catch block discarding an expected failure or a genuine exception? If expected, that's a refactor candidate (`_opt` wrapper or full rewrite).
- Fwiz's convention: `evaluate()` and `resolve()` still throw on failure for historical reasons and because the top-level CLI catches. Prefer adding `_opt` non-throwing overloads rather than callers wrapping in try/catch. Tier-based refactor is documented in `.fwiz-workflow/research-milestone-E.md`.
- The `try { parse_condition(s); } catch (...) {}` pattern at 24 sites in fwiz is the canonical violation of this rule — failure there is expected (malformed input during best-effort probing) and the catch block silently swallows it. All 24 sites are being refactored in the ongoing tech-debt cycle (Milestone E).
