# The fwiz Solver

How fwiz solves equations — resolution strategies, numeric mode, symbolic derivation, verification, and exploration.

For the language syntax, see [Language.md](Language.md).
For the CLI reference, see [CLI.md](CLI.md).

## Table of Contents

1. [Resolution Strategies](#1-resolution-strategies)
2. [Algebraic Capabilities](#2-algebraic-capabilities)
3. [Equation Ordering](#3-equation-ordering)
4. [Multiple Solutions](#4-multiple-solutions)
5. [Numeric Mode](#5-numeric-mode)
6. [Symbolic Derivation](#6-symbolic-derivation)
7. [Verification](#7-verification)
8. [Exploration](#8-exploration)

---

## 1. Resolution Strategies

When asked to solve for a variable, fwiz tries these in order:

1. **Direct**: Is there an equation `target = ...`? Evaluate the right side.
2. **Invert**: Does the target appear on an equation's right side? Algebraically isolate it.
3. **Forward formula call**: Is there a formula call whose output is `target`? Evaluate it.
4. **Substitute**: Do two equations share a left-hand variable? Set them equal and solve.
5. **Reverse formula call**: Does `target` appear among a formula call's input bindings? Solve the callee in reverse.
6. **Numeric**: If algebraic strategies fail, search for roots numerically (§5).
7. **Cross-equation elimination**: For `target` in equation E1 that depends on an unknown `U`, find equation E2 that expresses `U`, substitute into E1, solve the reduced equation in one variable. Two-level elimination handles three-variable chains.

At each step, if a needed variable is unknown, fwiz recursively tries to solve for it from other equations.

---

## 2. Algebraic Capabilities

- **Linear equations**: solved by inverting the path from the target to the expression root
- **Quadratic equations**: `ax^2 + bx + c = 0` detected via `decompose_quadratic`; solved by the quadratic formula, returning both roots
- **Invertible functions**: `sin`, `cos`, `sqrt`, `log`, etc. inverted via their `.fw`-defined inverse equations
- **Like-term combination**: `y + 3*y` simplifies to `4*y`
- **Exact rationals**: integer/integer division is preserved as a structural fraction (e.g. `200 / 9`)

### What fwiz can't solve (yet)

- **General linear systems**: cross-equation elimination handles two- and three-variable chains via substitution, but there's no full Gaussian-elimination pivot for larger dense systems

---

## 3. Equation Ordering

When multiple equations can solve for the same variable, **file order is priority order**:

1. Each equation is tried in turn
2. Any unknown variable it needs is recursively resolved
3. On dead end (circular dependency, no data, invalid result), skip to the next equation
4. First equation producing a finite valid result wins

This is why `triangle.fw` lists angle-sum equations before law-of-cosines before law-of-sines: simplest relationships first. The solver automatically finds the right path through the equations regardless of which variables you provide.

---

## 4. Multiple Solutions

`=?` returns all solutions:

```bash
$ fwiz f(x=?, y=9)       # y = x^2
x = -3
x = 3
```

`=?!` errors if more than one solution is found.

Conditions narrow the result set:

```bash
$ fwiz f(x=?, y=9)       # y = x^2, x > 0
x = 3
```

---

## 5. Numeric Mode

Enabled by default. Kicks in when algebraic strategies can't isolate the target (target appears in a power, a trig function, a denominator with other symbols, or a recursive formula call).

### 5.1 Algorithm

1. **Adaptive grid scan**: sample the search range at `--precision` points (default 200); identify sign changes
2. **Bisection**: bracket each sign change
3. **Newton's method**: refine to machine precision

Re-entrance guard prevents stack overflow when a numeric solve recursively calls another numeric solve. Results are memoized.

### 5.2 Result Classification

- `x = value` — exact: forward verification reproduces the result within `EPSILON_REL`
- `x ~ value` — approximate: numerically found but not verifiable as exact

Example:

```bash
$ fwiz formula(x=?, y=9)        # y = x^2
x = -3
x = 3

$ fwiz formula(x=?, y=1)        # y = x + sin(x)
x ~ 0.5109734294
```

### 5.3 Search Range

By default, the scan searches a broad range. Global conditions narrow it:

```
side > 0
```

This restricts the scan to positive values when solving for `side`.

### 5.4 Flags

| Flag | Effect |
|------|--------|
| `--no-numeric` | Disable numeric fallback; algebraic only |
| `--precision N` | Set scan density (default 200) |

---

## 6. Symbolic Derivation

`--derive` outputs the symbolic equation for the query instead of a numeric result:

```bash
$ fwiz --derive physics(circumference=?, radius=r)
circumference = 2 * pi * r
```

Formula-call bodies are inlined into parent expressions when simple enough, enabling algebraic solving through them:

```bash
$ fwiz --derive box(width=?, volume=V, depth=d, height=h)
width = V / h / d
```

`pi`, `e`, `phi`, `sqrt(2)`, etc. are preserved symbolically. Structural fractions flow through unchanged.

---

## 7. Verification

`--verify` checks whether supplied values are consistent with equations:

```bash
$ fwiz --verify all physics(force=98.1, mass=10, g=9.81)
force = mass * g: 98.1 = 10 * 9.81 = 98.1 ✓

$ fwiz --verify force,mass physics(force=98.1, mass=10, g=9.81)
# checks only equations that define force or mass
```

`--verify all` checks every equation. `--verify A,B` checks equations defining `A` or `B`.

Useful for catching typos and confirming that a set of inputs genuinely satisfies the model.

---

## 8. Exploration

`--explore` solves for everything derivable from inputs; prints `?` for the rest:

```bash
$ fwiz --explore triangle(A=?, B=?, C=?, a=3, b=4, c=5)
a = 3
b = 4
c = 5
A = 36.86989765
B = 53.13010235
C = 90
```

`--explore-full` additionally prints every variable the file exposes, even those you didn't query. Good for understanding what a file exposes.
