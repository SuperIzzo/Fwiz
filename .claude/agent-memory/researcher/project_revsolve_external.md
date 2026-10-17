---
name: revsolve-external
description: External prior-art for numeric reverse-solve / design inversion (F1 continuous root-find + F2 integer threshold search)
metadata:
  type: project
---

F1 (numeric root-find fallback) and F2 (integer monotonic threshold search) for fwiz.
Motivated by PNF hypergeometric affordability model inversion (K, r, h as design levers).

**Why:** F2 is the higher-value capability (daily use; currently emulated by max_r.sh shell sweep). F1 is more general but less urgent.

Key findings:

- No CAS (SymPy, Mathematica, Maxima, Maple) auto-falls-back from symbolic to numeric. All require an explicit user action (nsolve/FindRoot/fsolve). Fwiz's "symbolic-first, numeric-fallback, flag ~" precedent (from definite integral / adaptive_simpson) is more ergonomic than any CAS surveyed.
- For F1: existing Strategy 6 + find_numeric_roots infrastructure may already handle K=? after aggregation unroll. Run the reproducer FIRST before designing.
- For F1 root-finder algorithm: newton_solve -> bisection_solve chain in find_numeric_roots is adequate. Brent's method is the gold standard but the existing chain is functionally equivalent for smooth aggregation functions. No new algorithm needed.
- For F2: classical "binary search on the answer" pattern (competitive programming). For n ≤ 20 (PNF case), linear sweep is simpler and produces a full sweep table as a bonus. Add binary search path if n > 1000 is ever needed.
- F2 requires new CLI syntax: r=?max / r=?min (optimisation direction) + out_var >= threshold (comparator constraint). No CAS prior art for this syntax.
- Approximate signaling: fwiz's ~ flag is better than any CAS (all use float return type only).

Brief at: .fwiz-workflow/research-brief-revsolve-external.md

**How to apply:** Before designing F1, run the reproducer. Before designing F2, clarify whether the CLI comparator goes on the input (r=?max in [1..h]) or output (can_pay >= 0.40) side, and whether global conditions provide the search domain implicitly.
