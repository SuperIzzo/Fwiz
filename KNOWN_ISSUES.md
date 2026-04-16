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

## 3. Numeric solver explosion on multi-equation systems

When the numeric solver's system-probe runs on systems with many equations, it can scan thousands of values through recursive evaluation chains, making `--steps` output enormous and sometimes crashing.

**Example:** The temperature chain (`F = C*9/5+32; K = C+273.15; R = F+459.67`) — the algebraic solver finds the answer instantly, but the numeric solver still tries additional paths through unused equations.

**What's needed:** Skip numeric probing when the algebraic solver already found a clean result for all queries. Or limit numeric to only equations that the algebraic solver failed on.

## 4. Fraction display in exponents

```
y = x^3, solve for x → x = y^0.3333333333
```

Should display as `x = y^(1/3)`. The rational recognizer exists in the fitter but isn't applied to derive output.

## 5. Constant recognition in derive output

```
y = 2^x, solve for x → x = log(y) / 0.6931471806
```

Should display as `x = log(y) / log(2)`. The constant recognizer exists in the fitter but isn't applied to derive output.

