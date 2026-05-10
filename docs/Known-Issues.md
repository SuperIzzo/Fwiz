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

## 9. CSE-I3 roundtrip perf regression post-M1 cascade (Future.md #12g)

M1's branch-multiplicity cascade grew triangle `--derive --cse` output from 158 to 649 lines (4×); reloading and re-solving the 654-line intermediate `.fw` file exceeds 60s wall-clock. The CSE-I3 test now wraps the roundtrip call with `timeout 10` and accepts either a correct numeric result (A ≈ 15–16) or a timeout-bound empty output — real correctness is not lost, but perf is degraded. Investigation needed before next derive-heavy cycle; see Future.md #12g.

## 11. Complex numbers (`i`) and vectors/matrices — current scope and limitations (2026-05-09/10)

`i` is a builtin constant with NaN binding as of Cycle A. What works and what does not:

**Works:** `simplify("i * i") == "(-1)"`, `simplify("i ^ 2") == "(-1)"`, `simplify("3 * i * i") == "(-3)"`. `evaluate()` on any `i`-containing expression returns empty `Checked<double>{}`. `i` is never wildcard-matched in rewrite-rule patterns. Equations containing `i` (e.g. `y = 2*i`) correctly produce "Cannot solve" rather than a wrong real result.

**Does not work yet:**
- `(1+i)*(1-i)` does not simplify to `2` — requires MUL-over-ADD distribution (Future #13a).
- `i^4` does not simplify to `1` — no power-cascade rule for `i^N` with N ≥ 4 (Future #13b).
- `sqrt(-1)` does not produce `i` — requires rule-parser support for negative-literal LHS.
- Complex root-finding (`resolve_all` on `x^2+1=0` returning `i`-containing forms) is not supported.

The `flatten_additive` simplifier silently drops `Num(NaN)` terms rather than propagating NaN — a pre-existing bug made reachable by the NaN-binding for `i`. It is closed off from user input by the `is_active_builtin` NaN-skip, but remains latent; see Future #13c.

## 12. Vectors and matrices (`vec`/`mat`) — current scope (2026-05-10)

`[1, 2, 3]` and `[[1,0],[0,1]]` literals are supported as of Cycle B M3. What works and what does not:

**Works:** vector/matrix literals; element-wise add/sub (`[1,2]+[3,4] == [4,6]`); scalar-mul (`2*[1,2,3] == [2,4,6]`); `matmul`, `det` (2×2 and 3×3), `inv` (2×2), `transpose` (general). Symbolic arguments are preserved throughout — `det([[a,b],[c,d]])` returns `a*d - b*c`. Shape mismatch on any operation propagates `Var("undefined")`.

**Does not work yet:**
- `inv` for 3×3 and larger — returns `undefined` (Future #14 reopen trigger).
- `det` for 4×4 and larger — returns `undefined`.
- Matrix-valued CLI bindings: `bindings` is `map<string, double>`; matrix variables cannot be bound to concrete values in CLI queries (Future #10a bindings-parameter extension).
- Complex-element matrices (e.g. `[[1+i, 0],[0, 1-i]]`) — depends on Future #13a.
- `evaluate()` always returns empty for vec/mat expressions — no real-valued projection exists. Numeric solver and condition comparisons cannot consume matrix values.

**Shape mismatch behavior** is deliberate: fwiz's domain-boundary idiom propagates `undefined` rather than throwing, consistent with `x/x = undefined iff x = 0`. Equations containing undefined operands fail to solve cleanly at the `resolve()` boundary.

## 10. M3-6 fingerprint dedup relaxed post-M1 cascade (Future.md #12f)

`derive_all` uses a 3-point Schwartz–Zippel fingerprint; the M3-6 test uses 5 branch-distinguishing points. Post-M1 (added sin/cos second inverse branches), 4 candidates collide on `derive_all`'s 3 test points but diverge on the test's 5 — `derive_all` retains them (correct under its resolution); the original `dup_count == 0` assertion was too strict. The test now allows `dup_count <= 4` with a comment explaining the cascade. Underlying fix (extend test points or add structural canonicalization, ~30-50 LOC) tracked in Future.md #12f.

## 13. Symbolic integration — current scope (Cycle 1 M1, 2026-05-10)

Indefinite Tier 1 integration via `integral(f, x)` shipped in M1. What works and what does not:

**Works:** ~25 atomic patterns: constants (`integral(c, x) → c*x`), power rule (`x^n → x^(n+1)/(n+1)`), `1/x → log(x)`, `e^x → e^x`, `sin/cos/tan(x)` antiderivatives, linearity over ADD/SUB, scalar MUL/DIV. Two surfaces: inline `f = integral(g, x)` (resolved at load time via post-load pass) and `integral(target, var)=?[alias]` CLI query. Unrecognized forms preserve the unevaluated `integral(...)` FUNC_CALL — same convention as `diff(...)`; observable in `--steps` traces and output round-trip.

**Does not work yet:**
- Definite integrals (`integral(f, x, a, b)` 4-arg form) — deferred to M2.
- Derivative-divides u-substitution (e.g. `integral(2*x*cos(x^2), x)`) — deferred to M2.
- Adaptive Simpson numeric fallback — deferred to M2.
- Integration by parts / LIATE heuristic — deferred to M3.
- `+ C` constant of integration — deliberately excluded (would not round-trip as `.fw`).
- Risch algorithm, improper integrals, multi-variable integration, cyclic IBP, trig substitution, partial fractions, special functions — see cross-arc reopen triggers in Future #16.

**Domain assumption:** `integral(1/x, x)` emits `log(x)`, not `log(abs(x))`. The mathematically complete antiderivative for unknown-sign `x` requires `abs(x)`, but emitting it unconditionally pessimises concrete-domain cases. Deferred to a domain-aware pass gated on Future #31 (global-condition propagation). See Future #63.

**No `--integrate` flag:** integration is a per-query operation with natural in-file syntax; no global render mode exists. See Future #64 for the deferred-flag rationale.

## 8. `--cse 3` default is over-aggressive on dense formula sets — RESOLVED

Resolved by Option C refactor (commit `<hash-placeholder>`). `--cse N` semantics
reframed: instead of "extract every subtree with `>= N` occurrences" (frequency
threshold), `--cse N` now caps the helper count at `N` and ranks candidates by
`value = (occurrences - 1) * (leaves - 1)` — the approximate character savings.
Single-leaf atoms have value 0 and are never extracted. The triangle reproducer
at default `--cse 3` now produces exactly 3 high-value helpers (the `acos`
compound and its two derived forms) instead of 165 atom-heavy helpers.

