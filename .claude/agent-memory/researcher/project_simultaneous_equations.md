---
name: Simultaneous equations research
description: Deep architectural analysis of multi-equation solving — where variable elimination plugs in, what infrastructure exists, what's missing. Updated with external CAS research.
type: project
---

Researched April 2026. Covers system.h, expr.h, KNOWN_ISSUES, FUTURE, and external CAS algorithms.

## Internal architecture state

**Key finding:** Cross-equation validation already exists in `resolve_all()`. It filters candidates against all equations when `known_lhs_count > 1`. What's missing is *forward substitution* — deriving one variable from one equation and substituting it into another before solving. That would be a new strategy in `enumerate_candidates()`.

**Why:** Rectangle puzzle (`area=12, perimeter=14 → w=?`) currently stack-overflows in the system-probe numeric path because the 2D search space is unbounded.

**How to apply:** Any new elimination strategy must fit into `enumerate_candidates()` as a new `CandidateType`, handled in `solve_recursive`, `solve_all`, `derive_recursive`, and `verify_variable`.

## What external CAS research found

**SymPy nonlinsolve:** Sorts equations by fewest variables first, solves fewest-variable equation with solveset, substitutes solutions into remaining equations, validates with checksol (three-way: True/False/None). Uses Gröbner basis only to *classify* the system (inconsistent / zero-dim / positive-dim) before choosing a method.

**Maxima algsys:** Selects variable with lowest nonzero degree for elimination, computes resultant of that variable across all equation pairs to get a reduced system, recurses until univariate, then calls solve. Falls back to `realroots` (numeric) if exact solution is unavailable.

**Mathematica Solve:** Gröbner basis (Buchberger or GroebnerWalk) followed by eigenvalue extraction from multiplication matrix, then back-substitution. Variable ordering heuristic from Boege-Gebauer-Kredel 1986 for lex order efficiency.

**Consensus heuristic for variable selection:**
1. Prefer variables that appear linearly in at least one equation (degree 1) — isolating them produces no expression swell
2. Among those, prefer the equation with the fewest total variables (simplest to isolate from)
3. As a tiebreak, prefer lowest polynomial degree

**Expression swell:** Only a concern for large systems (5+ equations) or high-degree polynomials. For 2–3 equation systems (the Fwiz use case), direct substitution does not explode.

**Solution validation pattern (from SymPy checksol):** After finding candidates, substitute back into ALL original equations and check residual ≈ 0 within floating-point tolerance. Three outcomes: exact match, approximate match (mark as `~`), fail (discard). This is exactly what Fwiz's existing verify mechanism does — it can be reused.

**Underdetermined systems:** If fewer equations than unknowns after variable assignment, the system has a free variable — output is parametric or a ValueSet range. Fwiz's existing ValueSet handles this naturally if the solver emits it.

**Inconsistent systems:** After all substitution paths are exhausted and no candidate survives back-validation, report "no solution satisfying all equations". Detect early if any equation evaluates to `0 = nonzero constant` after substitution.
