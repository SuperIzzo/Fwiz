---
name: collections-aggregation-internal
description: Internal substrate map for gen-6 continuation arc — Collections & first-class aggregation (map/fold/{}). Coexist mechanics, {} token gaps, fold_aggregate policy table, reverse-solve keystone, fingerprint_expr oracle limits.
metadata:
  type: project
---

Collections & first-class aggregation (gen-6 continuation) — internal substrate research.
Brief at `.fwiz-workflow/research-brief-internal.md`. Date: 2026-06-23.

**Why:** arc introduces `map` + `fold` primitives + first-class `{}` collections + proves six reducers equivalent to their C++ fast-paths.

**Key findings:**

- `fold_aggregate` (expr.h:2876) is the single fold-policy table for all 6 reducers. Two paths share it: simplify-time (Path A, expr.h:2934) and post-load (Path B, system.h:1572). Path A fires BEFORE any .fw section lookup.
- Coexist is structurally sound: `is_aggregate_reducer` at 5 guard sites (expr.h:784, system.h:934/1013/3374/3402) blocks all 6 reducer names from ever being converted to FormulaCall. A stdlib `[sum(xs)]` spec section CAN coexist without interfering.
- Reverse-solve keystone: static-domain unroll → ordinary BinOp tree → existing 7 strategies invert it. Formula-bodied aggregations use `formula_call_bindings_contain` (system.h:4110) in Strategy 6.
- `{}` literals COMPLETELY ABSENT: `{`/`}` throw "Unexpected character" today. TokenType COUNT_==19 (static_assert lexer.h:32). Need LBRACE/RBRACE → +2 tokens → COUNT_ 21.
- `{}` literal template: `[1,2,3]` → `FUNC_CALL("vec",...)` pattern in parser.h:216-253 is the exact template for `{1,2,3}` → `FUNC_CALL("seq",...)`. No new ExprType, sizeof(Expr)==96 preserved.
- `map(body, i in [dom])` parses TODAY as 3-arg FUNC_CALL("map",...) — `parse_expr_or_iter_clause` (parser.h:79) already handles `IDENT in` lookahead. No parser change needed for explicit-iterator map form.
- `fingerprint_expr` (expr.h:1383) is the oracle for normal-case equivalence. Three supplemental direct asserts needed: empty-sum/product (Num identity), empty-max/min (unevaluated structure), exact-rational-mean (DIV(Num,Num) structural check).
- "Equivalent" = forward-evaluation equivalence only. `.fw` spec does not need to reverse-solve.

**Substrate obstacles for DESIGN:**
- O1: LBRACE/RBRACE absent (~25 LOC fix)
- O2: `{..}` range notation needs LBRACE+DOTDOT branch in primary() — design: reuse "range" node name
- O3: "map"/"fold"/"seq" must be added to `is_postload_builtin` (expr.h:2857)
- O4: `fold` operator-arg representation — no existing mechanism for passing operators as values
- O5: `count` body-free 2-arg shape vs 1-arg collector form — dual-form compatibility needed
- O6: `extract_range_values` only recognizes "range" FUNC_CALL — seq literals need O6(a) teaching or O6(b) post-load-only path
- O7: `max`/`min` have no fold identity — .fw spec form TBD (pairwise branching vs new primitive)

**How to apply:** DESIGN phase commits on the 7 obstacles; all have at least one clean path. The non-obstacles (coexist soundness, sizeof, parse) can be assumed safe.
