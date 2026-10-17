# fwiz Standard Library Reference

fwiz ships a small `.fw` standard library in `stdlib/`. These are ordinary fwiz
files — bidirectional where the math allows, invertible by the standard solver.
They are distinct from the C++ built-in functions documented in
[Language.md §11](Language.md#11-built-in-functions).

## Invocation

stdlib modules come in two structural forms with different invocation paths.

**Flat files** expose their variables as top-level bindings. Query `result=?alias`
directly:

```bash
$ fwiz 'stdlib/combinatorics/hyp_pmf.fw(N=54, K=13, n=5, k=2, result=?p)'
p = 0.2629177457
```

`hyp_pmf.fw` and `hyp_at_least.fw` are flat files.

**Section-based files** wrap the computation in a `[name(args) -> result]` section.
The `result` variable is section-scoped and cannot be queried at the file level as
`result=?alias`. Three working forms:

**1. Positional strict mode** — works for single-section files (e.g. `factorial`):

```bash
$ fwiz 'stdlib/combinatorics/factorial.fw(r=?, r=factorial(5))'
r = 120
```

**2. Quick synthetic eval (`--legacy-implicit`)** — works for all section-based
files; the second binding is evaluated inside the file's scope:

```bash
$ fwiz --legacy-implicit 'stdlib/combinatorics/nCr.fw(c=?, c=nCr(52,5))'
c = 2598960
```

**3. Production use (`@include` + consumer file)** — recommended for programs. Create
a consumer `.fw` file that includes the module and calls it by named binding:

```fw
# use_ncr.fw
@include "stdlib/combinatorics/nCr.fw"
c = nCr(n=52, k=5, result=?)
```

```bash
$ fwiz -I /path/to/repo use_ncr.fw(c=?)
c = 2598960
```

---

## stdlib/combinatorics/

Seven functions for counting, arrangement, and hypergeometric probability.

| Function | Signature | Computes | Verified example |
|---|---|---|---|
| `factorial` | `factorial(n)` | n! (bounded product; exact integer) | `factorial(5)` → `120` |
| `nCr` | `nCr(n, k)` | binomial coefficient C(n,k) (multiplicative form, overflow-safe, exact) | `nCr(52,5)` → `2598960`; `nCr(54,27)` → `1.946939426e+15` (exact `1946939425648112`) |
| `nPr` | `nPr(n, r)` | permutations n!/(n−r)! (falling-product form) | `nPr(5,2)` → `20` |
| `falling` | `falling(x, m)` | falling factorial x·(x−1)···(x−m+1) | `falling(5,2)` → `20` |
| `rising` | `rising(x, m)` | rising factorial x·(x+1)···(x+m−1) | `rising(5,2)` → `30` |
| `hyp_pmf` | `hyp_pmf(N,K,n,k)` | hypergeometric PMF P(exactly k successes) — **flat file, direct** | `hyp_pmf(N=54,K=13,n=5,k=2,result=?p)` → `0.2629177457` |
| `hyp_at_least` | `hyp_at_least(N,K,n,kmin)` | hypergeometric upper tail P(≥kmin) — **flat file, direct** | `hyp_at_least(N=54,K=13,n=5,kmin=2,result=?p)` → `0.3467505241` (P(≥2 clubs in 5 of 54)) |

`factorial`, `nCr`, `nPr`, `falling`, and `rising` are section-based; use one of the
forms from §Invocation above. `hyp_pmf` and `hyp_at_least` are flat files; query
`result=?alias` directly.

Full working CLI commands for copy-paste:

```bash
# Section-based — quick synthetic eval:
$ fwiz --legacy-implicit 'stdlib/combinatorics/nCr.fw(c=?, c=nCr(52,5))'
c = 2598960

# Flat file — direct:
$ fwiz 'stdlib/combinatorics/hyp_pmf.fw(N=54, K=13, n=5, k=2, result=?p)'
p = 0.2629177457
```

All five section-based combinatorics functions use bounded aggregation
(`product(body, i in [1..k])`) internally — no intermediate factorials are built,
so large arguments stay exact. Empty-domain identities: `factorial(0) = 1`,
`nCr(n,0) = 1`, `nPr(n,0) = 1`.

---

## stdlib/probability/

Five functions for expected values, variance, and order statistics of discrete
distributions.

| Function | Signature | Computes | Verified example |
|---|---|---|---|
| `expected_value` | `expected_value(N)` | E[X] of discrete uniform {1..N} = (N+1)/2 | `expected_value(6)` → `7 / 2` (fair die mean 3.5) |
| `variance` | `variance(N)` | Var[X] of discrete uniform {1..N} | `variance(6)` → `35 / 12` (≈ 2.9167) |
| `hyp_mean` | `hyp_mean(N,K,n)` | hypergeometric mean nK/N (closed form, exact rational) | `hyp_mean(54,13,5)` → `65 / 54` (≈ 1.2037) |
| `hyp_variance` | `hyp_variance(N,K,n)` | hypergeometric variance with finite-population correction | `hyp_variance(54,13,5)` → `0.8449478479` |
| `order_statistic` | `order_statistic(N,n,k)` | E[k-th smallest of n drawn from {1..N}] = k(N+1)/(n+1) (exact) | `order_statistic(5,5,5)` → `5` (E[max of 5]); `order_statistic(9,4,3)` → `6` |

All are section-based; use the `--legacy-implicit` or `@include` consumer form (see
§Invocation above).

`expected_value` and `variance` are implemented as bounded aggregations (Σ value·prob)
over the uniform domain {1..N}. The iterator is named `v`, not `i` — `i` is the
imaginary unit, and the rewrite rule `i^2 = -1` would corrupt any squared-iterator body
before the aggregation unrolls. Non-uniform expectations follow the same `value * prob`
body shape with a custom probability expression. `hyp_mean` and `hyp_variance` are
closed-form expressions (no aggregation).

---

## stdlib/collections/

Fold operators and named-section reducers for ordered `{}` collections. See
[Language.md §19](Language.md#19-ordered-collections--map-foldl) for full usage,
syntax, and the `@include` path.

**Fold operators** — binary step functions for `foldl(collection, op, identity)`:

| File | Section | Step |
|------|---------|------|
| `add.fw` / `operators.fw` | `[add(acc, elem) -> result]` | `result = acc + elem` |
| `mul.fw` / `operators.fw` | `[mul(acc, elem) -> result]` | `result = acc * elem` |
| `count_op.fw` | `[count_op(acc, elem) -> r]` | `r = acc + 1` (ignores elem value) |

**Named-section reducers** — accept `{}` seq collections directly, proven equivalent
to the C++ built-in reducers:

| File | Computes | Example |
|------|---------|---------|
| `sum_of.fw` | sum via `foldl(..., add, 0)` | `sum_of({1,2,3})` → `6` |
| `product_of.fw` | product via `foldl(..., mul, 1)` | `product_of({1,2,3,4})` → `24` |
| `count_of.fw` | count via `foldl(..., count_op, 0)` | `count_of({1,2,3})` → `3` |
| `mean_of.fw` | mean as `sum_of / count_of` | `mean_of({1,2,3,4})` → `5 / 2` (exact) |

---

## stdlib/stdlib.fw — helpers

Pure-fwiz piecewise functions using bidirectional `iff` conditions, so they work in
both forward and reverse solves.

| Function | Computes |
|----------|---------|
| `abs(x)` | Absolute value |
| `sign(x)` | Sign function (−1, 0, 1) |
| `clamp(x, lo, hi)` | Clamps x to [lo, hi] |
| `max(a, b)` | Maximum of two values |
| `min(a, b)` | Minimum of two values |
| `step(x)` | Heaviside step (0 if x<0, 1 if x≥0) |

Note: `abs` is also a C++ built-in (Language.md §11) for performance; the stdlib
version demonstrates user-definable piecewise functions. The C++ built-in takes
precedence when both are available.

---

## stdlib/units/si-minimal.fw — SI units

Binds SI base units, common prefixes, and five derived units to dimensionless scalars
so that unit-suffix expressions resolve numerically (see Language.md §2 "Unit Suffixes").

**Base units** (all scalar 1): `m, kg, s, A, K, mol, cd`

**Prefixes:** `km, mm, um, nm, Mm, Gm` (length); `g, mg` (mass); `ms, us, ns, min, hr, day` (time)

**Derived:** `N` (newton), `J` (joule), `W` (watt), `Pa` (pascal), `Hz` (hertz)

All values cascade from the base: `km = 1000 * m`, `N = kg * m / s^2`, etc.

```bash
$ fwiz 'stdlib/units/si-minimal.fw(distance=10km, time=2hr, speed=?, speed_eqn=distance/time, speed_eqn=speed)'
speed = 25 / 18
```

The CLI accepts exactly one filename; multi-file loads require inline bindings or the
`@include` idiom. Dimensional-analysis rejection is tracked in FWIZ-49/#7b.

---

## stdlib/physics/mechanics.fw — Newtonian mechanics

Bidirectional Newtonian-mechanics equations. Any single unknown resolves via the
standard solver (`force=?`, `mass=?`, and `accel=?` all work from
`force = mass * accel`). Inlines the SI base bindings it depends on so that unit-suffix
CLI args collapse end-to-end without a separate units file.

Equations: `force = mass * accel`, weight, kinetic energy, momentum, work, power,
pressure, `frequency = 1 / period`.

```bash
$ fwiz 'stdlib/physics/mechanics.fw(force=?, mass=10kg, accel=9.81)'
force = 981 / 10
$ fwiz 'stdlib/physics/mechanics.fw(mass=?, force=98.1, accel=9.81)'
mass = 10
```

Variable names use verbose forms (`mass`, not `m`; `displacement`, not `s`) to avoid
shadowing SI base-unit symbols.

---

## stdlib/builtin.fw — C++ built-in reference

The `.fw` representation of the C++-backed built-ins. Each section shows the `@extern`
directive and the inverse equation used for reverse solving:

```
[sin(x) -> result] @extern sin
x = asin(result)
```

This file is **reference documentation** — the actual built-ins are compiled into the
binary. Copy the pattern and register via the C++ `register_function()` API when
defining a new `@extern`-backed function; see Developer.md.
