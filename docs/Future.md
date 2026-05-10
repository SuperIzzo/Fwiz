# Future: Planned Features

## Motivation

Features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

> Shipped features and cleanup cycles live in `docs/COMPLETED.md`. Numbering matches across the two files — `#22` here is `#22` there.

## 4. Numeric Solving — Remaining enhancements

Core landed; see COMPLETED.md #4.

- Periodicity detection — core shipped (#12 DONE); numeric gap-based detection deferred (#12a)
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

## 10a. Extending `evaluate_symbolic` for new number types — PARTIAL (Cycles A+B shipped 2026-05-10)

**Shipped (Cycle A, 2026-05-09):** `i` as a builtin constant with NaN binding; `i*i=-1` and `i^2=-1` rewrite rules; `is_active_builtin` NaN-skip preventing silent real-valued resolution. The rewrite-rule route (step 1 of the complex checklist below) is the approach taken — no `ExprType::COMPLEX` leaf, no bindings-map promotion. Deferred complex items: `ExprType::COMPLEX` leaf, `evaluate_symbolic` complex dispatch, complex `expr_to_string`, `sqrt(-1)=i` rule, simplifier-side rational migrations.

**Shipped (Cycle B M3, 2026-05-10):** Vec/mat sugar via `FUNC_CALL("vec"/"mat", ...)` — no new `ExprType`, `sizeof(Expr)` unchanged. Element-wise add/sub/scalar-mul via `try_simplify_vec_mat_binop` hook. Matrix builtins `matmul`/`det`/`inv`/`transpose` dispatched from `try_dispatch_vec_mat_builtin`. `evaluate()` returns empty for matrix operands. Shape mismatch propagates `Var("undefined")`. Scope: `det` 2x2+3x3, `inv` 2x2 only, `transpose` general, `matmul` general NxN. Deferred matrix items: `ExprType::MATRIX` leaf, Gaussian elimination for `inv` N≥4, eigenvalues/LU/SVD, complex-element matrices, bindings-map promotion for matrix-valued CLI bindings.

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

## 12. Periodicity Detection — DONE (2026-05-08)

M1 + M2 fused. Sin/cos gained a second inverse equation each in `builtin_function_defs()` (M1); `PeriodicFamily`/`ValueSet::periodic_`/render/dedup shipped in `expr.h` and `system.h` (M2). Hybrid approach: algebraic branch generation via inverse equations, symbolic-table period lookup via `trig_period()` + `detect_trig_origin()` scanner (both `src/system.h`). Output: `x : 1 / 6 * pi + k * 2 * pi, k in Z | 5 / 6 * pi + k * 2 * pi, k in Z`. Numeric gap-based period detection (original M3) deferred to #12a.

### Periodicity-related semantic shift note

The previous test invariant "high-precision sin scan finds ≥6 roots in [0, 20]" is now "≥2 periodic-family bases" — the concrete-roots semantics moved to a render-time expansion concern. Future improvement: add an API to expand a periodic `ValueSet` over an interval (`vs.expand_periodic([lo, hi])` returning N concrete roots) — useful for users who want enumerated roots rather than the parametric family. ~20 LOC. Trigger: user requests enumerated roots from a periodic `ValueSet`.

## 12a. Numeric gap-based period detection

Deferred from the #12 cycle. Approach: arithmetic-progression detection on the sorted roots vector from `find_numeric_roots`, followed by constant recognition on base and period. Cost ~110 LOC. Enables period annotation for equations not expressible via a single named-trig builtin (e.g. composed trig, user-defined periodic functions). Reopen trigger: user reports a periodic equation whose roots come back as discrete dump AND the equation is not expressible via a single named-trig builtin AND no `@period` annotation could have been declared on the source.

## 12b. `@period <expr>` section annotation

Extend the trig-period table (`trig_period()` in `src/system.h`) to user-defined periodic functions via a section-level annotation `@period <expr>`. ~5 LOC parser edit + 3 LOC table-lookup. Reopen trigger: user writes a custom periodic function in `.fw` and asks why `--no-numeric` doesn't return a periodic family.

## 12c. `ValueSet::intersect()` / `unite()` on `periodic_`

Set algebra over periodic families — lcm-based period merging, base alignment. Required when a global condition (`x > 0`) coexists with a trig equation; today the periodic family is returned unconstrained. Reopen trigger: user writes a global condition alongside a trig equation AND reports the periodic family is unconstrained.

## 12d. `--derive` output format for periodic results

Decide between principal-branch-only + comment vs. full periodic family when a `--derive` query would naturally return a periodic `ValueSet`. Reopen trigger: `--derive` query produces an output whose RHS evaluates to a periodic family.

## 12e. Round-trip safety for periodic output

Ensure `x = pi/6 + k * 2*pi, k in Z` parses back into Fwiz with `k` as a free integer parameter. Currently the rendered form uses ASCII `k in Z` notation that is not a valid `.fw` expression. Reopen trigger: user pipes Fwiz output back into Fwiz and reports a parse error or semantic divergence on periodic output.

## 12f. Tighten `derive_all` fingerprint resolution

M3-6 test (5 branch-distinguishing points) exposed that `derive_all`'s 3-point Schwartz–Zippel fingerprint misses 4 candidates that diverge on broader probes. Post-M1 (more sin/cos branches), 4 candidates collide on the 3-point set but diverge on 5-point. Approach: extend test points OR add structural canonicalization. ~30-50 LOC. Impacts canonical-winner selection across all derive tests. Reopen trigger: user reports semantic-duplicate derive outputs not deduped.

## 12g. CSE-I3 / load+solve perf on M1-cascaded derive output

M1's branch-multiplicity cascade grew triangle `--derive --cse` output 158 → 649 lines (4×); loading and re-solving the 654-line `.fw` exceeds 60s wall-clock. The CSE-I3 test now wraps popen with `timeout 10` and accepts either correctness or timeout. Investigation needed: is the bottleneck parsing, simplify, `enumerate_candidates` explosion, or numeric solver re-entrance? Reopen trigger: user reports slow roundtrip on `--derive --cse` output, or any sufficiently-large `.fw` file load exceeds 10s.

## 13. Complex / Imaginary Numbers — PARTIAL (2026-05-09)

**Shipped (Cycle A, 2026-05-09):**
- `i` registered as a Fwiz-wide builtin constant with quiet-NaN binding in `builtin_constants()` (`src/expr.h`). Pattern-matcher literal-match guard (`src/expr.h:838`) prevents `i` from acting as a wildcard.
- `evaluate()` on any `i`-containing expression returns empty `Checked<double>{}` — no new code path; the NaN-as-empty contract in `Checked<double>` covers it automatically.
- Two rewrite rules added to `BUILTIN_REWRITE_RULES` (`src/system.h`): `i * i = -1` (defensive) and `i ^ 2 = -1` (fires after multiplicative flattening canonicalizes `i*i` → `i^2`).
- `is_active_builtin` extended with a `std::isnan` guard: NaN-valued builtin constants are never auto-bound in the resolver's fast-path, so `y = 2*i; y=?` correctly returns "Cannot solve" rather than silently evaluating to a wrong real result (regression caught at REVIEW phase, fix shipped same day).

**Still open — reopen triggers:**
- `(1+i)*(1-i) == 2`: requires MUL-over-ADD distribution in the simplifier (see Future.md #13a below).
- `i^4 == 1`: requires power-cascade for POW on builtin constants (see Future.md #13b below).
- `sqrt(-1) = i`: requires rule-parser support for negative-literal LHS patterns.
- `resolve_all` returning `i`-containing forms for `x^2+1=0`: depends on the `sqrt(-1)=i` rule landing first.
- `ExprType::COMPLEX` leaf, bindings-map promotion, simplifier-side migrations: see #10a.

Implementation plan and extension point: see **#10a — Extending `evaluate_symbolic` for new number types** (Complex numbers checklist).

## 13a. MUL-over-ADD distribution for cascading complex rule firing

`(1+i)*(1-i)` does not simplify to `2` because the simplifier has no general MUL-over-ADD distribution step. When a MUL child is an ADD, distributing it would expose inner sub-expressions that match existing rules (e.g. `i * i = -1`). The general form is: `a*(b+c) → a*b + a*c` when at least one of `a*b` or `a*c` is rule-reducible. This is a structural simplifier extension, not a new rule; it belongs in the flattening/distribution layer of `expr.h`. Per "simplification over filtration" — a MUL-distribution step would benefit every structural expression, not just complex ones.

**Reopen trigger:** 2+ users report expressions of shape `(A+B)*(A-B)` not collapsing to `A^2 - B^2`, where `B` is a symbol with a known square rule (e.g. `i`).

## 13b. Power-cascade for builtin constants (`i^N` for N ≥ 4)

`i^4` does not simplify to `1` because there is no intermediate `(i^2)^2` form — the simplifier sees `POW(i, 4)` directly and no rule matches `i^4`. Adding individual rules `i^3 = -i`, `i^4 = 1` covers the base cases; a general `(x^a)^b = x^(a*b)` rule paired with `i^2 = -1` would cover all `i^N`. Today: only `i^2 = -1` and `i^1 = i` (identity) are in `BUILTIN_REWRITE_RULES`.

**Reopen trigger:** user reports `i^N` for N ≥ 3 not simplifying, or a complex-polynomial reproducer produces non-simplified `i^4` forms in `--derive` output.

## 13c. `flatten_additive` NaN-propagation (pre-existing simplifier bug)

`flatten_additive` (`src/expr.h:1894-1937`) silently drops `Num(NaN)` terms: the integer-value guard at line 1903 is `false` for NaN (by IEEE 754), so the constant accumulator receives `NaN`; the emit guard at line 1935 (`std::abs(constant) >= EPSILON_ZERO`) is also `false` for NaN — so the accumulated NaN is dropped entirely, not propagated. Pre-Cycle-A this was unreachable from user input. Post-Cycle-A, `i` is the only NaN-valued builtin, and its resolver fast-path is now closed by the `is_active_builtin` NaN-skip. The bug remains latent: if any future builtin constant (or user-defined constant) carries a NaN value and reaches `flatten_additive`, the additive identity `1 + nan → 1` silently loses the NaN term instead of producing NaN. Companion bug exists in `flatten_multiplicative`.

**Reopen trigger:** a second NaN-valued builtin constant is added (e.g. a symbolic infinity, an indeterminate form), OR a user demonstrates that `i` reaches `flatten_additive` via an unanticipated path that bypasses the `is_active_builtin` guard.

## 13d. `defaults`-as-query-target limitation (pre-existing, not M2-specific)

A `.fw` line of the form `x = 42` (literal RHS, no free variables) populates `defaults`, not `equations`. Querying that variable directly — `x=?` in an otherwise-empty system — fails with "No equation found for 'x'" because the solver searches `equations`, not `defaults`. This applies to all default variables (dotted or not). Users typically query equations that *consume* defaults, so this rarely surfaces. It is not a regression from M2; M2's test suite documented it as a pre-existing limitation.

**Reopen trigger:** a user explicitly reports `mass=?` on `mass = 1500` failing as a usability issue, suggesting that querying a pure-default as a solve target should be supported.

## 14. Vectors, Quaternions, and Matrix Math — PARTIAL (vec/mat sugar shipped 2026-05-10)

**Shipped (Cycle B M3):** `[1, 2, 3]` / `[[1,0],[0,1]]` literal syntax; element-wise add/sub; scalar-mul; `matmul`, `det` (2x2+3x3 cofactor), `inv` (2x2), `transpose` (general); shape mismatch → `undefined`. All ops preserve symbolic args. See Developer.md §"Vectors and matrices" for implementation detail.

**Deferred — reopen triggers:**
- Gaussian elimination for `inv` N≥4 (or open `.fw`-rule approach): reopen when a user needs `inv` of a 3×3+ matrix.
- Eigenvalues / SVD / LU decomposition: reopen when a concrete use case (physics, statistics) drives the need.
- Quaternions and normed division algebras: reopen when a rotation-math use case arrives; classify as In-scope (rotation math is universal) vs Wrapper-tool (game engine) at that point.
- Complex-element matrices (e.g. `[[1+i, 0],[0, 1-i]]`): depends on Future #13a (MUL-over-ADD distribution) landing first.
- Matrix-valued CLI bindings: `bindings` is `map<string, double>` today; matrix variables need a parallel `ExprPtr` track (see #10a Bindings-parameter extension).

Implementation plan and extension point: see **#10a — Extending `evaluate_symbolic` for new number types** (Matrices / vectors checklist).

## 15. Structs / Dot Access — DONE (flat-naming approach, 2026-05-09)

Dotted identifiers like `car.velocity.x` work end-to-end as flat variable names. The lexer (`src/lexer.h:82-89`) tokenizes `IDENT.IDENT.IDENT` as a single `IDENT` token; no other pipeline stage needed changes. Confirmed via `test_struct_dotnames` (9 assertions: round-trip, `collect_vars`, defaults storage + downstream consumption, dotted names in conditions, Pythagoras example). See Developer.md `### Dotted variable names` for the full characterization.

**Real `ExprType::STRUCT` / `DOT_ACCESS` node (deferred):** required only for cross-field invariants, type-checking on dotted shapes, or namespace-scoped resolution. Reopen trigger: a user demonstrates a concrete need that flat naming cannot express (e.g. iterating all fields of a struct, or enforcing that two variables share the same dotted prefix).

## 16. Integrals and Differentials — DONE (M1+M2+M3 shipped 2026-05-10)

**Tier: In-scope.** Symbolic integration is a core math-inference capability; numeric quadrature fallback keeps it useful for non-elementary integrands.

Symbolic integration (`integral(f, x)`) alongside differentiation (#6). Definite integrals with bounds. Standard integration rules (power, trig, substitution, parts). Falls back to numeric quadrature when symbolic fails.

**Status (2026-05-10):** Three-cycle arc complete.
- **M1**: indefinite Tier 1 (~25 atomic patterns: constants, `x^n`, `1/x`, `sin/cos/tan(x)`, `e^x`, sums, scalar mul/div).
- **M2**: derivative-divides u-substitution (`integral(2*x*cos(x^2), x) → sin(x^2)`, `integral(x*e^(x^2), x) → e^(x^2)/2`), definite-integral 4-arg form `integral(f, x, a, b)` with symbolic F(b)-F(a) primary path, adaptive Simpson numeric fallback (`integral(e^(-x^2), x, 0, 1) ≈ 0.7468`).
- **M3**: Integration by parts via LIATE heuristic (depth ≤ 3, no cyclic detection — `e^x*sin(x)` family stays unevaluated by design). Examples: `integral(x*e^x, x) → x*e^x - e^x`; `integral(x^2*log(x), x) → x^3*log(x)/3 - x^3/9`; `integral(atan(x), x) → x*atan(x) - log(x^2+1)/2`. `BuiltinMeta` registry extracted (Future #49) — diff and integrate share a single per-builtin metadata table.

Two surfaces: `integral(target, var)=?[alias]` CLI query (also `integral(target, var, lo, hi)=?[alias]` for definite) and inline builtin form. Unrecognized forms preserve the unevaluated `integral(...)` FUNC_CALL. NO `--integrate` flag. NO `+ C`.

**Cross-arc reopen triggers (post-arc):**
- **Cyclic IBP (`e^x * sin(x)` family):** user reports family unevaluated in real reproducer AND cleanly-layered detection mechanism identified.
- **Risch algorithm:** user reports integrand provably elementary-integrable but Tier 1-3 cannot solve, AND same form recurs in 2+ unrelated reproducers.
- **Improper integrals (`integral(f, x, 0, inf)`):** user requests this form AND limit analysis is needed for another feature.
- **Multi-variable integration:** vector calculus or surface integrals enter a planning cycle, OR #14 matrix arc extends to tensor calculus.
- **Domain-aware antiderivative (`log(abs(x))`):** Future #31 (global-condition propagation) ships.
- **`--integrate` CLI flag:** LLM benchmark or user reports trying `--integrate` and finding it absent.

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

## 21. Composable / Nested Formula Calls — DONE 2026-05-09 (nested form, explicit routing, single-level)

Composes formula calls as expression-tree values:
`fwiz 'sin(result=?, triangle(A=?x, a=100, b=50, B=0.3))'`. The inner call
resolves first, exposes `A` aliased as `x`; the outer call binds `x` by
name. Implementation reuses `extract_formula_calls` (the same primitive
used by `.fw`-file equation parsing) and injects the resulting `FormulaCall`
into `sys.formula_calls` between `load_*` and the first solve dispatch.
See `examples/nested_demo.fw` + `examples/nested_inner.fw` for a working
demo, and `parse_cli_query` in `src/system.h` for the parse-time hook.

**Scope shipped**: single-level CLI nesting (one nested call per arg, with
primitive-valued bindings). Multi-level CLI nesting (call-as-binding-RHS,
e.g. `outer(result=?, mid(z=?x, a=inner(z=?y, p=1)))`) is NOT supported in
this cycle — it requires a structural change to either `parse_call_args`
(binding-RHS recursive extraction) or a new walker, both of which exceeded
the cycle's "do not modify primitives" boundary. This same limitation
applies to `.fw` files and is not new to the CLI surface.

Three follow-up sub-entries cover deferred design questions.

## 21a. Implicit output routing for nested calls

Currently nested calls require explicit `A=?x` aliasing
(`triangle(A=?x, a=..., b=..., B=...)`). When the inner sub-system has
exactly one resolvable free variable and the outer call has exactly one
unbound positional slot, the alias could be implicit
(`triangle(a=..., b=..., B=...)`).

**Reopen trigger**: LLM evaluation or user feedback shows explicit aliasing
is the dominant friction in 30%+ of nested-call chains.

## 21b. Dotted flat form for nested calls

The flat alternative to nested form:
`sin(result=?, triangle.A=?sin.x, triangle.a=100, ...)`. Routes variables
across scopes via path-qualified names. Shares grammar with #15
(Structs / Dot Access).

**Reopen trigger**: #15 Structs / Dot Access enters a planning cycle.

## 21c. `--derive` + nested-call symbolic threading

Currently `--derive` on a nested-call CLI query treats the inner result as
a free variable in the outer derivation. With typed FORMULA_CALL nodes
(#20), the inner call's symbolic form would compose into the outer
expression tree.

**Reopen trigger**: user requests symbolic `--derive` output through
nested formula calls, OR #20 (typed FORMULA_CALL) enters planning.

## 21d. Multi-level nested CLI calls (call-as-binding-RHS)

The single-level form `outer(result=?, inner(z=?x, p=3))` ships in #21
2026-05-09. The multi-level form
`outer(result=?, mid(z=?x, a=inner(z=?y, p=1)))` (where the inner call
appears as the RHS of a binding) does not — because `parse_call_args`
feeds binding RHS through `Parser.parse_expr()`, and Parser doesn't accept
`=?` inside expressions. Fix requires either recursive `extract_formula_calls`
inside `parse_call_args`'s binding handler, or a new walker. Same limitation
applies to `.fw` files.

**Reopen trigger**: a user / LLM benchmark demonstrates a real composition
chain that needs >1 level of CLI nesting (with no trivial flat-arg
restructuring). At that point the design should choose between
"inline-recurse `extract_formula_calls` from binding RHS" (smallest diff)
or "extend FormulaCall to carry sub-calls" (cleaner but invasive).

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

## 48. Generic `resolve_at_load(rewriter)` mechanism — DONE (2026-05-10, Future #16 M1)

`resolve_diff_in_equations` (system.h) was the first post-load tree-rewriter. Future #16 (M1) added integration as the second consumer; the shared skeleton was extracted as `resolve_at_load<Rewriter>(rewriter, up_to)` (template). Both `resolve_diff_in_equations` and `resolve_integral_in_equations` are 4-line wrappers around it. Subsequent post-load tree passes (e.g. typed-binding predicates per #53, units #7, LaTeX hints #9) plug in here.

## 49. Per-builtin metadata registry — DONE (2026-05-10, Future #16 M3)

`BuiltinMeta` struct + `builtin_meta()` registry shipped in `expr.h` at M3 close. Schema: `{DiffFn diff, IntegrateFn integrate}` per builtin name. Nine current entries (sin/cos/tan/asin/acos/atan/log/sqrt/abs); `symbolic_diff`'s 9-branch FUNC_CALL if-chain and `symbolic_integrate`'s 3-branch FUNC_CALL if-chain both replaced by a single registry lookup. Free `*_diff` / `*_integrate` helper functions live above the registry. Future consumers (#7 units, #9 LaTeX, dimensional annotation) extend by adding fields to `BuiltinMeta`. **The registry is the 4th consumer of Future #53 (typed-binding predicates)** — migration to `.fw` rules waits on #53.

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

**Escalation (2026-05-10, Future #16 M1):** Tier 1 antiderivative table (`symbolic_integrate` in `expr.h`) is the **3rd consumer** of the same wildcard-restriction need — `Var(var)^n` matching with `n` constant requires the same numeric-literal predicate. M1 ships with a C++ if-chain (matching diff's pattern); M3's `BuiltinMeta` registry (DONE 2026-05-10, Future #49) is the **4th consumer** carrying both diff and integrate metadata as C++ function pointers — the `.fw`-rule migration of these tables is gated on #53. Recommend prioritising in PLAN-NEXT.

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

## 58. `BinOpInfo` constexpr constructor (C2 carry-over)

`static const BinOpInfo table[]` in `expr.h` cannot become `static constexpr` because `BinOpInfo` stores lambda function pointers, which are not constexpr-constructible in C++17. The array is annotated `// static const: runtime-init lambda fn ptrs, not constexpr-able in C++17`. A constexpr constructor becomes feasible in C++20 (where `consteval` / `constinit` and designated initializers give better compile-time init stories for struct literals with function members). Shipping this conversion requires either a C++20 toolchain upgrade or demonstrated hot-path benefit from a `constinit` variant.

**Reopen trigger:** C++20 upgrade decision, OR a perf-auditor run (`make analyze-full` + disassembly) identifies `BinOpInfo` table initialization as a measurable startup cost.

## 60. Promote `make test-clang` to per-cycle gate

`make test-clang` (added in Cycle 7) is currently an optional cross-compiler sanity check — not part of the required `make test && make sanitize && make analyze-fast` gate. The cycle validated the target's value on first run (surfaced a latent UB in `recognize_fraction` that GCC -O2 masked). Promoting it to the per-cycle gate requires: (1) confirming `clang++` is available on all dev machines that close cycles, (2) updating CLAUDE.md's "Quality bar" line, and (3) updating the orchestrator's per-cycle gate protocol in `.claude/agents/fwiz-orchestrator-protocols.md`.

**Reopen trigger:** any clang-only warning or codegen divergence surfaces after a future cycle — i.e. the target has found a real issue in a cycle where it was optional.

## 61. Fuzzer coverage report target

Expand `make fuzz` recipe to produce a coverage profile for `parser.h`, `lexer.h`, and `expr.h::simplify`. Mechanically: add a separate `fuzz-cov` target that builds `bin/fwiz_fuzz_cov` with `-fprofile-instr-generate -fcoverage-mapping`, replays `fuzz_corpus/` through it, then runs `llvm-profdata merge` + `llvm-cov report --include parser.h` to emit a per-function branch-coverage table. Currently deferred — the 60-second blind run hits 1907+ unique features without targeted coverage tracking, and the extra Makefile and tooling complexity exceeds the SHIP-DESIRABLE threshold. Reopen trigger: a parser regression slips past the per-cycle gate AND was not caught by the fuzzer's blind exploration.

## 62. `analyze-fast` cppcheck scope expansion

The per-cycle gate hard-codes `src/main.cpp src/tests.cpp` and silently excludes `src/fuzz_parser.cpp`. Currently harmless — the harness body is a single try/catch, yielding zero cppcheck findings. Reopen trigger: when the harness grows beyond a single entry point (e.g. a separate solver-fuzzer harness or corpus-extraction script), add those files to the cppcheck invocation.

## 59. Periodic C1 follow-up (`misc-const-correctness`) — RESOLVED 2026-05-07

Cycle 7.5 drove the `misc-const-correctness` + `modernize-use-nodiscard` baseline to 0 via a one-shot `clang-tidy --fix` pass over the full codebase (245 findings fixed; 7 files, +251/-245 LOC). Local-variable `const` is now enforced by clang-tidy rather than being aspirational. Future `make analyze-full` runs start from a clean baseline; any new findings will surface incrementally at the next batch run.

## 63. Domain-aware antiderivative (`log(abs(x))` for `∫1/x`)

`symbolic_integrate(1/x, x)` currently emits `log(x)` (consistent with the existing `log(x^n) = n*log(x) iff x != 0` simplifier convention — same domain assumption). The mathematically-correct antiderivative is `log(abs(x))` when the sign of `x` is unknown. M1 deliberately does NOT emit `abs(x)` because (a) without global-condition propagation reaching the simplifier, the engine cannot prove `x > 0` to drop the abs, and (b) emitting `log(abs(x))` unconditionally pessimises every concrete-domain case. Defer to a domain-aware pass that consults the active condition system.

**Reopen trigger:** Future #31 (`abs(x) = x iff x >= 0` builtin rewrite + global-condition propagation reaches the simplifier).

## 64. `--integrate` CLI flag deferred

M1 ships in-file `integral(target, var)=?[alias]` and inline `f = integral(g, x)` surfaces only — there is no `--integrate` CLI flag analogous to `--derive`. Rationale: `--derive` exists because symbolic derivation is a global render mode applied to every query; integration is an explicit per-query operation that already has a natural in-file syntax. Adding a flag risks LLM/user confusion ("is `--integrate` a render mode or a target?").

**Reopen trigger:** LLM benchmark run or user reports trying `--integrate` and finding it absent (signals the discoverability gap is real).

## Refactors

Readability-driven refactor candidates filed by the blind-spot critic. Each carries a **From** (source cycle + grader-tier failure), a **Proposed** (concrete change), and a **Reopen trigger**. The visionary audit tier-classifies these on the next cycle.

## #R1. Refactor: `try_u_sub_integrate` cse_replace naming hijack

**From:** Cycle I-M2 blind-spot critic (function-scope). Haiku-B failed at T1 (wrong-on-detail) on `cse_replace(residual, {{u_name, g}})` — the helper's name signals "common-subexpression extraction" but the call site uses it as a generic structural-equality replace (g-subtree → Var(u_name)). T3 passes only because the comment explicitly says "cse_replace does exactly this with structural eq" — load-bearing comment papering over a naming mismatch.

**Proposed:** Promote `cse_replace` to a more general name reflecting its actual contract — e.g. `tree_replace_subtree` or `replace_subtree_by_name` — keeping `cse_extract` (which IS CSE-specific) named as is. OR add a thin wrapper alias `replace_subtree(tree, name, target)` used at the u-sub call site. Either lift the load-bearing comment off the call site by making the name tell the truth.

**Pattern coverage:** Single-site for now (`try_u_sub_integrate` is the only non-CSE consumer). Per agent profile (N≥3 for rule extraction), this stays as a per-function refactor item. If a third symbolic pass starts using `cse_replace` for non-CSE rewrites (likely in M3 IBP), promote to a rename rule.

**Reopen trigger:** Any third call site using `cse_replace` for non-CSE structural rewriting; OR any future cycle's blind-spot critic re-flagging the cse_replace usage in `try_u_sub_integrate`.

## #R2. Refactor: `resolve_integral_calls` 4-arg control-flow restructure

**From:** Cycle I-M2 blind-spot critic (function-scope). Haiku-B failed at T1 (wrong-on-detail) on the symbolic-vs-numeric dispatch in the 4-arg branch: the `if (val) { if (finite) return Num; /* fall through to numeric */ } else { return diff; }` shape has asymmetric early-return — the symbolic-bounds path returns directly, the NaN/inf path silently falls through. T3 only passes because the comment explicitly labels "fall through to numeric path" and "symbolic bounds — keep the closed form".

**Proposed:** Extract the 4-arg branch into a helper `resolve_definite_integral(antideriv, target, var, lo_expr, hi_expr) → ExprPtr` that hides the dispatch behind explicit named returns:
1. If `antideriv` and bounds collapse to a finite numeric → return `Num`.
2. If `antideriv` and bounds stay symbolic (free vars) → return symbolic difference.
3. Else if both bounds evaluate to finite → return adaptive_simpson result.
4. Else → return `nullptr` (caller surfaces unevaluated).

The fall-through-on-NaN/inf becomes an explicit early-fall-through `if (val && std::isfinite(val.value())) return Num;` followed by a shared numeric path. The asymmetry disappears because each terminal case is a named return; no comment-as-control-flow-marker needed.

**Pattern coverage:** Single-site. The `resolve_diff_calls` neighbour was rewritten cleanly in Cycle I-M1 with no analogous fall-through asymmetry — this is a 4-arg-specific shape.

**Reopen trigger:** When M3 adds IBP (which may add a third symbolic-fallback strategy) the dispatch grows; refactor before adding the fourth case.

## #R3. Refactor: `vec_mat_det` `en` vs `e` comment-code mismatch

**From:** Cycle I-M2 blind-spot critic (function-scope). Haiku-B at T3 scored wrong-on-detail: comment header reads textbook `a(ei - fh) - b(di - fg) + c(dh - eg)` using `e` for the (1,1) entry, but the code uses `en` (presumably renamed to avoid `e = Euler's number` collision). A reader trying to verify the cofactor expansion against the comment will find an apparent transcription bug. T3 fails *worse* than T1/T2 — the comment misleads.

**Proposed:** Either rename `en` → `e_` (trailing-underscore disambiguation) and update the comment correspondingly, OR keep `en` and rewrite the comment to use `en`: `// 3x3 cofactor: a(en*k - f*h) - b(d*k - f*g) + c(d*h - en*g)`. The latter is mechanical; the former is more idiomatic. Either eliminates the comment-code drift.

**Pattern coverage:** Single-site. Drift introduced because `e` is a reserved symbolic name (Euler's number) in this codebase, but no convention exists for "what to rename a `e`-collision local to". Watch for analogous collisions on `i` (imaginary unit, since 2026-05-09) and `pi`/`phi`.

**Reopen trigger:** Any third site renaming `e`/`i`/`pi`/`phi` locally without comment updates; OR a future cycle's grader re-flagging `vec_mat_det`. If the pattern recurs, extract a rule: "when renaming a builtin-constant collision, the comment must use the renamed identifier."

## #R4. Refactor: symbolic / numeric integration cross-references

**From:** Cycle I-M2 blind-spot critic (file-scope, src/expr.h §Symbolic integration). file-explainer at T3 scored vague-but-correct on Components/Relationships/Pattern: the symbolic helpers (`try_cancel`, `try_u_sub_integrate`, `symbolic_integrate`, `symbolic_integrate_simplified`) cluster at lines 2616–2878 and the numeric counterpart (`adaptive_simpson`, `adaptive_simpson_recurse`, `INTEGRATION_TOLERANCE`, `ADAPTIVE_SIMPSON_MAX_DEPTH`) lives at lines 3329–3391, separated by ~430 LOC of unrelated solver / Newton / bisection code. A reader of the symbolic section does not realise the numeric block is the same Future #16 milestone's other half.

**Proposed:** Add cross-reference comments on each end:
- On the symbolic section header: `// Numeric counterpart: adaptive_simpson (line ~3329, after numeric solver helpers) — definite-integral fallback when symbolic_integrate returns nullptr.`
- On the adaptive_simpson intro comment (already mentions "numeric fallback for definite `integral(f, x, a, b)`"): add `// Paired with symbolic_integrate (line ~2690 in §Symbolic integration above); dispatch is `resolve_integral_calls` in system.h.`

No code moves. The structural gap closes by making the cross-file-region link explicit.

**Pattern coverage:** This is one site of a broader pattern in expr.h (3500+ LOC, multiple feature-areas interleaved). The file-organisation rule extracted from this finding (Code-Style.md §File-organisation rules) generalises the principle.

**Reopen trigger:** Any future cycle's file-scope critic re-flagging the symbolic-integration section; OR a third milestone shipping a non-contiguous surface in expr.h (`symbolic_diff` already has `diff()`-as-builtin in expr.h + post-load resolver in system.h, but those are cross-file by design — a same-file split is the trigger).
