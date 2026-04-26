# Known Issues & Test Cases

Collected during testing. These are problems the solver can't handle yet but should eventually.

## 1. Simultaneous equations (rectangle puzzle) — ✅ RESOLVED

Strategy 7 in `enumerate_candidates()` handles cross-equation variable elimination by substitution. For each unknown variable in a target equation, it finds another equation that can express it, substitutes in, then solves the reduced single-variable expression. Two-level elimination handles 3-variable chains. `expand_for_var()` in `expr.h` distributes MUL over ADD/SUB to enable quadratic decomposition of substituted expressions.

```bash
$ fwiz '(w=?, area=12, perimeter=14) area = w * h; perimeter = 2 * w + 2 * h'
w = 3
w = 4
```

## 2. Multi-equation validation (spurious solutions) — ✅ RESOLVED

Strategy 7 eliminates spurious solutions structurally — by substituting one equation into another before solving, only values that satisfy both equations are returned. The circle intersection example now returns only the valid intersection point rather than all candidates from each equation independently.

## 3. Quadratic formula — ✅ RESOLVED

Algebraic quadratic solving now works. `decompose_quadratic` in `solve_for_all()` flattens expressions into additive terms, classifies each by degree in the target variable, and applies the quadratic formula. Returns two `Solution` structs with discriminant condition (`b²-4ac >= 0`).

```bash
$ fwiz --no-numeric '(x=?, y=0) y = x^2 - 7*x + 12'
x = 3
x = 4
```

## 3. Numeric solver explosion on multi-equation systems — ✅ RESOLVED

Two fixes: (1) `solve_all()` skips NUMERIC candidates for multi-variable equations when algebraic strategies already found results. (2) Trace output suppressed during numeric system-probe scans — the 200+ `resolve_memoized` calls per probe no longer emit full `solve_recursive` traces. Rectangle puzzle `--steps` went from 24,000 lines to 26.

```bash
$ fwiz --steps 'rect.fw(w=?, area=12, perimeter=14)'  # 26 lines, not 24,000
```

## 4. Fraction display in exponents — RESOLVED

Structural fractions: the simplifier now preserves `DIV(Num(a), Num(b))` when the result is non-integer, instead of folding to a decimal. GCD normalization and sign normalization applied. Rational arithmetic (add, subtract, multiply, divide, power) implemented for structural fractions.

```bash
$ fwiz --derive '(x=?, y=y) y = x^3'
x = y^(1 / 3)
```

## 6. Provenance loss in solve pipeline — RESOLVED

`resolve()` returns `double`; `fmt_solve_result` in `main.cpp` reconstructs exact-form display (fractions, `pi`, `sqrt(2)`, etc.) heuristically via `fmt_exact_double` → `recognize_fraction`/`recognize_constant`. The former `is_power_of_10` stopgap (which rendered `981 / 10` as `98.1`) has been removed in favour of the explicit `--approximate` flag — see [Solver.md §9](Solver.md#9-output-formatting). Default mode is now consistently exact (`weight = 1000.5` renders as `2001 / 2`); users who want the decimal form pass `--approximate`.

**Path B structural fix (2026-04-26):** `FormulaSystem` now carries a parallel `mutable std::map<std::string, ExprPtr> solved_symbolic_` alongside the numeric `bindings` map. At the binding-commit point (T10, `try_resolve`), `expr_recognize_constants` is applied once to the solver's `simplified` ExprPtr and the result is stored in `solved_symbolic_[target]`. Trace sites (T4/T7/T8/T9) read from this map via the unified `fmt_trace(double, ExprPtr=nullptr, key="")` helper. By construction, trace and final output cannot disagree — both render from the same stored ExprPtr.

The fix extends to cross-formula traces via a 5-line sub-system bridge at T7: after `sub_sys.resolve()`, the parent looks up `sub_sys.solved_symbolic_[resolve_var]` and adopts the ExprPtr, so formula-call results render symbolically in the parent trace. Confirmed working on 4 tests, including an adversarial case (`x = y / 401` with `y = 803`) where `RECOGNIZE_FRACTION_MAX_DEN = 360` would have blocked heuristic recovery — the structural carrier gave `803 / 401` directly.

**Remaining caveat:** T1 (`trace_loaded`) — the line emitted when file defaults are loaded — was intentionally left at `fmt_num`. At that point `aliases_` is not yet populated (it is built on first `build_alias_table()` call). If `--steps` shows a decimal default value at the loading line for a user-named constant, address via Future entry R6. The `@extern` result path (T6) also falls back to `fmt_exact_double` since C++-computed return values have no symbolic source.

**Research anchor:** `docs/research/provenance-plumbing.md` — full call-site audit (10 trace sites, 9 carrier-flow sites, 11 must-stay-double sites), CAS prior art (SymPy / Mathematica / Maxima / Maple all use parallel-symbolic; PSLQ argument rules out float-then-recover), and Path A (~30 LOC heuristic-only) vs Path B structural decision context.

## 5. Constant recognition in derive output — RESOLVED

`expr_recognize_constants()` walks derive output trees and replaces floating-point NUM nodes with recognized symbolic forms (fractions, known constants). Extended constant table includes `log(2)`, `log(3)`, `log(10)`, `sqrt(2)`, `sqrt(3)`, `sqrt(5)`, `pi`, `e`, `phi`. File-defined constants (e.g. `deg`) are recognized via `build_alias_table()` threaded through `fmt_exact_double`.

```bash
$ fwiz --derive '(x=?, y=y) y = 2^x'
x = log(y) / log(2)
```

## 7. `--derive` output duplication and ordering — PARTIALLY RESOLVED

`fwiz --derive` previously produced hundreds of semantically-equivalent output lines (294 for the triangle reproducer) in arbitrary order. Resolved in two cycles: (1) semantic fingerprint dedup in `derive_all` — `fingerprint_expr` evaluates each candidate at prime-cycled test points; candidates sharing a fingerprint are merged, retaining the most canonical form via `canonicity_score`; (2) results now emitted in ascending `canonicity_score` order (`{leaf_count, non_integer_num_count}`) so the simplest formula appears first and always-NaN sentinel forms appear last. `--derive N` caps output at N results after sorting. The `sqrt(x)^2 = x iff x >= 0` rewrite rule (2026-04-20) eliminates all `sqrt(...)^2` tautologies from the output.

The triangle reproducer is now at 158 lines (159 → 158 after the 2026-04-24 `rebuild_multiplicative` split-by-sign cycle eliminated one redundant line). The remaining output comes from two sources: (a) genuinely-distinct algebraic forms (different branch-cut coverage at obtuse-angle test points — correct behavior, not duplication), and (b) ~143 Category C "self-substitution" lines where the derivation strategy over-enumerates via cross-equation substitution, producing forms that are semantically equivalent to shorter canonical ones but fingerprint-distinctly at the chosen test points. Category C is an active investigation; see Future #32 and `docs/research/category-c-investigation.md`.

## 8. `--cse 3` default is over-aggressive on dense formula sets — RESOLVED

Resolved by Option C refactor (commit `<hash-placeholder>`). `--cse N` semantics
reframed: instead of "extract every subtree with `>= N` occurrences" (frequency
threshold), `--cse N` now caps the helper count at `N` and ranks candidates by
`value = (occurrences - 1) * (leaves - 1)` — the approximate character savings.
Single-leaf atoms have value 0 and are never extracted. The triangle reproducer
at default `--cse 3` now produces exactly 3 high-value helpers (the `acos`
compound and its two derived forms) instead of 165 atom-heavy helpers.

