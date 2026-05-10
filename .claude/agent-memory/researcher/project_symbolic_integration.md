---
name: Symbolic Integration Research
description: CAS survey (SymPy/Maxima/Mathematica/Maple/SageMath) for integral(f,x) arc — dispatch ladders, rule taxonomy, u-sub detection, IBP, numeric fallback
type: project
---

Fwiz symbolic integration arc research completed. Brief at `.fwiz-workflow/research-brief-integration.md`.

**Why:** `diff(f,x)` shipped; `integral(f,x)` is the next logical extension. Symbolic integration has no universal algorithm — prior-art survey drives design.

**Key findings:**
- All 5 CAS use the same tiered ladder: rule-table → substitution (derivative-divides) → IBP → Risch → unevaluated fallback. Never error on failure.
- SymPy `manualintegrate` has ~25 named Rule subclasses; dispatch is a DFS over typed rules, NOT per-AST-class switch. Per-AST-class dispatch (like `symbolic_diff`) is insufficient because integration rules span multiple AST levels simultaneously.
- Derivative-divides is the primary u-sub detection: enumerate subexpressions g(x), compute g'(x) via `symbolic_diff`, check if quotient simplifies to function of g alone.
- LIATE priority (Log→InvTrig→Algebraic→Trig→Exp) is the standard IBP u-selection heuristic across all CAS.
- Risch algorithm: complete only for transcendental case; no CAS has full algebraic case. NOT recommended for Fwiz first cycle.
- SymPy omits `+ C`; Mathematica/Maxima add it. Fwiz should omit (cleaner derive output).
- SageMath is just a multi-backend dispatcher (Maxima default → Giac fallback). Adds no integration logic of its own.
- Rubi (Mathematica companion): 6700+ rules in 9 categories; decision-tree; 72k test suite. Too large for Fwiz but validates rule-first approach.
- ~35-45 atomic rules cover school/physics/engineering use cases (polynomial, trig, exp, log, inverse trig, hyperbolic, rational forms).
- Numeric fallback for definite integrals: adaptive Simpson's is the minimal viable choice; 5-point Gauss-Legendre is more efficient for smooth functions (static weight table fits Fwiz's static-data aesthetic).

**Recommended Fwiz strategy:**
- Tier 1: ~35 static rules (C++ if-chain in `symbolic_integrate`, mirroring `symbolic_diff`)
- Tier 2: derivative-divides substitution (uses existing `symbolic_diff`)
- Tier 3: IBP / LIATE (recursive, algebraic)
- Tier 4: unevaluated `integral(f,x)` FUNC_CALL node + adaptive Simpson's for definite integrals
- Stop before Risch. Extensible via `.fw` rewrite rules for parametric patterns if condition parser supports wildcard numeric constants.

**Open questions for planner:** (1) can `.fw` rewrite rules express parametric integrals with numeric-wildcard conditions? (2) integral as FUNC_CALL vs new ExprType? (3) definite integral 4-arg syntax. (4) `log(abs(x))` vs `log(x)` for ∫1/x.

**How to apply:** When planning the integration arc, reference this brief. First cycle scope: Tier 1 + Tier 4 (static rules + unevaluated fallback + numeric definite). Tiers 2+3 (derivative-divides + IBP) are the follow-up cycle.
