# Future: Planned Features

## Motivation

Features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

> Shipped features and cleanup cycles live in `docs/COMPLETED.md`. Numbering matches across the two files — `#22` here is `#22` there.

## 4. Numeric Solving — Remaining enhancements

Core landed; see COMPLETED.md #4.

- Periodicity detection for functions with infinitely many roots (e.g., `sin(x) = 0.5`)
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

## 10. Fraction Representation — Remaining enhancements

Core landed; see COMPLETED.md #10.

- Rational propagation in `evaluate()` for exact intermediate results

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

## 11. Curve Fitting — Remaining enhancements

Core landed; see COMPLETED.md #11.

- Rational (Padé) approximation: `p(x)/q(x)` for better convergence near singularities
- Sum-of-products inners: `a*f(x) + b*g(x)` for Stirling-type approximations

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

## Open cleanup-cycle reopen triggers

Carried forward from completed T1 and T2+T3 cleanup cycles (see COMPLETED.md). Each fires under a specific condition.

- **T3.2** (`expr_to_string` on simplifier hot path, `expr.h:1479`) — trigger: profiling shows string allocations dominating `simplify_div`. Fix: store `ExprPtr` in `SimplifyAssumption`, defer `expr_to_string` to output site.
- **T3.3** (`cse_extract` keys by string, `system.h:277`) — trigger: `--cse` output is visibly slow on large derive results (>50 candidates). Fix: structural hash (integer mixing, no allocation).
- **T4.1** (file split: `numeric.h` first, then `query.h`) — trigger: when `system.h` exceeds 4000 LOC, or when a new contributor asks "where does CLI parsing live?" Post-T1, `system.h` is ~3580 LOC; extract `numeric.h` (700 LOC, newton/bisection/adaptive_scan boundary at `try_resolve_numeric`) first, then `query.h` (200 LOC, CLI query parsing) if still warranted. **T3.8 payload**: include the `numeric_results_` → `result_is_exact_` rename as part of the file-split atomic diff (deferred from T2+T3 M3 to avoid dual-churn).
- **T4.2** (`SolveContext` struct replacing mutable mode flags) — trigger: a second mutable solve-mode flag is added to `FormulaSystem`, OR Future #56 escalation lands. Fix: explicit `SolveContext` passed through the solve chain.
- **`tree_map` creep guard** — trigger: a new `tree_map`/`tree_map_leaf` caller is added in a subsequent cycle without first checking whether a `.fw` rewrite rule subsumes it. Baseline: 5 callers post-T1. Threshold: >7 callers without rule-equivalence justification triggers re-review.
- **Condition-in-expr.h spread** — trigger: a second non-rewrite-rule consumer in `expr.h` starts holding `Condition` (e.g. simplifier internals consulting global conditions per Future.md #31). At that point, "should `Condition` be a first-class part of the simplifier contract?" is open and warrants design.
- **M3 description-string regression** — trigger: a user reports a `--steps`/`--calc` trace where the assumption text is less informative post-T1. Fix: `condition_to_string` is already the intended solution; trigger fires only if its output is itself unsatisfactory.

---

## 23. `group_like` contract inversion (expr.h)

The two lambdas at expr.h:1259/1267 carry `// cppcheck-suppress constParameterReference` because they expose a `double&` write-back interface (`val(x) -> double&`). Structural fix: invert the contract to a `combine(dst, src)` callable — callee receives destination + source, writes the merged value directly, no reference escape. Eliminates both suppressions without silencing cppcheck.

**Reopen trigger:** next warnings-cleanup cycle, or any refactor of additive/multiplicative flattening in expr.h.

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

**Reopen trigger:** when global-condition propagation to the simplifier is implemented (specifically when `check_condition` can query `global_conditions` for domain bounds). The long-term form is `abs(x) = x iff known(x >= 0)` — guarded rather than permissive. When domain-propagation ships, the tests at `tests.cpp:8289/8291` naturally pass (`x` is unknown-sign so the guard blocks) while `abs(x) where x >= 0` simplifies as expected.

## 32. Category C architectural tautology — derivation over-enumeration

**Status after 2026-04-20 P1 cycle**: promoted from conditional to **active investigation**. The `sqrt(x)^2 = x iff x >= 0` rule shipped and correctly eliminates all `sqrt(...)^2` patterns, but the predicted fingerprint-cascade dedup did NOT reduce line count (159 → 159; 16 lines changed form but stayed distinct). Simplified forms have different free-variable signatures than canonical forms, so they fingerprint-distinctly — correct behavior, but doesn't solve the Category C problem.

The originally-proposed `target_identity_fp` approach is non-functional (`Var(target)` evaluates to empty at test points under `subst_for_fingerprint`). The next cycle must NOT re-propose a variant of this without diagnostic evidence.

**Investigation artifact**: `docs/research/category-c-investigation.md` — six speculative approaches (leaf-count gate, `--derive N` default cap, provenance-cycle detection, canonicity soft cull, algebraic closure, strategy filter) each with "why it might be right / wrong" and "cost". Five diagnostic questions (D1–D5) the next cycle should answer BEFORE designing. Explicitly speculative — the investigation prompts exploration, not prescription.

**Reopen trigger:** dedicated next cycle's RESEARCH phase takes `docs/research/category-c-investigation.md` as its starting point. Answer D1–D5 first, choose an approach, then design.

## 33. Category E symbolic form preservation for CLI bindings

The 21 lines in the triangle reproducer output using pre-evaluated numeric constants (`1.368080573 = a*sin(B*deg)`) arise because CLI-supplied `a=4, B=20` are bound numerically before derivation. Fix requires deferring binding substitution until after fingerprint-dedup — a risky change because it potentially explodes candidate count (symbolic forms of CLI-bound variables multiply through every derive branch before dedup can trim them).

**Reopen trigger:** user requests symbolic intermediate steps in derive output (e.g. `--derive --symbolic` or a stepped-derivation mode), OR scheduled batch-mode feature (see Future #5) requires preserved symbolic form for tabular output.

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

## 53. Typed-binding predicates in `.fw` rule conditions

Extend the rule-condition language with predicates `is_num(x)`, `is_neg_num(x)`, `is_int(x)` that test the runtime binding of a wildcard: `is_num(x)` is true only when `x` binds to a numeric literal — NOT permissive-unknown (unknown → false, not true). This is the foundational extension that would unblock three blocked migrations and one blocked rule: T3.5 (rational arithmetic in `simplify_div`), T3.6 (`x^(-n)` rendering), and Future #31 (`abs(x) = x iff x >= 0`). Without this, the rule engine's wildcard semantics treat any binding — symbolic or numeric — identically, making it impossible to safely restrict a rule to numeric-only operands.

**Reopen trigger**: a third C++ simplifier block resists migration for the same reason (wildcard binds both numeric and symbolic, rational arithmetic is C++-only), OR Future #31 (`abs(x) = x iff x >= 0`) is reopened, OR T3.5/T3.6 are reopened.

## 54. T3.5 non-migration: constant reassociation in `simplify_div`

The constant-reassociation block in `simplify_div` (expr.h) cannot migrate to `.fw` rewrite rules. Root cause: the block extracts numeric factors from symbolic expressions using `make_rational` — a C++-only operation — and rebalances the numeric side. The rule engine's wildcard semantics bind `b` to any expression (symbolic or numeric) indistinguishably, so a rule condition like `iff is_num(a)` cannot be expressed today.

**Reopen trigger**: typed-binding predicates (Future #53) ship AND `make_rational` is callable from rule RHS evaluation.

## 55. T3.6 non-migration: `x^(-n) → 1/x^n`

The `x^(-n)` rewriting in the simplifier cannot migrate to a `.fw` rewrite rule. Root cause: permissive-condition semantics would treat `x^y` (symbolic exponent) as satisfying any numeric-sign condition on `y` (unknown → permissive), causing infinite rewriting loops on symbolic exponents. The check that `n` is a negative numeric literal requires `is_neg_num(n)` — a typed-binding predicate not yet in the rule engine.

**Reopen trigger**: typed-binding predicates (Future #53) ship.

## 56. Issue 1 severity escalation option

The T2+T3 M1 fix drops parse-failed rewrite rules at load time with a stderr warning (`"warning: dropping rewrite rule '…' — malformed condition: …"`). If the load-time warning proves insufficient for diagnostics — e.g. a user's `.fw` file silently loses rules and they cannot see why — add a `bool condition_parse_failed` flag to `RewriteRule` and surface it at `compute_rewrite_groups` (option (b) from the original fix discussion). Today option (d) "drop silently to stderr" was chosen as the smallest correct delta; option (b) is available as a follow-up if the warning surface is too weak.

**Reopen trigger**: a user reports they lost a rewrite rule silently (i.e. did not see the stderr warning) due to a malformed condition string in their `.fw` file, OR the T4.2 `SolveContext` structural fix absorbs this anyway.

## 57. recognize_constant: std::map → sorted std::array

`base_recognition_constants()` in `fit.h` currently uses `std::map<std::string, double>` as the backing store for the merged builtins+sqrt/log table (9 entries). Iteration is alphabetical (red-black tree order), which is what `recognize_constant` and downstream fingerprint-dedup depend on. At 9 entries the tree fits in ~5 cache lines and is L1-warm after the first call, so real-world impact is negligible. If the constant table grows past ~20 entries OR `recognize_constant` becomes a measurable hot path under `--fit`/`--derive`, replacing the `std::map` with a `constexpr`-sorted `std::array<std::pair<const char*, double>>` would convert pointer-chasing tree traversal into a sequential scan.

**Reopen trigger**: constant table size > 20 entries, OR perf-auditor flags `recognize_constant` as measurably hot in a future cycle.

## Interaction with existing features

- **--verify**: conditions become part of verification — check that inputs satisfy all relevant conditions
- **--derive**: output conditions alongside derived equations
- **--explore**: show which conditions are satisfiable with given inputs
- **Cross-file calls**: conditions in sub-systems are checked when resolving through formula calls
