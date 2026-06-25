---
name: project-combinatorics-aggregation
description: Internal substrate research for combinatorics + bounded aggregation arc (PNF card-game use case). Covers P0a/P0b scope, overflow analysis, parse_range reuse verdict, and roadmap fit.
metadata:
  type: project
---

Arc driven by PNF card-game design-inversion use case. Brief at `.fwiz-workflow/research-brief-internal.md`.

**Key findings:**

1. **P0b (bignum) is NOT a blocker for PNF.** All C(n,k) for n<=54 fit in double exactly (max C(54,27)=1.95e15 < 2^53=9.0e15). The proposal's claim that "choose(52,k) overflows double" is false for all k. The only real overflow risk is naive factorial materialization (n! for n>=19 overflows double) — but int64-backed accumulation (same as existing `make_rational`) handles up to 20! and the safe alternating-mul/div nCr avoids materializing n! entirely. P0b remains Future #17 (planned, unscheduled) for crypto/number-theory but is NOT P0 for this arc.

2. **parse_range (system.h:4712) is NOT reusable directly for in-expression aggregation.** It materializes a `vector<double>` at CLI-parse time; `sum(i, 1..n)` must fold at simplify-time with bindings available. The grammar (start..stop @ step) and the idea transfer; the implementation doesn't.

3. **The vec/mat FUNC_CALL pattern is the exact structural analogy for sum/product.** No new ExprType, no sizeof(Expr) change. FUNC_CALL nodes named "sum"/"product" intercepted in simplify_once's FUNC_CALL branch before the all_num check (expr.h:2779). evaluate() returns empty (multi-arg FUNC_CALL, line 1305). Accumulation via make_rational for integer inputs.

4. **Reverse-solve substrate: inversion is FREE for static ranges.** When both range bounds are numeric, the aggregation folds to a plain rational at simplify-time → existing algebraic solver inverts without new work. Hard case (solving for range bound `n` in `product(i,1..n)=X`) requires Strategy 6 numeric extension — flagged as a deferred question.

**No existing Future item for P0a (bounded aggregation as evaluable builtin).** Nearest is #5b (parked, about vec materialization from ranges, not reduction). #5b's "second consumer" trigger fires with P0a — they share range grammar but serve different purposes.

**Roadmap:** closes/advances stdlib `combinatorics/permutations.fw` (Future.md:472-473) and `probability/expected_value.fw` (Future.md:477-478). Fires #80 (@include) reopen trigger. Does NOT close #94 (factorial reverse-solve) or #17 (bignum).

**Why:** PNF card-game design-inversion requires closed-form combinatorics (not recursion) for cheap bidirectional solve. Recursive factorial/nCr work forward but are NP-hard to invert.
