# Future: Conditionals, Ranges, and Recursion

## Motivation

Three features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

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

## Interaction with existing features

- **--verify**: conditions become part of verification — check that inputs satisfy all relevant conditions
- **--derive**: output conditions alongside derived equations
- **--explore**: show which conditions are satisfiable with given inputs
- **Cross-file calls**: conditions in sub-systems are checked when resolving through formula calls
