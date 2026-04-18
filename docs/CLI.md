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

---

## 1. Syntax

```
fwiz [flags] <file>(<var>=?, <var>=?<alias>, <var>=<value>, <var>=<expr>, ...)
```

The `.fw` extension is added automatically if omitted. The argument list is parsed as a formula call (see [Language.md §7](Language.md#7-formula-calls) and [§8](Language.md#8-queries-and-multiple-returns)).

Input values can be expressions: `width=2^3, height=sqrt(9)`.

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
| `--fit [N]` | Fit a curve (composition depth N, default 5) |
| `--output FILE` | Write fitted equation to `.fw` file |
| `--no-numeric` | Disable numeric solving (algebraic only) |
| `--precision N` | Numeric scan density (default 200) |

See [Solver.md](Solver.md) for `--derive`, `--verify`, `--explore`, `--no-numeric`, `--precision`.
See [Fitting.md](Fitting.md) for `--fit` and `--output`.

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
