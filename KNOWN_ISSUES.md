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

## 5. Constant recognition in derive output — RESOLVED

`expr_recognize_constants()` walks derive output trees and replaces floating-point NUM nodes with recognized symbolic forms (fractions, known constants). Extended constant table includes `log(2)`, `log(3)`, `log(10)`, `sqrt(2)`, `sqrt(3)`, `sqrt(5)`, `pi`, `e`, `phi`.

```bash
$ fwiz --derive '(x=?, y=y) y = 2^x'
x = log(y) / log(2)
```

