# Known Issues & Test Cases

Collected during testing. These are problems the solver can't handle yet but should eventually.

## 1. Simultaneous equations (rectangle puzzle)

```
area = w * h
perimeter = 2 * w + 2 * h
```

Query: `w=?, area=12, perimeter=14` → should give `w = 3` or `w = 4`.

**What happens:** Numeric solver crashes (stack overflow) — the 2D search space is too large for the system-probe approach.

**What's needed:** Substitution-then-solve: derive `h = 7 - w` from perimeter, substitute into area → `w² - 7w + 12 = 0`, then solve the quadratic. This requires the solver to recognize it can eliminate a variable by substitution across equations, reducing a 2-equation system to a single-variable problem.

## 2. Multi-equation validation (spurious solutions)

```
r1 = sqrt(x^2 + y^2)
r2 = sqrt((x-6)^2 + y^2)
```

Query: `x=?, r1=5, r2=4, y=0` → returns 4 values (±5 from circle 1, 2 and 10 from circle 2).

**What happens:** Each equation is solved independently. Solutions from one equation aren't validated against the others. None of the 4 values actually satisfy both equations simultaneously.

**What's needed:** Post-validation of candidates against ALL equations in the system, not just the source equation. A candidate `x=5` from `r1=5` should be checked against `r2=4` — if it fails, discard it.

This is related to issue #1 (simultaneous equations) — both require the solver to reason across multiple equations at once.

## 3. Quadratic formula

```
y = a*x^2 + b*x + c
```

Query: `x=?, y=0, a=1, b=2, c=-10` → should give `x = -1 ± sqrt(11)`.

**What happens:** Numeric solver finds both roots approximately (`x ~ -4.317, x ~ 2.317`). Algebraic solver fails because target appears in both `x²` and `x` terms.

**What's needed:** `decompose_quadratic(expr, target)` that detects `ax² + bx + c` form and applies the quadratic formula. Returns two `Solution` structs with discriminant condition (`b²-4ac >= 0`).

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

## 6. ValueSet display: open + point → closed

```
max(a=?, result=3, b=3) → a : (-inf, 3) | {3}
```

Should display as `a : (-inf, 3]`. Uniting an open interval endpoint with a discrete point at that endpoint should produce a closed interval.
