# Future: Planned Features

## Motivation

Features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

## 0-3: Conditions, Ranges, Recursion, Numeric Solving — ✅ DONE

All implemented. `if`/`iff` conditions, ValueSet ranges, recursive formula calls with depth guard, numeric solving with adaptive grid scan + Newton/bisection. See Developer.md for details.

## 4. Numeric Solving — ✅ DONE

See Developer.md.

**Remaining enhancements:**
- Periodicity detection for functions with infinitely many roots (e.g., `sin(x) = 0.5`)
- Newton now uses symbolic derivatives automatically when `try_resolve_numeric` calls `symbolic_diff_simplified`; finite-diff is the fallback when `symbolic_diff_simplified` returns `nullptr` (e.g., unrecognized function). No further wiring needed.
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

## 6. Symbolic Differentiation — ✅ DONE (2026-04-26)

`symbolic_diff(const Expr&, const std::string& var) → ExprPtr` (expr.h). Two-level dispatch: per-AST-class switch for ADD/SUB/MUL/DIV/POW/NEG, inline if-chain for FUNC_CALL covering 9 builtins (sin, cos, tan, asin, acos, atan, log, sqrt, abs). `symbolic_diff_simplified` wrapper calls `simplify()` on the result. Returns `nullptr` for unknown FUNC_CALLs — used as a "leave-symbolic" signal by the post-load pass.

Post-load pass `resolve_diff_in_equations` (system.h): rewrites `diff(named_var, x)` and `diff(formula_call_placeholder, x)` nodes after all equations and rewrite rules are loaded. Handles three target shapes: Var-as-equation RHS, Var-as-formula-call output, and literal expression.

Two API surfaces:
1. In-file builtin: `sensitivity = diff(force, mass)` — parser-level recognition replaces the call with the differentiated tree at load time.
2. CLI query: `fwiz kinematic.fw 'diff(distance, time)=?'` — `CLIDiffQuery` struct in `parse_cli_query`; dispatched in `main.cpp` Pass 1.5 by injecting a synthetic equation then running the post-load pass. Falls back to printing the symbolic expression when free variables remain.

`solved_symbolic_` carries derivative results (confirmed via `test_symbolic_diff_provenance`), validating the pre-positioned carrier.

Three new rewrite rules in `BUILTIN_REWRITE_RULES`: `x^a / x^b = x^(a-b) iff x != 0` (pulled forward from Future #5 scope), `abs(x) / x = sign(x) iff x != 0`, `abs(x) / x = undefined iff x = 0`. `sign(x)` registered as a new builtin with `sign_eval` numeric evaluator.

Tests: 2237 → 2272 (+35). Newton-exact-Jacobian drop-in remains a near-term M4 micro-cycle — see #4 remaining enhancements.

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
- ~~Extended constant table (configurable extra constants)~~ — shipped 2026-04-19 (ccacc8e / 43cbc0d) via `fmt_exact_double` alias threading and `build_alias_table()`: user-defined `.fw` constants (e.g. `deg = pi/180`) are now recognized in solve + derive output.
- Rational propagation in evaluate() for exact intermediate results

## 10a. Extending `evaluate_symbolic` for new number types

`expr.h` now has two evaluators in sibling roles:

- **`Checked<double> evaluate(const Expr&)`** — numeric projection. Collapses the whole tree to a real `double`; empty on structural failure (NaN sentinel, no bool). Stays real-valued forever. Used by: Newton/bisection grid scan, condition comparisons, verify-mode equality, CLI arg parsing, `solve_recursive` bindings commit.
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
3. **Prefer `matmul(A, B)` function call** over a new `BinOp::MATMUL`. Keeps the binop table small — data-driven principle (see Developer.md). Same for `det(A)`, `inv(A)`, `transpose(A)`.
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

When `evaluate()` gains a `bindings` parameter (symbolic substitution during evaluation), extend `evaluate_symbolic` with the same signature. Keep them twin APIs — every numeric projection has an exact sibling.

For the solver binding track specifically: `solved_symbolic_` (`src/system.h`) is already the parallel ExprPtr map that the provenance-plumbing cycle shipped for trace output. The symbolic-differentiation cycle (#6) confirmed that `diff(...)` results commit into `solved_symbolic_` exactly as algebraic results do — no API change was needed. Matrix bindings simply need to add their leaf type to `expr_to_string` dispatch; the carrier itself requires no structural change.

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

## 19. `Checked<double>` — zero-overhead optional alternative — ✅ DONE

Implemented as `Checked<T>` (not `checked_value`). NaN-sentinel optional replacing `std::optional<double>` for `evaluate()` return type. `sizeof(Checked<double>) == sizeof(double)`; returns in one FP register; `operator*` deliberately absent — unwrap via `.value()`. Full three-file migration: `expr.h` (type definition + evaluate signature), `system.h` (~20 call sites + hot probe lambda), `fit.h` (2 probe lambdas). Commits 7095f95 (type + evaluate), 620c3d9 (hot probe), 6608bdd (fit.h). 1829/1829 tests passing post-migration.

## 20. Formula calls as typed expression nodes

Currently, formula calls are extracted at parse time into a side-channel
(`FormulaSystem::formula_calls`) and replaced with synthetic `Var`
placeholders in the expression tree. This works but requires ad-hoc
exclusion of alias identifiers from `collect_vars` at one call site
(`system.h:~1423`), and makes it harder to support:

- Matrix-valued formula calls (need typed expression boundaries)
- Symbolic differentiation through formula calls (chain rule applications
  want the call as a stable tree node, not a synthetic identifier)
- Batch-mode amortization of formula-call derivation
- LaTeX export of formula calls

Promoting formula calls to a dedicated `ExprType::FORMULA_CALL` node would
remove the alias-exclusion hack and give matrix types / symbolic
differentiation a clean foundation. Estimated ~180 lines across parser,
evaluator, simplifier, and solver strategies.

**Status: DEFERRED** (decision recorded in
`.fwiz-workflow/design-formula-call-typed.md`). Full design + critic +
visionary review ran. Conclusion: #22 (post-derive simplification & dedup,
the only scheduled dependent) doesn't actually need typed nodes — it's a
`.fw` rewrite rule plus ~5 LOC of dedup against the existing side channel.
Matrix types (#14) and symbolic differentiation (#6) are the natural
drivers, but neither is scheduled. Shipping this refactor now is
speculative infrastructure.

**Reopen trigger — revisit when ANY of these lands in a planning cycle:**

1. **Matrix-valued formula returns (#14)** where a call's result needs shape
   metadata not expressible in scalar bindings.
2. **Symbolic differentiation (#6)** reaches `diff(formula_call(...), var)`
   and the chain rule needs stable node identity.
3. A **second** unrelated feature wants an `aux_index` payload (LaTeX hints
   #9, big-number handles #17, units annotation #7). At that point **build
   the generic `aux_index` primitive first** (a `uint32_t` in `Expr`'s
   existing padding after `op`, zero new bytes), then migrate FORMULA_CALL
   as the first consumer. Correct sequencing: general primitive precedes
   specialization.
4. **Sub-system bridge deletion**: when typed FORMULA_CALL nodes ship, delete the 5-line direct member access at the T7 sub-system bridge in `system.h` (the `sub_sys.solved_symbolic_.find(resolve_var)` lookup) and route through the typed node's evaluation instead.

**Do NOT use `reinterpret_cast` overlay** when eventually revisited. The
`aux_index` handle in existing padding is strictly more general and
debugger/ASan-friendly — it's the correct primitive for all four listed
use cases, not a specialization.

## 21. Composable / Nested Formula Calls

Compose formula calls as expression-tree values, so the output of one call
feeds directly into another without intermediate files or manual binding.

### Proposed syntax (nested form)

```bash
fwiz 'sin(x=?, triangle(A=?x, a=100, b=50, B=0.3))'
```

Reads as: "solve `triangle` for `A`, alias it as `x` so the outer `sin`
call picks it up by name; solve `sin(x, result=?)`." Positional binding of
the nested call's value into `sin`'s `x` argument also works:

```bash
fwiz 'sin(result=?, triangle(A=?, a=100, b=50, B=0.3))'
```

Here the nested call is evaluated to a single value (its queried `A`) and
positionally binds `sin`'s first argument (which is `x` per the section
header `[sin(x) -> result]`).

### Proposed syntax (dotted form)

Flat alternative with path-qualified variables, related to #15 Structs /
Dot Access:

```bash
fwiz 'sin(result=?, triangle.A=?sin.x, triangle.a=100, triangle.b=50, triangle.B=0.3)'
```

Reads as: "triangle's `A` (queried) is bound to `sin`'s `x`; triangle's
sides and `B` are given; solve `sin.result`." The dotted alias on the RHS
of `=?` routes the value into a named scope, not just exposing it by name.

### Open design question — direction of binding

Both forms below express "`triangle.A` and `sin.x` are the same variable":

```
triangle.A=?sin.x         # query A in triangle, feed into sin.x
sin.x=?triangle.A         # query sin.x, receive it from triangle.A
```

The first reads as a "producer" point of view (triangle produces, sin
consumes); the second as a "consumer" point of view (sin names its input
by where it came from). Three possible resolutions:

1. **Accept only the producer form** (`triangle.A=?sin.x`). Matches the
   existing `A=?alias` convention where LHS names the variable being
   solved and RHS names the output slot. Simpler grammar.
2. **Accept both as equivalent** — the dotted paths and `=?` form a
   bidirectional "these two identifiers refer to one variable" assertion;
   direction is stylistic. More flexible, but invites confusion about
   which side is "the source."
3. **Assign different semantics** — producer form means "forward-evaluate
   triangle then feed sin"; consumer form means "inverse-solve sin.x then
   back-propagate to triangle.A as a constraint." This matches how one
   might naturally express the two computational directions, but fwiz's
   solver should already pick direction automatically — so this
   distinction is likely spurious.

Leaning toward (2) at design time: treat `=?` with dotted paths as a
binding-equality assertion, solver chooses direction.

### Open design question — implicit output routing

A tempting shortcut removes the explicit `triangle.A=?sin.x` binding:

```bash
fwiz 'sin(result=?, triangle.a=100, triangle.b=50, triangle.B=0.3)'
```

Intuition: "whatever triangle computes becomes sin's input." With the
above bindings (two sides + one angle, an AAS/SSA shape), triangle
produces a single resolvable angle `A`, and `sin.x = A` is unambiguous.

But with **more** bindings the shortcut breaks:

```bash
fwiz 'sin(result=?, triangle.a=100, triangle.b=50, triangle.c=70, triangle.B=0.3)'
```

Given SSS (`a, b, c`) plus `B`, all three angles `A, B, C` become derivable.
Which one routes to `sin.x`?

Four possible designs, each with tradeoffs:

1. **No implicit routing.** Require explicit `triangle.A=?sin.x`. Simple and
   predictable but verbose in the common unambiguous case.
2. **Implicit when unambiguous; clear error otherwise.** Allow the short form
   when the inner scope exposes exactly one resolvable free variable
   compatible with the outer call's input type; else error with a message
   pointing at explicit syntax (`triangle exposes {A, B, C} — specify which
   feeds sin.x via triangle.A=?sin.x`). Matches fwiz's general philosophy.
3. **Multi-solution — `sin.result` returns once per compatible inner value.**
   Leverages existing multi-solution support, but likely violates the
   first-successful / LLM-deterministic commitments from the triangle cycle.
   Use `--explore` for this semantic instead.
4. **Positional-order default.** Use the first inner variable in file order.
   Fragile; depends on `.fw` file formatting.

Leaning toward **(2)** — allows the shortcut where it's safe, fails loud
where it isn't. Gives LLMs a deterministic surface, and nudges users toward
explicit routing when they need it. Option (3) is what `--explore` already
gives you; no need to overload `=?`.

### Why

- Encourages composition over monolithic `.fw` files.
- LLM-friendly: a single CLI line expresses a multi-step reasoning chain
  without creating transient files.
- Matches how users think about chained problems: "solve the triangle, then
  take the sine of that angle."

### Implementation notes

Both forms touch `parse_cli_query` (the arg-list parser at `system.h:~3037`)
and the expression evaluator. Benefits directly from #20 (formula calls as
typed expression nodes) — the nested form becomes a tree of `FORMULA_CALL`
nodes, trivially evaluated left-to-right. Without #20, the synthetic-alias
side-channel approach can still work but gets messier with multiple aliases
in one CLI line.

Dotted form interacts with #15 — a shared implementation of path-qualified
variable names covers both CLI-query dotted access and in-file sub-scope
references.

## 22. Post-derive simplification and deduplication — ✅ DONE (2026-04-19, ccacc8e / 43cbc0d / 319c9e3)

Semantic fingerprint dedup shipped. `fingerprint_expr` + `canonicity_score` primitives in `expr.h`; streaming `winners` map in `derive_all`; `build_alias_table()` + `source_label_` on `FormulaSystem`; `RECOGNIZE_FRACTION_MAX_DEN` raised to 360 with `extra_constants` threading. Triangle reproducer: 294 → 159 output lines (46% reduction). Results now emitted ascending by `canonicity_score` — simplest formula first; sentinel-bucket forms last. `--derive N` caps at N results. Defect A fixed: `free_vars` in fingerprinting now uses alias values, not keys, so CLI alias queries fingerprint correctly. 1944/1944 tests pass.

**Original problem statement (archived):**

`fwiz --derive "examples/triangle(A=?, a=4, B=20, c, b)"` used to produce 294
distinct output lines. Many were semantically equivalent, just rendered
differently — e.g.:

```
A = 180 / pi * acos((b^2 + c^2 - 16) / (2 * b * c))
A = acos((b^2 + c^2 - 16) / (2 * b * c)) / 0.01745329252
```

Same equation; one uses `180/pi`, the other uses its decimal reciprocal.

Worse, individual expressions contain obvious dead arithmetic. Example
from the same output:

```
A = ... (-b/2 - c/2 + (b+4)/2 - 2) ...
```

That parenthesized sub-expression equals `-c/2`. Verified: fwiz's simplifier
correctly handles `-b/2 - c/2 + b/2 + 4/2 - 2 → -c/2` when the user writes
the distributed form, but leaves `(b+4)/2` opaque.

### Root cause

The simplifier's like-term combiner can't peek inside `DIV(ADD(b, 4), 2)`
to see the `b/2` hiding there. Distribution of division over addition
(`(a + c) / k → a/k + c/k` when `k` is a numeric literal) is missing.

Two independent improvements:

### Improvement A — Division-over-addition distribution

Add a simplification rule (either in `simplify_div` in `src/expr.h` or as
a `.fw` rewrite rule in `BUILTIN_REWRITE_RULES`):

```
(a + b) / k = a/k + b/k   iff k is a numeric literal, k != 0
(a - b) / k = a/k - b/k   iff k is a numeric literal, k != 0
```

Guard on `k` being a literal to avoid introducing `1/k` symbolic inverses
where `k` is itself variable. The like-term combiner then collapses terms
like `-b/2 + b/2` automatically as it already does.

### Improvement B — Post-derive deduplication pass

After `derive_all` produces its raw result set, run each expression
through `simplify()` once more (re-simplification may catch patterns
produced by the cross-equation-elimination strategy that weren't in
canonical form when the candidate was emitted), then `expr_to_string`,
then deduplicate the string set. With Improvement A in place, the
deduplication becomes effective — many of the 294 current outputs collapse.

### Why queue these together

- (A) alone simplifies individual expressions but doesn't reduce the output
  count — two differently-structured derivations may still produce equivalent
  forms that only match after simplification.
- (B) alone collapses exact-string duplicates but misses semantic duplicates
  whose differing forms survive simplification.
- Together: simplifier handles pattern (A), re-simplification in (B) maps
  variants to canonical forms, deduplication collapses the set.

### Cost estimate

- Improvement A: ~10-20 lines (either a `.fw` rule or a few lines in
  `simplify_div`), plus tests.
- Improvement B: ~5 lines (re-simplify + string-set dedup in `derive_all`),
  plus tests for the 294-line triangle case.

### Interaction with planned features

- `--validate` (#21) benefits — cross-checking is cheaper when the output
  set is minimal and canonical.
- `--fit` interactions: fit output already goes through simplification; no
  change.

### Post-commit follow-up: Semantic (numeric) fingerprint dedup

Distribution over addition (Improvement A) landed. It successfully reduces
individual expression complexity — `(-b/2 - c/2 + (b+4)/2 - 2)` now simplifies
to `-c/2` as intended. But `fwiz --derive 'examples/triangle(A=?, a=4, B=20, c, b)'`
still produces ~294 output lines. Diagnostic: those 294 lines are not exact-string
duplicates — they're **294 structurally-distinct derivations** produced by the
solver exploring different candidate paths. Distribution simplifies each line but
doesn't merge them.

The real fix: **semantic/numeric fingerprint dedup**. After formatting each
candidate, evaluate it at several random points in the free-variable space (small
integer coordinates, avoiding known singularities). Group candidates whose
fingerprints match to within `EPSILON_REL`. Surface one canonical form per group.

Estimated cost: ~30 lines in `derive_all` — pick 3-5 random-but-seeded test
points, evaluate each candidate, hash the result tuple, dedupe. Edge cases:
expressions that evaluate to NaN at some points (fall back to second tuple),
expressions with different valid domains (rare — accept false-dup as a
non-blocking loss).

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

## 23. `group_like` contract inversion (expr.h)

The two lambdas at expr.h:1259/1267 carry `// cppcheck-suppress constParameterReference` because they expose a `double&` write-back interface (`val(x) -> double&`). Structural fix: invert the contract to a `combine(dst, src)` callable — callee receives destination + source, writes the merged value directly, no reference escape. Eliminates both suppressions without silencing cppcheck.

**Reopen trigger:** next warnings-cleanup cycle, or any refactor of additive/multiplicative flattening in expr.h.

## 24. ~~Widen `is_one`/`is_neg_one`/`is_neg` pointer overloads to `const Expr*`~~ — **done 2026-04-19 (0708bf5)**

Widened in the M6+M7+F24 micro-cycle. No caller cascade surfaced (unlike M3/M10).

## 25. ~~M6/M7 deferred: `variableScope` and shadow renames~~ — **done 2026-04-19 (0708bf5)**

All 24 warnings cleared (8 `variableScope` + 16 shadow renames). No behavior changes; test output byte-identical.

## 26. ~~`system.h:1890` redundantAssignment bug-smell~~ — **done 2026-04-19 (6caf0a4)**

Debugger round confirmed truly-dead code (inner branch fires 2× in test suite but the else-if below independently re-finds the builtin). Four lines deleted; semantically equivalent. Findings preserved in `.fwiz-workflow/debug-findings-system-1890.md`.

## 27. Unified tolerance doctrine

Three independent thresholds were introduced across cycles without a shared rationale: `RECOGNIZE_FRACTION_MAX_DEN=360` (fit.h, fraction recognizer ceiling), `llround(v*1e9)` (expr.h, fingerprint rounding), and the pre-existing `EPSILON_REL` / `EPSILON_ZERO` (solver). These serve different concerns but should eventually be documented in one place — or consolidated where the concerns actually overlap. A "tolerance doctrine" section in Developer.md (or a single named header) that maps each threshold to its role and acceptable range would prevent future ad-hoc additions.

**Reopen trigger:** any new numeric threshold introduced outside an existing named constant.

## 28. Hook B — post-recognition re-simplification

Hook B from the 2026-04-19 dedup cycle (research-brief.md) was deferred. The idea: after `expr_recognize_constants` rewrites numeric leaves into `pi` / `e` / `deg`, re-run `simplify` so freshly-introduced symbolic nodes can fold with their neighbors (e.g. `pi * 2 / pi → 2`). The cycle's visionary/critic synthesis argued the recognizer emits opaque Var nodes, which `simplify` already handles, so Hook B would be speculative. Not shipped.

**Reopen trigger:** a specific derive-output line surfaces where post-recognition re-simplification would collapse `pi*2/pi → 2`, `deg * (180/pi) → 1`, or similar. Add the failing case to the test suite first, then re-evaluate whether Hook B is the right fix or whether a `.fw` rewrite rule suffices.

## 29. Expansion productivity gate

When `derive_all` expands an expression via Strategy 7 (cross-equation elimination), compare `canonicity_score` before and after expand + simplify; accept the expanded form only if the score decreased or the expansion enabled a new solving strategy. The fwiz-native form of this is `.fw` rewrite rules that encode preferred-form directionality — a data-driven `ComplexityFunction` analog.

**Reopen trigger:** user observes an expanded form in `--derive` output that simplifies back to a compact form already present earlier in the output.

## 30. Input-bounded derive cull

Drop derive candidates whose `leaf_count > sum(source_equation_rhs_leaf_counts)`. Zero free parameters — the bound is a closure property of the source equations, not a magic number. Requires provenance tracking in `winners` map entries (currently anonymous).

**Reopen trigger:** a `--derive` reproducer beyond triangle shows this leaf-count ratio exceeded on >10% of output lines after a real `.fw` file stress test.

## 31. `abs(x) = x iff x >= 0` builtin rewrite rule

Blocked by permissive-condition behavior breaking existing `abs` tests (`tests.cpp:8289` asserts `simplify(abs(abs(x))) == "abs(x)"` but with the rule becomes `"x"`; `tests.cpp:8291` asserts `simplify(abs(-x)) == "abs(x)"` but becomes `"x"`). The existing rule `abs(-x) = abs(x)` fires first, then `abs(x) = x iff x >= 0` (condition undetermined → permissive) fires on the result. This is a semantic correctness question — dropping sign info from `abs(-x)` is a soundness bug for unknown-sign symbolic `x`. The existing failing tests are specifications: `abs` preserves sign information until we have a principled way to discharge it.

**Reopen trigger:** when global-condition propagation to the simplifier is implemented (specifically when `condition_violated` can query `global_conditions` for domain bounds). The long-term form is `abs(x) = x iff known(x >= 0)` — guarded rather than permissive. When domain-propagation ships, the tests at `tests.cpp:8289/8291` naturally pass (`x` is unknown-sign so the guard blocks) while `abs(x) where x >= 0` simplifies as expected.

## 32. Category C architectural tautology — derivation over-enumeration

**Status after 2026-04-20 P1 cycle**: promoted from conditional to **active investigation**. The `sqrt(x)^2 = x iff x >= 0` rule shipped and correctly eliminates all `sqrt(...)^2` patterns, but the predicted fingerprint-cascade dedup did NOT reduce line count (159 → 159; 16 lines changed form but stayed distinct). Simplified forms have different free-variable signatures than canonical forms, so they fingerprint-distinctly — correct behavior, but doesn't solve the Category C problem.

The originally-proposed `target_identity_fp` approach is non-functional (`Var(target)` evaluates to empty at test points under `subst_for_fingerprint`). The next cycle must NOT re-propose a variant of this without diagnostic evidence.

**Investigation artifact**: `docs/research/category-c-investigation.md` — six speculative approaches (leaf-count gate, `--derive N` default cap, provenance-cycle detection, canonicity soft cull, algebraic closure, strategy filter) each with "why it might be right / wrong" and "cost". Five diagnostic questions (D1–D5) the next cycle should answer BEFORE designing. Explicitly speculative — the investigation prompts exploration, not prescription.

**Reopen trigger:** dedicated next cycle's RESEARCH phase takes `docs/research/category-c-investigation.md` as its starting point. Answer D1–D5 first, choose an approach, then design.

## 33. Category E symbolic form preservation for CLI bindings

The 21 lines in the triangle reproducer output using pre-evaluated numeric constants (`1.368080573 = a*sin(B*deg)`) arise because CLI-supplied `a=4, B=20` are bound numerically before derivation. Fix requires deferring binding substitution until after fingerprint-dedup — a risky change because it potentially explodes candidate count (symbolic forms of CLI-bound variables multiply through every derive branch before dedup can trim them).

**Reopen trigger:** user requests symbolic intermediate steps in derive output (e.g. `--derive --symbolic` or a stepped-derivation mode), OR scheduled batch-mode feature (see Future #5) requires preserved symbolic form for tabular output.

## 34. `x / (1/y) = x*y iff y != 0` rewrite rule — ✅ DONE (2026-04-24, <commit-hash-placeholder>)

Shipped as a builtin rewrite rule alongside the related `k * x / (k * y) = x / y iff k != 0` cancellation rule. Now also handles **numeric** denominators — `a / (1/20) → a * 20` (canonical: `20 * a`) — eliminating all 57 instances of `/ (1 / 20)` in the triangle reproducer's derive output. The original reopen-trigger ("`/(1/SYMBOL)` substring where SYMBOL is a non-numeric identifier") is now ACTIVE as a regression guard.

**Open residual** (not addressed by this rule): composite-denominator patterns like `x / ((1/k) * Y)` where the unit fraction is one factor inside a MUL. These survive (29 occurrences in the triangle output, all of form `1 / deg * acos(...) * ...` or `1 / 2 * (b+c+4) * ...`). Rewriting these would require a wider rule (`x / (a/b * y) = x * b / (a * y)`) — see Future entry to be added if/when needed.

## 35. Stale CLAUDE.md / `system.h:521` claim about `stdlib/builtin.fw` mirroring

The comment at `src/system.h:521` claims `stdlib/builtin.fw` mirrors `BUILTIN_REWRITE_RULES` for documentation. The file actually contains only builtin-function section definitions (sin, cos, sqrt, log, abs, etc.) — NOT rewrite rules. Fix the stale comment in a future cleanup pass.

**Reopen trigger:** whenever `stdlib/builtin.fw` is edited for rewrite-rule documentation purposes (i.e. whenever someone discovers the comment claims something the file doesn't deliver).

## 36. Composite-denominator unit-fraction rule: `x / ((1/a) * y) = x*a / y`

Surfaced in the 2026-04-24 Tier 1 cycle: after shipping G3 (`x/(1/y) = x*y iff y != 0`), 29 occurrences remained in the triangle reproducer of shape `x / ((1/k) * Y)` where the unit fraction is one factor inside a MUL chain. G3 (correctly) does not match — its pattern is strictly `DIV(x, DIV(Num(1), y))`, not `DIV(x, MUL(DIV(1, y), z, ...))`. Examples from the triangle output:

```
/ (1 / 2 * (b + c + 4) * (b / 2 + c / 2 - 2))
/ (1 / deg * acos((b^2 - c^2 + 16) / (8 * b)) * asin(...))
/ (1 / deg * acos((c^2 - b^2 + 16) / (8 * c)) * asin(...))
```

The rewrite `x / ((1/a) * y) = x * a / y iff a != 0` (with `a` extracted from the MUL) would flatten these. But this is a structurally different rule than G3 — `a` here must be identified as "one factor inside a MUL chain", which is a harder pattern match than G3's direct reciprocal. Would need a fresh design/critic round.

**Cost estimate**: 1 rule + 3 tests + walker extension. Mechanical once designed; the critical question is whether the existing commutative flattened matcher can express "extract one factor matching `1/a` from a MUL chain" or whether new matcher primitives are needed.

**Reopen trigger**: the 29 residual occurrences visible in `./bin/fwiz --derive "examples/triangle(A=?, a=4, B=20, c, b)" | grep -c "/ (1 / "` — any time that count is non-zero after this entry lands. Alternatively: when the composite pattern is observed in another reproducer's output.

**Update (2026-04-24, post-rebuild_multiplicative split-by-sign cycle):** The
rebuild fix cascades modestly with G3: since `MUL(_, POW(_, -1))` factors now
emit as `DIV(_, _)` directly, some chains that previously appeared as
`MUL(DIV(1, a), y)` are no longer constructed in the first place — their
upstream sources (`(1/a) * y` after rebuild on `x * a^(-1) * y`) emit cleaner
forms. Triangle measurement: `/ (1 / ` count dropped 29 → 26 (-3). The bulk
of the residual is unrelated to negative exponents (e.g. `1 / deg * acos(...)`
constructed directly from a deg-multiplication, not from a `^(-1)` factor).
Entry remains valid; not obsolete.

## 38. `x^(-n)` rendering as `1/x^n` for any integer n — ✅ DONE-BY-SIDE-EFFECT (2026-04-24)

Originally tracked as a residual: `simplify_pow`'s standalone case
(`expr.h:1759-1765`) handled `x^(-n)` outside any MUL chain, but the moment
the POW-with-negative-exponent was wrapped in a MUL chain, the
`rebuild_multiplicative` factor-emit loop would re-emit `POW(base, Num(-n))`
unconditionally, undoing the cleanup.

**Resolution:** `rebuild_multiplicative` (`src/expr.h`, ~lines 1296-1330) was
rewritten to split factors by exponent sign: positive exponents → numerator
product; negative exponents (with sign flipped) → denominator product;
emit `DIV(num, denom)` when any negative-exp factors exist. Walker assertion
(`tests.cpp` M3-6 block) pins the invariant: no `^(-` substring in derive
output for the triangle reproducer. Triangle measurement: 66 `^(-` substrings
→ 0; 159 lines → 158; 42024 chars → 40983.

## 39. Shared CSE helper preamble across `--table` rows

`--cse` (Cycle B) extracts subexpressions per `derive_all` invocation. A
future `--table` mode that emits multiple parameterized rows (e.g. one row
per `(a, b, c)` triple) should share a single `# Helpers` preamble across
all rows when the structural shape repeats. Reusing the existing
`cse_extract` over the union of row-expressions gives this for free; only
the print block needs new layout logic.

**Reopen trigger**: when `--table` is designed.

## 40. Chain-rule CSE composition for symbolic differentiation

`cse_extract` and `cse_replace` (Cycle B) are general-purpose structural
primitives. When `--derive dY/dX` lands, intermediate chain-rule terms
(e.g. `dY/du * du/dx` where `u` repeats across multiple bindings) are exact
candidates for the same dedup pipeline.

**Reopen trigger**: `--derive dY/dX` design phase.

## 41. LaTeX helper rendering

LaTeX output (`--latex`) for `--cse` should render the helper preamble as
`\text{Let } t_1 = \ldots` in a `\begin{align}` block, with main equations
referencing `t_i`. The structural separation already exists in the
`--cse` output stream (helpers vs main equations); `--latex` only needs
a different formatter.

**Reopen trigger**: `--latex` is designed.

## 42. Cross-tier rewrite rule `(a/b)^n * b^n = a^n iff b != 0`

Tier 1.x's `rebuild_multiplicative` renders `(a/b)^2 * b^2` as-is rather
than collapsing to `a^2`. With `--cse` active, a helper `t1 = (a/b)^2`
can survive next to a main term `t1 * b^2` — visually noisy. Upstream
fix is a `.fw` rewrite rule, not a CSE-side fix (per CLAUDE.md
"simplification over filtration"): adding the rule simultaneously cleans
non-CSE output and reduces CSE candidate count.

**Reopen trigger**: a user reports the non-collapse, OR the next
rewrite-rule cycle.

## 44. Raw-provenance inspection mode (`--symbolic`)

`solved_symbolic_` (provenance-plumbing cycle, 2026-04-26) stores the post-recognition ExprPtr — i.e. the recognized symbolic form, not the raw pre-recognition solver tree. For normal trace output this is the right policy (consistent with final output). If a future `--symbolic` or LLM-debug mode needs access to the pre-recognition solver tree, a separate `solved_symbolic_raw_` member would be needed alongside the existing `solved_symbolic_`.

**Reopen trigger (R3):** a user requests `--symbolic` mode or an LLM debug-inspection surface that needs the raw solver ExprPtr before constant recognition. Do NOT add `solved_symbolic_raw_` preemptively.

## 45. `aliases_` second consumer

`FormulaSystem::aliases_` (provenance-plumbing cycle, 2026-04-26) is the universal alias-resolution table — a `mutable std::map<std::string, double>` cached by `build_alias_table()` and read by `fmt_trace`/`fmt_exact_double`. When LaTeX export (#9) or units (#7) ships, confirm the member generalizes to that renderer and rename if it has grown a more descriptive role.

**Reopen trigger (R4):** LaTeX (#9) or units (#7) enters a planning cycle. Check whether `aliases_` serves both use cases; rename or split at that point, not before.

## 46. T1 (`trace_loaded`) provenance

T1 — the `trace_loaded()` call that emits file defaults at load time — was intentionally left at `fmt_num` in the provenance-plumbing cycle. At that call site, `aliases_` is not yet populated (it is built on first `build_alias_table()` call, which happens inside `resolve()`). Fix options: defer `trace_loaded` output until after the first `build_alias_table()` call, or call `build_alias_table()` eagerly during load.

**Reopen trigger (R6):** a user reports that `--steps` shows a decimal for a user-named constant at the "loading" line (e.g. `deg = 0.01745329252` rather than `pi / 180`).

## 43. Per-`.fw` CSE cap frontmatter

`--cse 3` is the default. Option C's value-rank semantics reduce the need
for per-`.fw` tuning — the cap-N model ranks by `(occurrences - 1) * (leaves - 1)` and takes
the top N, so the same default works across most domains. A `.fw` file with unusual structure (very flat, very deep, or
highly repetitive non-semantic atoms after canonicalization) might still
benefit from a frontmatter directive (e.g. `# fwiz: cse_default 5`) that
sets the file's preferred cap. CLI `--cse N` would still override.

**Reopen trigger**: a user reports the default cap of 3 is wrong for their
domain AND a different `value` formula would not fix it (i.e. the cap
itself is the issue, not the ranking).

## 47. Higher-order `diff(f, x, n)` sugar

`diff(diff(f, x), x)` already works via composition — the output of `symbolic_diff` is a valid expression that can be fed back into `symbolic_diff`. A sugar form `diff(f, x, 3)` would expand to `n` nested calls at parse time. Opt-in when a third argument is present; the two-argument form is unchanged.

**Reopen trigger:** a user requests the shorthand, OR a `--derive` use case requires high-order derivatives (e.g. Taylor series output).

## 48. Generic `resolve_at_load(rewriter)` mechanism

`resolve_diff_in_equations` (system.h) is the first post-load tree-rewriter. When a second consumer wants the same pattern — integrals (#16), units (#7), LaTeX hints (#9) — factor out the recursive visitor into a `resolve_at_load(rewriter_fn)` primitive that `FormulaSystem::load_with_sections()` invokes for each registered rewriter in order.

**Reopen trigger:** a second feature needs a post-load tree-rewriting pass.

## 49. Per-builtin metadata registry

`symbolic_diff`'s FUNC_CALL case is an inline if-chain over 9 builtin names. When a second consumer of per-builtin metadata appears (e.g. an antiderivative table for future #16, or a LaTeX renderer for #9), refactor the chain into a shared registry (`map<string, BuiltinMeta>`) that stores derivative rule, antiderivative rule, and LaTeX form together.

**Reopen trigger:** a second consumer of per-builtin metadata (antiderivative table, LaTeX rendering, dimensional annotation) enters a planning cycle.

## 50. `diff(formula_call, var)` corner cases

The post-load pass (`resolve_diff_in_equations`) inlines formula-call bodies for `diff(formula_call_placeholder, var)` via `unfold_formula_call_for_diff`. Corner cases deferred: piecewise formula calls (multiple RHS branches — which branch to differentiate?), multi-return formula calls (which output var?), and formula calls with expression bindings that themselves contain `diff`. Revisit when Future #20 typed FORMULA_CALL nodes land (giving stable node identity for the chain rule), or when >2 user reports of unexpected behavior surface.

**Reopen trigger:** Future #20 (typed FORMULA_CALL nodes) enters a planning cycle, OR >2 user reports of unexpected `diff(formula_call, var)` behavior.

## 51. Piecewise / conditional formula-call diff (multi-branch)

When `diff(formula_call, var)` targets a sub-system with multiple equations defining the output (e.g., `abs` via two `iff` branches: `result = x iff x >= 0` and `result = -x iff x < 0`), the post-load pass `unfold_formula_call_for_diff` currently uses only the first equation's RHS. The correct behavior depends on which branch is active at evaluation time — this requires either evaluating conditions symbolically (and folding them into a piecewise derivative) OR returning a piecewise result expression. Today the user silently gets one branch's derivative.

**Reopen trigger:** user reports unexpected derivative of a piecewise formula call.

## 52. Test coverage for `diff(...)=?` range-ValueSet output path

The `diff(...)=?` query path returns a `ValueSet` (CLI Surface 2) and so should support range/interval results in addition to discrete values. Polish-pass Item 6 attempted to construct a CLI-level reproducer for the range branch but found that range-valued constraints on RHS variables (e.g., `slope = a` with `a > 1, a < 5`) do not propagate through `resolve_all` to the LHS — this is a structural gap independent of `diff()`. A range-result test for `diff(...)=?` therefore requires either (a) extending `resolve_all` to propagate constraint ranges through equation chains, or (b) constructing a derivative whose internal evaluation directly produces a `ValueSet` interval.

**Reopen trigger:** range-propagation through `resolve_all` lands (independent feature), OR a user surfaces a `diff(...)=?` query whose natural answer is an interval.

## Interaction with existing features

- **--verify**: conditions become part of verification — check that inputs satisfy all relevant conditions
- **--derive**: output conditions alongside derived equations
- **--explore**: show which conditions are satisfiable with given inputs
- **Cross-file calls**: conditions in sub-systems are checked when resolving through formula calls
