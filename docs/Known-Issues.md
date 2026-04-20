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

## 6. Provenance loss in solve pipeline — PARTIALLY ADDRESSED

`resolve()` returns `double`; `fmt_solve_result` in `main.cpp` reconstructs exact-form display (fractions, `pi`, `sqrt(2)`, etc.) heuristically via `fmt_exact_double` → `recognize_fraction`/`recognize_constant`. The former `is_power_of_10` stopgap (which rendered `981 / 10` as `98.1`) has been removed in favour of the explicit `--approximate` flag — see [Solver.md §9](Solver.md#9-output-formatting). Default mode is now consistently exact (`weight = 1000.5` renders as `2001 / 2`); users who want the decimal form pass `--approximate`.

The underlying provenance issue — that the solver collapses symbolic values to `double` in `map<string, double>` — remains open, but its user-visible impact is now limited to two areas:

1. **`--steps` / `--calc` traces**: render intermediate values via `fmt_num` only; a fractional intermediate like `981 / 10` shows up in the trace as `98.1` even though the final answer comes out as the fraction.
2. **Recognisability limits**: values that don't match the fraction (max denominator 360, `RECOGNIZE_FRACTION_MAX_DEN`) or constant table fall through to decimal output even in exact mode. File-defined constants are now recognized via `build_alias_table()` and injected into `fmt_exact_double` as `extra_constants`, so user-defined values like `deg=pi/180` render by name rather than as raw decimals.

The long-term structural fix is to plumb `ExprPtr` through solve result types (parallel `map<string, ExprPtr>` track) so the original symbolic value reaches the formatter without reconstruction. That would make trace output match final output, remove the table-limit issue, and mirror how mainstream CAS (Mathematica, SymPy, Maxima) track exactness via the type system.

## 5. Constant recognition in derive output — RESOLVED

`expr_recognize_constants()` walks derive output trees and replaces floating-point NUM nodes with recognized symbolic forms (fractions, known constants). Extended constant table includes `log(2)`, `log(3)`, `log(10)`, `sqrt(2)`, `sqrt(3)`, `sqrt(5)`, `pi`, `e`, `phi`. File-defined constants (e.g. `deg`) are recognized via `build_alias_table()` threaded through `fmt_exact_double`.

```bash
$ fwiz --derive '(x=?, y=y) y = 2^x'
x = log(y) / log(2)
```

## 7. `--derive` output duplication and ordering — RESOLVED

`fwiz --derive` previously produced hundreds of semantically-equivalent output lines (294 for the triangle reproducer) in arbitrary order. Resolved in two cycles: (1) semantic fingerprint dedup in `derive_all` — `fingerprint_expr` evaluates each candidate at prime-cycled test points; candidates sharing a fingerprint are merged, retaining the most canonical form via `canonicity_score`; (2) results now emitted in ascending `canonicity_score` order (`{leaf_count, non_integer_num_count}`) so the simplest formula appears first and always-NaN sentinel forms appear last. `--derive N` caps output at N results after sorting. The triangle reproducer produces 159 lines; residual variation is genuine algebraic non-equivalence (different branch-cut coverage at obtuse-angle test points), not duplication.

