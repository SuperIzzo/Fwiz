# Future: Planned Features

## Motivation

Features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

## 0-3: Conditions, Ranges, Recursion, Numeric Solving — ✅ DONE

All implemented. `if`/`iff` conditions, ValueSet ranges, recursive formula calls with depth guard, numeric solving with adaptive grid scan + Newton/bisection. See DEVELOPER.md for details.

## 4. Numeric Solving — ✅ DONE

See DEVELOPER.md.

**Remaining enhancements:**
- Periodicity detection for functions with infinitely many roots (e.g., `sin(x) = 0.5`)
- Symbolic differentiation for exact Newton derivatives (currently uses finite differences)
- User-provided initial guess syntax (e.g., `x=?~5`)

## 5. Batch/Table Mode

### Problem

Users often want to evaluate a formula across a range of inputs — parameter sweeps, lookup tables, sensitivity analysis.

### Proposed syntax

```bash
fwiz --table triangle(C=?, a=[1..10], b=4, c=5)
```

Range syntax (Python-inspired with step):
```
[1..10]              # 1, 2, 3, ..., 10 (integer step)
[1..10 @ 0.5]        # 1, 1.5, 2, ..., 10 (custom step)
[0..1 @ 0.1]         # 0, 0.1, 0.2, ..., 1
[1..10 @ 0.1, 11..100 @ 1]  # compound: fine near 0, coarse further out
```

### Output

Tab-separated table, one row per input combination:
```
a       C
1       168.4630527
2       153.4349488
3       133.4321...
...
```

Multiple range inputs → cartesian product (or zip mode with `--zip`).

### Use cases

- Parameter sweeps for engineering design
- Generating lookup tables
- Plotting data (pipe to gnuplot: `fwiz --table ... | gnuplot`)
- Sensitivity analysis: how does output change across input range?

## 6. Symbolic Differentiation

### Problem

Sensitivity analysis ("how much does C change per unit change in a?") requires derivatives. Currently impossible without external tools.

### Proposed syntax

```bash
fwiz --derive triangle(dC/da=?, a=a, b=b, c=c)
```

Or as a built-in function in .fw files:
```
sensitivity = diff(force, mass)    # ∂force/∂mass
```

### Implementation

Derivative rules on the expression tree (standard calculus):
- `d/dx(x) = 1`, `d/dx(c) = 0`
- `d/dx(f + g) = f' + g'`
- `d/dx(f * g) = f'*g + f*g'` (product rule)
- `d/dx(f / g) = (f'*g - f*g') / g^2` (quotient rule)
- `d/dx(f^n) = n * f^(n-1) * f'` (chain rule + power)
- `d/dx(sin(f)) = cos(f) * f'`, etc.

The expression tree already supports all needed operations. The derivative is itself an ExprPtr that goes through the simplifier.

### Synergy with numeric solving

Newton's method currently uses finite differences for `f'(x)`. Symbolic differentiation would provide exact derivatives — faster convergence and more accurate results. This is a drop-in improvement to the existing numeric solver.

## 7. Units and Dimensional Analysis

### Problem

`speed = distance / time` should know that `100km / 2hr = 50 km/hr`, and reject `100kg / 2hr` as dimensionally invalid.

### Proposed syntax

```
# units.fw
distance [m]
time [s]
speed [m/s] = distance / time
```

Or inline:
```bash
fwiz physics(force=? [N], mass=10 [kg])
```

### Capabilities

- Automatic unit conversion within compatible dimensions
- Dimensional analysis: reject `mass + time` at parse time
- Unit inference: if `speed = distance / time` and distance is in km, time in hr, speed is in km/hr
- SI prefix handling: km → 1000m, ms → 0.001s

## 8. Standard Library

A curated collection of .fw files shipped with fwiz:

```
stdlib/
  physics/
    mechanics.fw      # F=ma, kinetic energy, momentum
    gravity.fw        # gravitational force, orbital mechanics
    thermodynamics.fw # ideal gas, heat transfer
    electromagnetism.fw
  finance/
    compound_interest.fw
    mortgage.fw
    depreciation.fw
  engineering/
    beam_deflection.fw
    pipe_flow.fw
    heat_exchanger.fw
  conversion/
    temperature.fw
    length.fw
    mass.fw
    pressure.fw
  geometry/
    circle.fw
    sphere.fw
    cylinder.fw
    triangle.fw
```

Each file is a self-contained formula sheet. Cross-file calls compose them:
```
# rocket.fw
physics/gravity(force=?weight, mass=fuel_mass, ...)
physics/mechanics(acceleration=?thrust_accel, force=thrust, mass=total_mass)
```

## 9. LaTeX Export

```bash
fwiz --latex --derive triangle(C=?, a=a, b=b, c=c)
# C = \arccos\left(\frac{b^2 + c^2 - a^2}{2bc}\right) \cdot \frac{180}{\pi}
```

Useful for documentation, papers, and reports. The expression tree already has all the structure needed — just a different printer.

## 10. Fraction Representation — ✅ DONE

Structural fractions: `DIV(Num(a), Num(b))` preserved when result is non-integer. GCD normalization, sign normalization, rational arithmetic (add/sub/mul/div/pow). Constant recognition in derive output (`log(2)`, `log(3)`, `sqrt(N)`, `pi`, `e`). Rational display in solve output via `fmt_solve_result` (main.cpp) when the result is exact. No Expr struct changes — sizeof(Expr) unchanged.

**Remaining enhancements:**
- Extended constant table (configurable extra constants)
- Rational propagation in evaluate() for exact intermediate results

## 10a. Extending `evaluate_symbolic` for new number types

`expr.h` now has two evaluators in sibling roles:

- **`double evaluate(const Expr&)`** — numeric projection. Collapses the whole tree to a real `double`. Stays real-valued forever. Used by: Newton/bisection grid scan, condition comparisons, verify-mode equality, CLI arg parsing, `solve_recursive` bindings commit.
- **`ExprPtr evaluate_symbolic(const Expr&)`** — exact projection. Returns a tree that preserves non-real structure (currently: integer rationals as `DIV(Num, Num)`). Used by: simplifier constant-folding paths (`simplify_once_impl` BINOP num/num, FUNC_CALL all-numeric).

The split is the extension point for new non-real number types. Callers choose the projection; `evaluate_symbolic`'s dispatch grows without touching call sites.

### Complex numbers checklist

Attack in this order; stop at the level that passes the user's profile:

1. **Prefer rewrite-rule route first**: `i` as a symbolic builtin constant + `.fw` rules `i * i = -1`, `i^2 = -1`, `(a + b*i) * (c + d*i) = (a*c - b*d) + (a*d + b*c)*i`. Minimalist — no new `ExprType`. Escalate to a leaf only if profile shows dense complex arithmetic as a hot path.
2. **`ExprType::COMPLEX` leaf** (fallback): adds `real, imag` doubles to `Expr`. Preserve `sizeof(Expr)` — overlay onto existing `num` + auxiliary field, or tag via `op`. Add `is_complex(e)` predicate next to `is_num(e)`.
3. **`evaluate_symbolic` BINOP dispatch** (expr.h): when one operand is complex (leaf or symbolic tree containing `i`), implement add/sub/mul/div using closed-form formulas. `pow` needs a documented branch-cut convention (principal value; log of negative reals = `i*pi`).
4. **`expr_to_string`**: render `COMPLEX(a, b)` as `a + b*i`, with sign handling for `b < 0`.
5. **`double evaluate()` rejection**: complex operands throw. Conditions `x > 0` remain real-only (complex has no total ordering). Numeric solver rejects complex systems at the outer boundary.
6. **Tests**: `simplify(i*i) == -1`, `simplify((1+i)*(1-i)) == 2`, `simplify(sqrt(-1))` propagates through compound expressions, condition on complex throws.

### Matrices / vectors checklist

Attack after complex numbers (the bindings-map extension below is shared):

1. **`ExprType::MATRIX` leaf**: shape (rows, cols) + element storage. Start with `vector<double>` for scalar entries; promote to `vector<ExprPtr>` only when symbolic entries arrive.
2. **`evaluate_symbolic` BINOP dispatch**: shape-checked add/sub (element-wise), mul (matrix product), scalar-matrix multiply. Shape mismatch → `undefined` (existing symbolic-undefined propagation).
3. **Prefer `matmul(A, B)` function call** over a new `BinOp::MATMUL`. Keeps the binop table small — data-driven principle (see DEVELOPER.md). Same for `det(A)`, `inv(A)`, `transpose(A)`.
4. **`double evaluate()` rejection**: matrix operands throw ("cannot reduce matrix to scalar").
5. **Solver bindings extension**: `bindings` is `map<string, double>` today. Matrix-valued variables require a parallel `map<string, ExprPtr>` track or promotion of the existing map to `ExprPtr`. Scope that when the first matrix use case lands — don't pre-generalize.

### Migration candidates inside the simplifier

These rational-aware sites could centralize through `evaluate_symbolic` once it gains symbolic×rational dispatch (today it only handles pure-numeric folding):

- `simplify_additive` fraction coalescing (expr.h ~line 1265)
- `simplify_mul` rational × rational (expr.h ~line 1278)
- `simplify_div` rational / rational (expr.h ~line 1306)
- `simplify_div` constant × symbolic reassociation (expr.h ~lines 1323, 1333)
- `POW` rational-base folding (expr.h ~line 1469)

Each is a future minimalism target — remove duplicated logic, single source of truth for numeric folding.

### Bindings-parameter extension

When `double evaluate()` gains a `bindings` parameter (symbolic substitution during evaluation), extend `evaluate_symbolic` with the same signature. Keep them twin APIs — every numeric projection has an exact sibling.

## 11. Curve Fitting — ✅ DONE

Implemented as `--fit [N]` flag. Templates: polynomial, power law, exponential (including Gaussian/quadratic exponent), logarithmic, sinusoidal, reciprocal. Recursive composition (depth N, default 5) discovers nested forms like `sin(sin(x))`, `e^(x*log(x))` = `x^x`. Product inners (`x*log(x)`, `x*sin(x)`) enable complex decompositions. Constant recognition (pi, e, phi, sqrt(2), sqrt(3)) in fitted coefficients.

**Remaining enhancements:**
- Rational (Padé) approximation: `p(x)/q(x)` for better convergence near singularities
- Sum-of-products inners: `a*f(x) + b*g(x)` for Stirling-type approximations
- Canonical number representation: ✅ structural fractions preserved; constant recognition in derive output

## 12. Periodicity Detection

### Problem

Functions like `sin(x) = 0.5` have infinitely many roots. Listing them all is useless. Detecting the pattern is useful.

### Approach

Post-process the roots array from `find_numeric_roots`:
1. Sort roots, compute differences between adjacent roots
2. Cluster differences — if they repeat, infer period
3. Group roots by position within one period
4. Output pattern: `x = 0.5236 + 2kπ | x = 2.618 + 2kπ`

This extends naturally from the existing numeric solver — same scan data, just pattern recognition on top.

## 13. Complex / Imaginary Numbers

Support `i` as a builtin constant. Complex arithmetic in the expression tree — enables solving polynomials with no real roots, AC circuit analysis, signal processing. Structural representation as `a + b*i` pairs, similar to how rationals use structural fractions.

Implementation plan and extension point: see **#10a — Extending `evaluate_symbolic` for new number types** (Complex numbers checklist).

## 14. Vectors, Quaternions, and Matrix Math

Vector literals (`[1, 2, 3]`), dot product, cross product, magnitude. Quaternions for rotation math. Matrix operations (multiply, inverse, determinant, eigenvalues). Key question: how to represent multi-dimensional values in the expression tree without breaking the scalar pipeline.

Implementation plan and extension point: see **#10a — Extending `evaluate_symbolic` for new number types** (Matrices / vectors checklist).

## 15. Structs / Dot Access

Hierarchical variable namespacing: `car.velocity.x`, `beam.load.max`. Enables modeling complex systems with nested properties. Could be syntactic sugar over flattened variable names (`car_velocity_x`) or a real structural feature.

## 16. Integrals and Differentials

Symbolic integration (`integral(f, x)`) alongside differentiation (#6). Definite integrals with bounds. Standard integration rules (power, trig, substitution, parts). Falls back to numeric quadrature when symbolic fails.

## 17. Big Numbers / Arbitrary Precision

Arbitrary-precision integers and rationals for exact computation beyond double range. Natural extension of structural fractions (#10). Useful for combinatorics, cryptography, number theory problems.

## 18. Bitwise / Programming Operators

`xor`, `and`, `or`, `nand`, `nor`, `not`, bit shifts, modulo. Enables digital logic, cryptographic formulas, CS-oriented problem solving. Integer-only operations — error on non-integer inputs.

## Standard Library Ideas

Beyond the collections in #8:

```
stdlib/
  number_theory/
    primes.fw           # primality, factorization, sieve
    divisibility.fw     # GCD, LCM, modular arithmetic
  combinatorics/
    permutations.fw     # nPr, nCr, factorial
    partitions.fw       # integer partitions
  probability/
    distributions.fw    # normal, binomial, poisson, uniform
    bayesian.fw         # Bayes' theorem, prior/posterior/likelihood
    expected_value.fw   # E[X], variance, standard deviation
  statistics/
    descriptive.fw      # mean, median, mode, percentiles
    regression.fw       # linear, polynomial (ties into --fit)
    hypothesis.fw       # t-test, chi-squared, p-values
```

## Interaction with existing features

- **--verify**: conditions become part of verification — check that inputs satisfy all relevant conditions
- **--derive**: output conditions alongside derived equations
- **--explore**: show which conditions are satisfiable with given inputs
- **Cross-file calls**: conditions in sub-systems are checked when resolving through formula calls
