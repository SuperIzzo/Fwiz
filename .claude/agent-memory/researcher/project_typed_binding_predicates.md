---
name: Typed-binding predicates Future #53
description: Research for #53 — is_num/is_neg_num/is_int/is_pos_num predicate extension to .fw rule conditions; 4 consumers, Approach C recommended
type: project
---

Cycle #53 typed-binding-predicate research, 2026-05-10.

4 consumers audited:
- T3.5 (`simplify_div` constant reassociation, expr.h:2021-2044): PARTIAL — needs `is_int(k) && is_int(m)` but also rule-RHS rational arithmetic (secondary blocker, Future #54)
- T3.6 (`x^(-n)` rendering, expr.h:2471-2477): STRAIGHTFORWARD — `is_neg_num(n)` alone unlocks migration
- #31 (`abs(x) = x iff x >= 0`): PARTIAL — `is_pos_num(x)` unlocks numeric-literal case; full symbolic `known(x>=0)` is a separate domain-propagation feature
- Integration Tier 1 / BuiltinMeta (expr.h:2985-3005 POW case + builtin_meta() registry): PARTIAL — needs `is_num(n)` AND a mechanism for the rule to reference the integration variable (secondary blocker)

Recommended approach: Approach C (predicate FUNC_CALLs in condition clauses, e.g. `iff is_neg_num(n)`). Parser reads a FUNC_CALL-shaped clause as a CondPredicate instead of a binary CondClause. Evaluated against the `ExprPtr` binding directly (not the `double` numeric map). Fail-safe semantics (unknown = false, not permissive-true).

Key open question #7: `compute_rewrite_groups` exhaustiveness check uses `to_valueset`; predicate clauses don't produce ValueSets — must be handled specially.

Brief at `.fwiz-workflow/research-brief.md`.

**Why:** Fail-safe semantics for type predicates is the unanimous CAS choice (Mathematica/SymPy/Maple/Maxima all fail on unknown). Fwiz's current permissive-true is only safe for comparison clauses (where unknown x means "don't block the rule"); it is unsound for type predicates (where unknown means "definitely not a numeric literal").
