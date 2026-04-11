# Future: Conditionals, Ranges, and Recursion

## Motivation

Three features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

## 0. ValueSet — Unified Foundation

### Core concept

Conditions, ranges, discrete solutions, and constraints are all the same thing: **sets of valid values**. A `ValueSet` is a union of intervals and discrete points.

```cpp
struct Interval {
    double low, high;
    bool low_inclusive, high_inclusive;  // ( vs [
};

struct ValueSet {
    std::vector<Interval> intervals;   // continuous ranges
    std::vector<double> discrete;       // individual values
    // The full set = union of all intervals + discrete points
};
```

### Operations

- **Intersect** (`&&`, `&`): `(0, +∞) ∩ (-∞, 30)` → `(0, 30)`
- **Union** (`||`, `|`): `(-∞, 0) ∪ (0, +∞)` → everything except 0
- **Filter**: `{3, -3} ∩ (0, +∞)` → `{3}`
- **Contains**: `5 ∈ (0, 30)` → true
- **Is empty**: `(0, 0)` → empty set (no solutions)
- **Complement**: `¬(0, 30)` → `(-∞, 0] ∪ [30, +∞)`

### Syntax (ASCII for .fw files and CLI)

Logic notation (on equations):
```
y = sqrt(x)        : x >= 0
tax = income * 0.1  : income > 0 && income <= 50000
```

Set notation (explicit constraints):
```
x : {1, 2, 3}              # discrete set
x : (0, 30)                 # open interval
x : [0, 30]                 # closed interval
x : (0, +inf)               # half-open to infinity
x : {-3, 3} & (0, +inf)     # intersection: positive roots only
x : (-inf, 0) | (0, +inf)   # union: nonzero
```

Terminal output (Unicode when supported):
```
x ∈ (0, 30)
x ∈ {3, -3}
x ∈ (-∞, 0) ∪ (0, +∞)
```

### Relationship to other features

- **Conditions** (`: x > 0`) are syntactic sugar for constraining to a ValueSet
- **Multiple solutions** (`?` returns all) are filtered through the ValueSet
- **Query conditions** (`x=?:>0`) apply a ValueSet filter to results
- **Verify mode** checks that values satisfy their ValueSets
- **Simplifier** can use ValueSets: `s/s → 1` when `s ∈ (-∞, 0) ∪ (0, +∞)` (nonzero)
- **Recursion base cases** are ValueSet conditions: `n = 0` is `n ∈ {0}`

## 1. Ranges and Conditions

### Problem

The simplifier assumes variables are nonzero when cancelling (e.g., `s/s → 1`). The solver returns the first valid result with no way to express constraints on valid inputs. Quadratic equations have multiple solutions with no way to select between them.

### Proposed syntax

Conditions on equations:
```
# Only valid when x > 0
y = sqrt(x)      : x >= 0
y = log(x)       : x > 0
```

Conditions on defaults:
```
g = 9.81          : g > 0
```

Range results:
```
# x^2 = 9 → x ∈ {-3, 3}
# x^2 = 9 : x > 0 → x = 3
```

### Semantics

- Conditions are checked at solve time — an equation with a failing condition is skipped (like NaN/inf today)
- Conditions propagate through substitution — if `a > 0` and `b = 2*a`, then `b > 0` is implied
- The simplifier can use conditions: `s/s → 1` is valid when `s ≠ 0` is known
- Verify mode checks conditions as part of verification

### Implementation sketch

- New syntax element: `: condition` suffix on equation lines
- Condition types: `>`, `>=`, `<`, `<=`, `!=`, `=` (equality constraint)
- Store as part of `Equation` struct: `std::optional<Condition> condition`
- Check conditions in `try_resolve` before accepting a result
- In derive mode, output conditions alongside equations

## 2. Conditionals (branching)

### Problem

Some formulas have different forms depending on input values. The triangle SSA case can have 0, 1, or 2 solutions. Piecewise functions (tax brackets, shipping tiers) need branching.

### Proposed syntax

Multiple equations for the same variable with mutually exclusive conditions:
```
# Piecewise tax
tax = income * 0.1         : income <= 50000
tax = 5000 + (income - 50000) * 0.2  : income > 50000

# Absolute value (if we didn't have abs())
result = x       : x >= 0
result = -x      : x < 0
```

### Semantics

- The solver already tries equations in order and takes the first valid one
- Conditions make the selection explicit and verifiable rather than relying on NaN fallthrough
- Verify mode can check that conditions are mutually exclusive and exhaustive
- Derive mode can show which branch was taken

## 3. Recursion

### Current state

A file can already call itself via formula calls — it stack overflows because there's no termination condition. The recursive machinery works (bindings flow through, each call gets a fresh solver), it just needs a base case.

### Proposed syntax

Conditions provide the base case:
```
# factorial.fw
factorial = n * factorial(factorial=?prev, n=n-1)   : n > 0
factorial = 1                                        : n = 0
```

Or with explicit self-reference:
```
# fibonacci.fw
fib = fibonacci(fib=?a, n=n-1) + fibonacci(fib=?b, n=n-2)  : n > 1
fib = 1    : n = 1
fib = 0    : n = 0
```

### Semantics

- Conditions on equations determine which branch fires
- When `n = 0`, the condition `: n > 0` fails → that equation is skipped
- The condition `: n = 0` succeeds → `factorial = 1` is used (base case)
- Depth limit as a safety net (configurable, default maybe 1000)

### Implementation order

1. **Conditions on equations** — parsing `: condition` syntax, storing in Equation, checking in try_resolve
2. **Range tracking** — propagating known ranges through substitution
3. **Conditional branching** — using conditions to select between equations (mostly works already via equation ordering + condition checking)
4. **Recursion** — depth guard on formula calls, conditions provide base cases
5. **Multi-solution results** — returning all valid solutions when conditions allow multiple branches

### Design considerations

- Conditions should be simple expressions that evaluate to true/false, not full equations
- The solver should report when no condition matches (exhaustiveness error)
- Recursive depth limit should produce a clear error, not a stack overflow
- Derive mode with recursion: derive the recursive formula symbolically? Or only derive non-recursive equations?
- Performance: memoization for recursive calls with the same bindings

## 4. Iterative Solving (Newton's Method)

### Problem

fwiz currently can't solve nonlinear inversions — `y = sin(x)` for `x`, `y = x^2` for `x`, or circular dependency systems. The user has to write explicit inverse equations.

### Approach

Derive the equation symbolically once, then use Newton-Raphson iteration to converge on a numeric solution. This combines --derive (get the symbolic equation) with numeric iteration.

For `y = sin(x)`, solving for `x`:
1. Rearrange: `f(x) = sin(x) - y = 0`
2. Derive: `f'(x) = cos(x)`
3. Iterate: `x_{n+1} = x_n - f(x_n) / f'(x_n)` until convergence

### Why this is powerful

The symbolic derivative only needs to be computed once. The iteration loop is pure numeric evaluation — fast. This turns fwiz's existing symbolic infrastructure into a general nonlinear solver.

### Implementation sketch

- Symbolic differentiation of expression trees (new capability in expr.h)
- `try_resolve_iterative()` as a fallback when algebraic solving fails
- Initial guess: 1.0 (or user-provided via syntax like `x=?~5` for "solve for x, start near 5")
- Convergence criterion: `|f(x)| < 1e-10`
- Max iterations: 100 (configurable)
- Multiple starting points to find different roots

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

### Synergy with iterative solving

Newton's method needs `f'(x)`. Symbolic differentiation provides it exactly — no numerical approximation needed. This makes iterative solving both faster and more accurate.

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

## 10. Fraction Representation

### Problem

`x / 2` becomes `0.5 * x` during simplification. Users prefer the fraction form, especially in derived equations.

### Approach

Keep rational numbers as `(numerator, denominator)` pairs internally. Only convert to decimal at output time (or when the fraction is irrational). This preserves `1/3` instead of `0.333...`, enables exact arithmetic, and produces cleaner derived equations.

### Key insight

Most engineering formulas use small integer fractions (1/2, 1/3, 1/4). Keeping them exact avoids floating point drift in long derivation chains.

## Interaction with existing features

- **--verify**: conditions become part of verification — check that inputs satisfy all relevant conditions
- **--derive**: output conditions alongside derived equations
- **--explore**: show which conditions are satisfiable with given inputs
- **Cross-file calls**: conditions in sub-systems are checked when resolving through formula calls
