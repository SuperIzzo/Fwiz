# CLI Reference

The `fwiz` command-line interface.

For the language syntax, see [Language.md](Language.md).
For solver behavior, see [Solver.md](Solver.md).
For curve fitting, see [Fitting.md](Fitting.md).

## Table of Contents

1. [Syntax](#1-syntax)
2. [Flags](#2-flags)
3. [Debug Output](#3-debug-output)
4. [Exit Codes](#4-exit-codes)
5. [Error Messages](#5-error-messages)
6. [Batch / Table Mode](#6-batch--table-mode)

---

## 1. Syntax

```
fwiz [flags] <file>(<var>=?, <var>=?<alias>, <var>=<value>, <var>=<expr>, ...)
```

The `.fw` extension is added automatically if omitted. The argument list is parsed as a formula call (see [Language.md §7](Language.md#7-formula-calls) and [§8](Language.md#8-queries-and-multiple-returns)).

Input values can be expressions: `width=2^3, height=sqrt(9)`.

A `diff(target, var)=?[alias]` query computes the symbolic derivative of `target` with respect to `var` and prints it in `lhs = rhs` form. The query goes inside the parenthesized binding list, alongside any inputs:

```bash
fwiz 'kinematic.fw(diff(distance, time)=?dx_dt)'
# dx_dt = velocity   (symbolic — distance = velocity * time)

fwiz 'kinematic.fw(diff(distance, time)=?dx_dt, velocity=15)'
# dx_dt = 15         (numerically evaluated once free variables are bound)
```

`target` may be a named variable in the loaded system (the corresponding equation's RHS is differentiated) or any expression. `var` must be a bare variable name. The output uses fwiz's standard `lhs = rhs` format; no new `d(...)/d(...)` notation is introduced.

An `integral(target, var)=?[alias]` query computes the indefinite symbolic integral of `target` with respect to `var`. The 4-arg form `integral(target, var, lo, hi)=?` computes the definite integral with symbolic F(b) − F(a) as the primary path, falling back to adaptive Simpson when symbolic integration fails on the antiderivative.

```bash
fwiz 'demo.fw(integral(x^2, x)=?antideriv)'
# antideriv = x^3 / 3

fwiz 'demo.fw(integral(x^2, x, 0, 3)=?area)'
# area = 9
```

`target` may be a named variable, formula-call expression, or literal expression. `var` must be a bare variable name. Unrecognised integrals preserve the `integral(...)` form unevaluated.

---

## 2. Flags

| Flag | Effect |
|------|--------|
| `--steps` | Show algebraic reasoning |
| `--calc` | `--steps` + numeric evaluation detail |
| `--explore` | Solve what's solvable; print `?` for the rest |
| `--explore-full` | Like `--explore`, plus print every variable in the system |
| `--verify all` | Verify all values against all equations |
| `--verify A,B` | Verify specific variables |
| `--derive` | Output symbolic equation instead of numeric result |
| `--approximate` | Collapse exact output (fractions, `pi`, etc.) to floating-point |
| `--exact` | Force exact output — default; useful to override `--approximate` |
| `--fit [N]` | Fit a curve (composition depth N, default 5) |
| `--output FILE` | Write fitted equation to `.fw` file |
| `--no-numeric` | Disable numeric solving (algebraic only) |
| `--precision N` | Numeric scan density (default 200) |
| `--table` | Emit TSV table evaluating query across range-valued inputs |
| `--zip` | With `--table`: zip-pair inputs instead of cartesian product |

Default mode optimises for **human readability** — you'll see exact fractions like `200 / 9` and recognised constants like `pi`. `--approximate` collapses everything to floating-point, including symbolic constants in `--derive` output. Use it when piping into another tool (gnuplot, a script, an LLM) that expects pre-computed numeric coefficients rather than expressions to evaluate. `--exact` is a no-op against the default, useful only to override an earlier `--approximate` in a command chain — if both appear, last wins.

See [Solver.md](Solver.md) for `--derive`, `--verify`, `--explore`, `--no-numeric`, `--precision`, `--approximate`/`--exact`.
See [Fitting.md](Fitting.md) for `--fit` and `--output`.
See [§6](#6-batch--table-mode) for `--table` and `--zip`.

---

## 3. Debug Output

Use `--steps` to see fwiz's algebraic reasoning:

```bash
$ fwiz --steps convert(celsius=?, fahrenheit=72)
loading convert.fw
  equation: fahrenheit = celsius * 9 / 5 + 32

solving for: celsius
  given:
    fahrenheit = 72
  result: celsius = 22.22222222
celsius = 200 / 9
```

Use `--calc` to also see the numeric substitution and evaluation:

```bash
$ fwiz --calc convert(celsius=?, fahrenheit=72)
  ...
    substitute fahrenheit = 72
    evaluate: (-72 + 32) / ((-9) / 5)
  result: celsius = 22.22222222
celsius = 200 / 9
```

The trace goes to stderr, so it doesn't interfere with piping the result.

Use `--approximate` to collapse exact output to floating-point — fractions, `pi`, `sqrt(2)`, etc. all become decimals. This is the mode for feeding fwiz output to another tool:

```bash
$ fwiz convert(celsius=?, fahrenheit=72)
celsius = 200 / 9

$ fwiz --approximate convert(celsius=?, fahrenheit=72)
celsius = 22.22222222

$ fwiz --derive physics(circumference=?, radius=r)
circumference = 2 * pi * r

$ fwiz --approximate --derive physics(circumference=?, radius=r)
circumference = 6.283185307 * r
```

---

## 4. Exit Codes

- `0` — success
- `1` — solve failure, parse error, file I/O error

Error messages go to stderr. Trace output from `--steps` and `--calc` also goes to stderr.

---

## 5. Error Messages

| Condition | Message |
|-----------|---------|
| No equation defines the target | `No equation found for 'x'` |
| Missing input and no default | `Cannot solve for 'x': no value for 'z'` |
| All candidate equations produce NaN/inf | `Cannot solve for 'x': all equations produced invalid results (NaN or infinity)` |
| File missing | `Cannot open file: path/to/file.fw` |
| Path is a directory | `Path is a directory, not a file: path/` |
| Bad input expression | `Invalid number 'abc' for variable 'y'` |
| `inf`/`nan` as input | `Infinity is not a valid value for 'y'` |
| Recursion depth exceeded | `Formula recursion depth exceeded (max 1000)` |
| `=?!` returned multiple solutions | `Variable 'x' has multiple solutions but was queried with =?!` |

Use `--steps` or `--calc` to diagnose solve failures — the trace shows every strategy that was tried and why it failed.

---

## 6. Batch / Table Mode

`--table` evaluates a query repeatedly across one or more range-valued inputs and emits a tab-separated table to stdout. Useful for parameter sweeps, generating data for plotting, or feeding LLMs/scripts with pre-computed grids.

### 6.1 Range Syntax

A range-valued input is an argument whose value matches `[...]` with `..` inside. Four forms:

| Form | Meaning | Example |
|------|---------|---------|
| `[a..b]` | Integer step 1, both endpoints inclusive | `[1..10]` → 1, 2, …, 10 |
| `[a..b @ s]` | Custom step, count-based generation | `[0..1 @ 0.25]` → 0, 0.25, 0.5, 0.75, 1 |
| `[r1, r2, ...]` | Compound — concatenated values | `[1..3, 7..9]` → 1, 2, 3, 7, 8, 9 |
| expression bounds | Bounds evaluated via the expression parser | `[0..2*pi @ pi/4]` |

Descending ranges require an explicit negative step: `[10..1 @ -1]`. Omitting `@` on a descending range is an error. Step zero and empty ranges both throw.

### 6.2 Cartesian vs Zip

With two range inputs, the default is **cartesian product** (every combination):

```bash
$ fwiz --table 'f(z=?, a=[1..3], b=[10..11])'
a   b   z
1   10  ...
1   11  ...
2   10  ...
2   11  ...
3   10  ...
3   11  ...
```

`--zip` pairs inputs element-wise (truncated to the shorter length; stderr warning on mismatch):

```bash
$ fwiz --table --zip 'f(z=?, a=[1..3], b=[10..12])'
a   b   z
1   10  ...
2   11  ...
3   12  ...
```

### 6.3 Output Format

The header row contains range variable names (in CLI order) followed by query aliases (in CLI order), tab-separated. Each data row has the corresponding range values then solve results, tab-separated. Unsolvable cells render as `?`.

```bash
$ fwiz --table 'examples/triangle(C=?, a=[1..5], b=4, c=5)'
a   C
1   180
2   108.2099569
3   90
4   77.36437491
5   66.42182152
```

Only range vars and query aliases appear in the header — scalar inputs like `b=4, c=5` apply to every row but aren't column-emitted. `--output FILE` redirects the TSV to a file instead of stdout. `--approximate` and `--precision` apply per-cell as usual.

### 6.4 Limits and Compatibility

A soft cap of 1 million cartesian rows emits a warning to stderr but continues running. `--table` is mutually exclusive with `--derive`, `--verify`, `--fit`, and `--explore` — one row-shaped output mode at a time. `--zip` without `--table` is an error.
