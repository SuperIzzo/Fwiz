# Curve Fitting

`--fit [N]` samples a function and finds a closed-form approximation.

For the language syntax, see [Language.md](Language.md).
For the CLI reference, see [CLI.md](CLI.md).

## Table of Contents

1. [How It Works](#1-how-it-works)
2. [Base Templates](#2-base-templates)
3. [Composition](#3-composition)
4. [Constant Recognition](#4-constant-recognition)
5. [Output](#5-output)
6. [Flags](#6-flags)

---

## 1. How It Works

The fitter samples your function at many points, then tries to match the samples against a library of closed-form templates. Good fits (R² > 0.9) are all reported, sorted by quality.

```bash
$ fwiz --fit formula(y=?, x=x)              # y = x^2
y = x^2
  R² = 1, max error = 0

$ fwiz --fit formula(y=?, x=x)              # y = sin(x), x in [0, 2π]
y = sin(x)
  R² = 1, max error = 0
```

The fitter is most useful for recognizing numerically-computed or opaque functions as known analytical forms.

---

## 2. Base Templates

| Template | Form |
|----------|------|
| Polynomial | degree 1-10, auto-selected |
| Power law | `a * x^b` |
| Exponential | `a * e^(b*x)`, including Gaussian `a * e^(b*x^2 + c*x)` |
| Logarithmic | `a * log(x) + b` |
| Sinusoidal | `a * sin(b*x + c) + d` |
| Reciprocal | `a / (x + b) + c` |

Each template is fit via least-squares regression on the sampled points.

---

## 3. Composition

Depth `N` (default 5) wraps inner patterns in outer ones — discovering expressions like:

- `sin(sin(x))`
- `e^(x * log(x))` (= `x^x`)
- `e * sin(x)^pi`

Inner functions tried during composition: `sin`, `cos`, `sqrt`, `log`, and their products with `x`. At each composition level, the full template library is re-applied to the wrapped expression.

Depth 1 means base templates only. Each additional depth level roughly doubles the search space.

---

## 4. Constant Recognition

Coefficients are matched against known constants:

- `pi`, `e`, `phi`
- `sqrt(2)`, `sqrt(3)`, `sqrt(5)`
- Common fractions (`1/2`, `1/3`, etc.)

Good fits with recognized constants replace numeric coefficients:

```bash
$ fwiz --fit formula(y=?, x=x)              # y = pi * x
y = pi * x

$ fwiz --fit formula(y=?, x=x)              # y = 5 * sqrt(3) * x
y = 5 * sqrt(3) * x
```

---

## 5. Output

- All fits with `R² > 0.9` are listed, sorted by quality
- `--output FILE` writes the best fit as a reusable `.fw` file
- `--derive --fit` derives symbolically first, then lists fit alternatives if different

---

## 6. Flags

| Flag | Effect |
|------|--------|
| `--fit [N]` | Fit with composition depth N (default 5, depth 1 = base templates only) |
| `--output FILE` | Write best fit to `.fw` file |
| `--derive --fit` | Derive symbolically first, then show fit alternatives |
