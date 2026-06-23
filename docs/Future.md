# Future: Planned Features

## Motivation

Features that build on each other to make fwiz significantly more expressive while staying true to the "equations, not assignments" philosophy.

> Shipped features and cleanup cycles live in `docs/COMPLETED.md`. Numbering matches across the two files — `#22` here is `#22` there.

## 4. Numeric Solving — Remaining enhancements

Core landed; see COMPLETED.md #4.

- Periodicity detection — core shipped (#12 DONE); numeric gap-based detection deferred (#12a)
- User-provided initial guess syntax (e.g., `x=?~5`)

## 101. Aggregation reverse-solve: algebraic `=` upgrade for linear inverses — PARKED

**Surfaced gen-6 cycle 1 Step D (2026-06-21).** Reverse-solving through a
formula-bodied aggregation (`sum(dmg(atk=f, def=k), f in [1..6]) = total`)
currently composes via the numeric solver (Strategy 6 system-probe), so a linear
inverse like `total = 21*k` is recovered as an approximate `~` result even though
it is exactly `k = total/21`.

**Proposed fix:** when the aggregate body unfolds to a closed form linear in the
unknown, `unfold_formula_call_body` the per-value clones into a single collapsed
expression (`total = 21*k`) BEFORE Strategy 6, so the exact algebraic Strategy 2
path (`k = total/21`) fires first and emits `=`.

**Trigger:** user reports `~` on a linear reverse-solve that should be exact, OR a
cycle needs exact symbolic aggregation inverses.

## 102. `resolve()` first-wins divergence on multi-branch FORMULA_REV — PARKED

**Surfaced gen-6 cycle 1 Step D (2026-06-21).** For a reverse-solve through a
PIECEWISE (multi-branch) formula call inside an aggregation
(`sum(combat(atk=f, def=k, dmg=?), f in [1..6]) = 6`), `resolve_all("k")`
correctly returns `{3}`, but the single-value `resolve("k")` can return a spurious
root (e.g. `-5`). Root cause: a FORMULA_REV candidate inverts one clone's
sub-system for `def` via the `dmg = atk - def` branch even when the active branch
was `dmg = 0` (condition `atk <= def`), producing a `def`(=`k`) value that does
NOT satisfy the aggregate. `resolve()` accepts the first candidate's value with no
global forward re-verification; `resolve_all()` is robust because the correct
numeric-scan root survives among all collected candidates.

This is pre-existing FORMULA_REV behaviour (no global re-verification of an
inverted single value), merely *exposed* by aggregation — it is not specific to
the aggregation surface. The Step D BLOCKING piecewise test asserts via
`resolve_all` accordingly.

**Proposed fix:** add a forward-consistency gate to the FORMULA_REV candidate in
`try_resolve` — after binding `target` from an inverted sub-system, re-evaluate the
source equation forward and reject the binding if it does not reproduce the known
RHS. (Larger surface than a single cycle; touches the general single-value solve
path, not just aggregation.)

**Trigger:** a user reports a wrong single value from `resolve()` (or CLI
single-result mode) on a piecewise/multi-branch reverse-solve where `resolve_all`
gives the right answer, OR a cycle needs sound single-value reverse-solve through
multi-branch formula calls.

## 103. Bounded aggregation over a cross-file formula-call body — PARKED

**Surfaced gen-6 cycle 2 (2026-06-22).** The aggregate-unroll post-load pass resolves formula calls registered in the SAME system (`custom_function_defs_`/sections), but a cross-file callee is resolved later via `load_sub_system`. The aggregate unrolls at load time before that cross-file sub exists, so `sum(hyp_pmf(N=N,..., result=?p), j in [kmin..n])` fails with "no value for 'p'". Workaround used: hyp_at_least.fw inlines the PMF term as nested products — a single self-contained bounded aggregation that unrolls cleanly.

**Trigger:** reopen when the aggregate-unroll pass is taught to defer / re-run after cross-file sub-systems load, OR when a user reports a tail-sum of a sibling stdlib function not folding.

## 104. Iterator named `i` collides with imaginary unit in non-linear aggregation bodies — PARKED

**Surfaced gen-6 cycle 3 (2026-06-22).** The builtin rewrite rule `i^2 = -1` fires on the aggregation body BEFORE the unroll substitutes the iterator, so `sum(i^2, i in [1..N])` resolved via a binding returns `-N` instead of the correct sum of squares. Linear bodies (`sum(i, ...)`, `sum(i+1, ...)`) are unaffected. The fix used in stdlib/probability/ is to name iterators `v` instead of `i`. A principled fix would require the unroll pass to substitute the iterator into the body BEFORE rewrite rules run, or to shadow the `i` builtin during unroll.

**Trigger:** user reports a surprising aggregation result with an `i` iterator (e.g. `sum(i^2, i in [1..5])` returning -5), OR a scoping pass makes iterators shadow builtin constants.

## 105. `;` in a `.fw` comment parses as a statement separator — PARKED

**Surfaced gen-6 cycle 3 (2026-06-22).** A semicolon `;` is a statement separator anywhere in fwiz (documented). The comment-stripping does not protect text after a `;` on the same line. A comment like `# draw; with k = 1` silently creates a default equation `k = 1` because the post-`;` text `with k = 1` parses as a valid statement. Workaround: avoid `; var = value` shaped prose inside comments. The combinatorics files use `;` in comments harmlessly (the post-`;` text is non-parseable prose); only `var = value`-shaped post-`;` text triggers it. Reproducer: `printf '# foo; with k = 1\n[f(N,n,k)->result]\nresult=k*(N+1)/(n+1)\n'` then resolve `f(N=9,n=4,k=3)` → returns 1 instead of 6.

**Trigger:** user reports a stray binding from a comment, OR a comment-lexer hardening cycle. Fix: teach the lexer to consume the rest of a `#`-comment line including any `;` so post-semicolon prose inside comments is inert.

## 5. Batch/Table Mode — DONE (2026-05-11)

Core shipped. See `COMPLETED.md #5`. Four parked follow-ups below.

### 5a. `--table-max-rows N` (parked)

**Trigger:** a user hits the soft 1M-row stderr warning and reports it as friction (either wants to silence it, or wants a hard cap below 1M). Currently `--table` warns at ~1M cartesian rows and continues; an explicit `--table-max-rows N` would let users set both a hard cap and a hard-error threshold without writing a wrapper.

### 5b. In-file declarative range (parked)

**Trigger:** a second consumer asks for `v = [1..10]` as a first-class vec/mat literal (e.g. `--fit` wanting sample-point specification, or numeric solver scan-range syntax). Today `parse_range` is CLI-only — the `[start..stop @ step]` grammar lives in `system.h` next to `parse_cli_query`, not in the Lexer/Parser. Promoting it to a parser-level range-to-vec materialization is consistent with the vec/mat work (#14) and would reuse `parse_range` directly.

### 5c. `--all-results` table mode (parked)

**Trigger:** user with a multi-root system wants every solution per row, not just the first. Today `--table` emits the first element of `resolve_all().discrete()`. The change is mechanical (emit one row per result, repeating the input columns) but breaks the deterministic 1-row-per-input-tuple shape, so it needs an opt-in flag.

### 5d. `--nan` sentinel (parked)

**Trigger:** concrete pandas/gnuplot/R interop failure where `?` (Fwiz convention) breaks a downstream pipeline. `--nan` would emit `NaN` (numpy/R convention) instead. Today users can `sed 's/?/NaN/g'`; a flag becomes worth adding once a real consumer complains.

### 5e. Bindings-copy-per-row optimization (parked)

Inside `emit_row` (main.cpp), each row starts with `auto bindings_copy = query.bindings` — an O(M) copy of all M fixed bindings — then overlays K range-dimension keys. For tables with M ≥ 5 fixed bindings and N ≥ 100K rows this produces N×M unnecessary copies. Mitigation: hold one pre-allocated base copy outside the loop, overlay only the K range-dimension keys per row (O(K log M) per row instead of O(M)). No API change needed; the driver block in `main.cpp` owns the loop.

**Trigger:** user-reported latency on tables with N ≥ 100K rows and M ≥ 5 fixed bindings.

### 5f. Arena accumulation across table rows (parked)

The single `ExprArena::Scope` wrapping the entire table loop grows without bound across rows. At the 1M-row threshold with constant-recognizable output, the arena can reach ~960 MB. Two mitigation options: (a) add `ExprArena::checkpoint()` / `reset_to(checkpoint)` API for periodic flush while preserving the recognized-form ExprPtrs referenced by the current row's output; (b) a `fmt_exact_double` string-direct variant that avoids arena allocation entirely for the display path. Neither option requires changing the solver or the TSV format.

**Trigger:** user memory-pressure report at 1M rows, or the 1M-row soft-warning threshold reached in practice with observed RSS growth.

### 5g. `numeric_memo_` clear in table mode (parked)

For numeric-solver equations, `numeric_memo_` accumulates per-key memoized results across all rows. In table mode the memo is safe (each key encodes all bindings, so no stale reuse), but the map grows unboundedly for large tables. A single `numeric_memo_.clear()` before the table loop (NOT between rows) would cap memory at one row's worth of memo entries at a time.

**Trigger:** numeric-equation table at ≥ 100K rows with measurable RSS growth attributable to the memo map.

## 7. Units and Dimensional Analysis

### Split — engine vs stdlib (clarified 2026-05-11)

This item splits along the engine/stdlib axis per the project's wrapper-tier discipline:

- **#7 (this item — IN-SCOPE core)**: language-level support for unit suffixes. **Cycle 1 shipped 2026-05-13**: Option C (parser desugar `100kg` → `MUL(Num(100), Var("kg"))`) chosen and implemented. `kg` is an ordinary `Var`; unit semantics live in stdlib `.fw` bindings. `stdlib/units/si-minimal.fw` ships the 7 SI base units as scalar 1. **Cycle 2 shipped 2026-05-13**: CLI-arg evaluation (#73 DONE); stdlib expanded 16→42 lines (SI prefixes + derived units). Remaining #7 work: dim-analysis stdlib (#7a), dimensional rejection (#7b).
- **#7a (NEW SUB-ITEM — WRAPPER-TOOL)**: the unit catalog itself (SI units, prefixes, derived units, conversion factors) lives in `stdlib/units/*.fw`. Built ON the engine's suffix mechanism, not inside the core.
- **#7b (UNBLOCKED 2026-05-14 — two-step DONE framing per gen-3 cycle 1)**: dimensional analysis rejection at parse/simplify time. Future #78 resolved (hybrid dim model). Two-step DONE:
  - **#7b BASIC (atomic-Var dimensional rejection)**: ✅ BASIC DONE 2026-05-15 (gen-3 cycle 2 substrate). Atomic-Var dimensional rejection rules can now be written in stdlib `.fw` — e.g. `x + y = undefined iff is_in(x, mass) && is_in(y, time)` (canonical form; `is_in_dimension(x, mass)` also accepted as sugar) — and the engine evaluates them at simplify time via `is_in` predicate + `type_map_` + `set_definitions_` registry (gen-5 cycle 3a unified these under `SimplifyContext`). Criterion 5 (predicate works as rule condition) passed.
  - **#7b FULL (compound-expression dimensional rejection)**: ✅ FULL DONE 2026-06-06 (gen-5 cycle 3c). `BindingType::dim` promoted from `std::string` to `DimMap = std::map<std::string,int>` (exponent algebra). `compute_dim(Expr) → std::optional<DimMap>` recursive fold propagates dimension through MUL (add exponents), DIV (subtract), POW (integer-exponent scale), NEG (passthrough), ADD/SUB (match-or-nullopt mismatch), Var (type_map lookup), Num (dimensionless empty-map). `BuiltinMeta.dim_propagate` callback field added; `sqrt` halves exponents, `abs` passthrough. DIM_SECTION arm of `check_condition` lifted its `is_var` guard — compound expressions get dimensional membership via map-equality. ADD/SUB mismatch detection ships (sentinel `nullopt`); the cerr warning for hot-loop-spam is deferred DESIRABLE. Criterion for DONE ("ADD/SUB mismatch detection and compound-expression test passing") satisfied. Tests 3805→3831 (+26). All gates green.

  Original blocker (Future #78 resolution) cleared. Cycle-4-of-Units-arc verdict ("`.fw` predicates can't ship today because substrate doesn't exist") superseded — the substrate is the gen-3 cycle 2 deliverable.

### Problem (#7)

`speed = distance / time` should know that `100km / 2hr = 50 km/hr`. The minimum engine support: `100km` is a parseable token (suffix-bearing number). The arithmetic and dimensional reasoning live in stdlib `.fw` rules.

### Proposed syntax (#7 engine surface)

```
# inline value with unit suffix
fwiz physics(force=?, mass=100kg)
fwiz speed(distance=10km, time=2hr)
```

```fw
# in a .fw file — suffix is parsed as engine-level token
m = 100kg
v = 50km/hr
```

### Cycle 1 decisions (#7, shipped 2026-05-13)

1. **Suffix → AST mapping**: Option C chosen — `100kg` → `MUL(Num(100), Var("kg"))`. No new `ExprType`, no `sizeof(Expr)` change.
2. **Suffix grammar**: `[A-Za-z_][A-Za-z0-9_]*` — any identifier immediately following a number (no space). Compound suffixes (`km/hr`) require explicit `*` and `/` operators.
3. **Disambiguation**: `100m` always means `100 * m` under Option C; if `m` is bound in scope, it resolves to that binding. This IS the elegance argument for (C).
4. **Round-tripping**: `--derive` expands to `100 * m` (the `MUL` form); re-parses identically.
5. **Precedence quirk**: `100m^2` parses as `(100 * m)^2` (see #74). Mitigation: parse-time warning. Fix deferred to #74.

### Capabilities (in-scope for #7 core)

- Lexer recognizes `<number><identifier>` as `<number> * <identifier>` (or similar minimal binding)
- The identifier is a regular `Var` — semantics deferred to `.fw` rules
- Round-tripping safe: output of `--derive` re-parses identically

### Capabilities (deferred to #7a stdlib / #7b)

- The unit catalog (SI base + derived) — stdlib `.fw` files
- Conversion rules (`1km = 1000m`) — stdlib `.fw` rewrite rules
- Display formatting in user's preferred unit — stdlib option
- Dimensional analysis rejection — #7b (typed-predicate escalation)

### Reopen trigger for #7

Cycle 1 shipped. Cycle 2 shipped: #73 (CLI-arg) DONE; stdlib expanded 16→42 lines. Remaining work: #74 (precedence fix), and the queued dim-analysis cycles.

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

`--cse` (Cycle B) extracts subexpressions per `derive_all` invocation. `--table` mode (shipped 2026-05-11, Future #5) emits multiple parameterized rows but does not share a `# Helpers` preamble — each row is a numeric result, not a symbolic equation. This item activates if a future mode combines `--table` with symbolic output (e.g., `--derive --table` were allowed, or a per-row symbolic-form option were added). In that case, running `cse_extract` over the union of row-expressions and emitting a single `# Helpers` header would reuse the existing primitive with only a new print-block layout.

**Reopen trigger**: a future mode produces one symbolic equation per table row, making shared helpers worthwhile.

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

The post-load pass (`resolve_diff_in_equations`) inlines formula-call bodies for `diff(formula_call_placeholder, var)` via `unfold_formula_call_body`. Corner cases deferred: piecewise formula calls (multiple RHS branches — which branch to differentiate?), multi-return formula calls (which output var?), and formula calls with expression bindings that themselves contain `diff`. Revisit when Future #20 typed FORMULA_CALL nodes land (giving stable node identity for the chain rule), or when >2 user reports of unexpected behavior surface.

**Reopen trigger:** Future #20 (typed FORMULA_CALL nodes) enters a planning cycle, OR >2 user reports of unexpected `diff(formula_call, var)` behavior.

## 51. Piecewise / conditional formula-call diff (multi-branch)

When `diff(formula_call, var)` targets a sub-system with multiple equations defining the output (e.g., `abs` via two `iff` branches: `result = x iff x >= 0` and `result = -x iff x < 0`), the post-load pass `unfold_formula_call_body` currently uses only the first equation's RHS. The correct behavior depends on which branch is active at evaluation time — this requires either evaluating conditions symbolically (and folding them into a piecewise derivative) OR returning a piecewise result expression. Today the user silently gets one branch's derivative.

**Reopen trigger:** user reports unexpected derivative of a piecewise formula call.

## 52. Test coverage for `diff(...)=?` range-ValueSet output path

The `diff(...)=?` query path returns a `ValueSet` (CLI Surface 2) and so should support range/interval results in addition to discrete values. Polish-pass Item 6 attempted to construct a CLI-level reproducer for the range branch but found that range-valued constraints on RHS variables (e.g., `slope = a` with `a > 1, a < 5`) do not propagate through `resolve_all` to the LHS — this is a structural gap independent of `diff()`. A range-result test for `diff(...)=?` therefore requires either (a) extending `resolve_all` to propagate constraint ranges through equation chains, or (b) constructing a derivative whose internal evaluation directly produces a `ValueSet` interval.

**Reopen trigger:** range-propagation through `resolve_all` lands (independent feature), OR a user surfaces a `diff(...)=?` query whose natural answer is an interval.

## 53. Typed-binding predicates in `.fw` rule conditions — DONE (2026-05-10)

Mechanism shipped with `is_neg_num` as the initial predicate. `is_neg_num(n)` in a rule condition tests that wildcard `n` binds to a negative numeric literal; fail-safe semantics (unknown or non-Num binding → false). Encoded as `CondClause{lhs=FUNC_CALL("is_neg_num", {Var("n")}), rhs=nullptr, op=CondOp::EQ}`. T3.6 (`x^(-n) → 1/x^n`) is the first consumer — migration complete (see #55). Predicate set extends per-consumer schedule: `is_int` ← T3.5/#54 cycle; `is_pos_num` ← #31 cycle; `is_num` ← #49 BuiltinMeta migration cycle. Four consumers audited at escalation time (2026-05-10): T3.5, T3.6, #31, BuiltinMeta #49; each triggers its own predicate addition when that consumer cycle begins.

**Reopen trigger**: a new consumer cycle starts that needs an additional predicate (see NEW item below for the per-consumer schedule), OR Future #31 (`abs(x) = x iff x >= 0`) is reopened, OR T3.5 is reopened.

## 54. T3.5 non-migration: constant reassociation in `simplify_div`

The constant-reassociation block in `simplify_div` (expr.h) cannot migrate to `.fw` rewrite rules. Root cause: the block extracts numeric factors from symbolic expressions using `make_rational` — a C++-only operation — and rebalances the numeric side. The typed-binding predicate blocker (`iff is_num(a)` unrepresentable in rule conditions) was cleared by Future #53 (2026-05-10). The remaining blocker is rule-RHS semantics: `make_rational` is a C++-only operation producing structural `DIV(Num, Num)` nodes; the rule engine cannot express this on the RHS today.

**Reopen trigger**: `make_rational` is callable from rule RHS evaluation, AND `is_int` predicate is added to `predicate_names()` (per #53 per-consumer schedule).

## 55. T3.6 non-migration: `x^(-n) → 1/x^n` — DONE (2026-05-10)

Migrated. The C++ block at `expr.h:2471-2477` (`is_num(r) && r->num < 0` branch) was deleted; replaced by `.fw` rule `x^n = 1 / x^(-n) iff is_neg_num(n)` in `BUILTIN_REWRITE_RULES` (system.h). Fail-safe predicate semantics prevent the rule from firing on symbolic exponents (`x^y` with Var `y` → false). The `-1` special case merges naturally: `x^(-1)` → `1 / x^1` → `x^1 = x` → `1 / x`. Regression guards: `b^(-1) → 1/b`, `b^(-2) → 1/b^2`, `b^(-3) → 1/b^3`, `x^y` not rewritten, `0^(-1)` still folds to `+inf` via numeric-fold path before rule engine sees the node — all confirmed by new tests in `test_future53_t36_negative_exp_migration`.

## 65. Additional `.fw` typed-binding predicates (per-consumer schedule)

Four predicates have named consumers but were not shipped in #53 (which delivered `is_neg_num` only, per minimalism critique). Each predicate ships in the cycle that consumes it:

- `is_int(n)` — ✅ DONE (gen-3 cycle 2, 2026-05-15); **unified (gen-5 cycle 3a, 2026-05-15)**. `is_int(n)` is now sugar: `parse_condition` rewrites it to `is_in(n, int)` at parse time. The `int` built-in `SetDef` (`BUILTIN_PREDICATE` kind, `is_integer_value` membership) handles dispatch. Users may still write `is_int(n)` — the engine normalizes it. Note: the original consumer (T3.5/#54 migration) still requires the independent `make_rational`-callable-from-rule-RHS blocker. Future named predicates whose semantics fit the membership-test shape SHOULD ship as built-in `SetDef` entries rather than new dispatch arms.
- `is_pos_num(n)` — binding is `Num` with value > 0. Consumer: Future #31 (`abs(x) = x iff is_pos_num(x)` partial form, deliverable without full global-condition propagation).
- `is_num(n)` — binding is any `Num`. Consumer: Future #49 BuiltinMeta migration to `.fw` rules (needs integration-variable context threading as the independent secondary blocker).
- `is_in_dimension(v, dim)` — ✅ DONE (gen-3 cycle 2, 2026-05-15); **unified (gen-5 cycle 3a, 2026-05-15)**. `is_in_dimension(v, dim)` is now sugar: `parse_condition` rewrites it to `is_in(v, dim)` at parse time. Dispatch goes through `set_definitions_[dim]` (kind `DIM_SECTION` → compares `type_map_[v_name].dim`). The 4th `check_condition` param is now `const SimplifyContext*` (carries both `type_map_` and `set_definitions_` together); `RewriteRulesGuard` 5th-arg type changed accordingly.

**Reopen trigger (each remaining):** the named consumer cycle starts. The predicate machinery (encoding, `predicate_names()` extension, `check_condition` dispatch branch) is already in place — adding a new predicate is one dispatch line + tests.

**V3 policy update (gen-5 cycle 3b, 2026-05-16)**: with `USER_PREDICATE` SetDef Kind shipping (cycle 3b — users can write `[name(arg)] iff condition` to declare their own predicate sets), **new dispatch arms in `check_condition` for predicate semantics require explicit justification against the SetDef alternative.** The SetDef route (`BUILTIN_PREDICATE` for C++-fast-path; `USER_PREDICATE` for `.fw`-extensible) handles any predicate whose semantics fit the membership-test shape. New C++ dispatch arms are reserved for predicates whose semantics fundamentally do NOT fit membership (e.g. `is_neg_num`'s literal-shape check — testing whether a binding is itself a Num node with negative value — which is structural, not value-membership).

## 67. CLI / `.fw` dispatch-path unification for `integral`/`diff` queries — DONE (2026-05-12)

`parse_cli_query` now synthesises `<alias> = diff/integral(...)` equations into `CLIQuery::synthetic_equations` (string) and pushes the alias as a regular entry in `CLIQuery::queries`. `main.cpp` loads it via a single `sys.load_string` after the file/inline source; the standard query loop and post-load passes handle the rest. `CLIDiffQuery` / `CLIIntegralQuery` structs deleted; Pass 1.5 / Pass 1.6 dispatcher blocks (~50 LOC each) deleted. Net production LOC: -65 (-83 main.cpp, +18 system.h). Tests: 3340 → 3343 (+3 net).

Two visible bugs closed as a side effect: `--table` + `integral(...)=?A` now composes naturally (integral queries flow through `q.queries[]` which the table driver already iterates); CLI binding RHS `F=integral(x^2, x)` combined with `F=?` now produces `F = x^3 / 3` (previously errored "Invalid value" because `evaluate()` on an unresolved integral FUNC_CALL returns empty by design).

## 66. `is_num(arbitrary_expr)` argument-shape validation

v1 of the typed-binding predicate mechanism (Future #53) enforces single-Var argument in `parse_condition` dispatch — `is_predicate_clause` checks `c.lhs->args[0]->type == VAR`. A non-Var argument (e.g. `is_num(x+1)`) currently passes through silently: `parse_condition` will look up a binding named `"x+1"` which does not exist, and the predicate returns false (fail-safe). No parse-time error is raised.

**Reopen trigger:** user reports a rule with `is_num(<structured_arg>)` that silently fails to fire when they expected it to. Fix: add a parse-time `runtime_error` (or at minimum a stderr warning) when the predicate argument is not a bare identifier.

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

## 69. Cross-file resolution cycle SIGSEGV (matrix-arc diagnostic finding) — DONE (2026-05-13)

Shipped in cycle 2 of the Matrix-surface diagnostic-quality arc. Fix:
thread-local `currently_loading` set keyed on cache_key in
`load_sub_system` (system.h); on re-entry, throws
`CrossFileResolutionCycleError` (new, sibling of `SolveBudgetExceededError`)
with message "Cross-file resolution cycle: <file_stem> recursively loads
itself". The new exception is intentionally NOT a `std::runtime_error`, so
the many in-solver `catch (const std::runtime_error&)` sites in `system.h`
don't swallow it; the top-level `catch (const std::exception&)` in
`main.cpp` and the test-harness `get_error` both still see it. RAII
`LoadGuard` erases on success and exception paths. Regression test in
`test_recursion_depth_guard` (matmul + myfn generalization cases). Pre-fix
section archived below for the record.



**Surfaced 2026-05-13** during the Matrix-surface diagnostic-quality arc, cycle 1 corpus prep. Found via smoke-testing fuzz seeds before invoking the fuzzer.

**Reproducer:**
```bash
mkdir -p /tmp/repro
cat > /tmp/repro/matmul.fw << 'EOF'
[matmul(A, B) -> R] = matmul(A, B)
EOF
cat > /tmp/repro/caller.fw << 'EOF'
result = matmul([[1, 2], [3, 4]], [[5, 6], [7, 8]])
EOF
./bin/fwiz '/tmp/repro/caller.fw(result=?)'
# → SIGSEGV (exit 139)
```

**Mechanism:** `load_sub_system(file_stem)` at `system.h:2633` always tries `<base_dir>/<file_stem>.fw` first before falling back to embedded definitions (lines 2672-2688). When `caller.fw` is loaded as the main file, `base_dir` = `/tmp/repro`. The `matmul(A, B)` call triggers `load_sub_system("matmul")`, which loads `/tmp/repro/matmul.fw`. That file itself contains `matmul(A, B)` recursively. Each recursive cross-file load creates a NEW `FormulaSystem` instance with its own `sub_systems` cache (line 376), so the cache hit at line 2662 never fires. Infinite recursion → stack overflow → SIGSEGV.

**Critically NOT caught by the fuzz harness**: the harness uses `sys.load_string(source)` directly (`src/fuzz_parser.cpp:32`); the bug surfaces only when `sub->load_file(abs_path, section)` (line 2673) is reached, which only happens via the CLI dispatch path or programmatic `load_file`. Verified: `load_string` + `resolve_all` on the same content throws "Cannot solve for 'result'" cleanly (no crash).

**Pre-existing or new?** Pre-existing. `load_sub_system` predates #14 (vec/mat); the bug is in cross-file resolution generally, not in matrix-specific code. #14 (matmul/det/inv/transpose builtins) is the trigger that surfaced it — any user file that calls a builtin AND has a same-named `.fw` file in its directory shadows the builtin and recursively loads. Likely affects every other builtin name (`sin.fw`, `cos.fw`, `sqrt.fw`, `log.fw`, etc.) the same way.

**Why it slipped previously:** Probably zero users had a same-named `.fw` file in their working directory until the matrix arc started exercising the new builtins with corpus seeds. The conditions for triggering it are: (a) `.fw` file at `<base_dir>/<funcname>.fw` exists, (b) the file's body recursively calls `<funcname>`, (c) invocation goes through the CLI or `load_file` (not pure `load_string`). All three conditions are easy to hit accidentally during corpus prep.

**Fix surface** (deferred to cycle 2 of the diagnostic-quality arc — the error-messages cycle):
- Add a thread-local "currently loading" set to `load_sub_system`. Bail with a clear error message ("cross-file resolution cycle: matmul → matmul → ...") when the same file_stem is re-entered.
- Alternative: pre-populate `sub_systems[cache_key]` with a placeholder before recursing, replace after load completes. The cache check at line 2662 then catches the recursion.
- Either fix is ~5-10 LOC + a regression test.

**Scope alignment:** Cycle 2 of the arc is "user-facing error messages and shape-mismatch provenance." A clean "cross-file resolution cycle" error message replacing a SIGSEGV is exactly the cycle's deliverable shape.

**Reopen trigger:** picked up in cycle 2 of the active ROADMAP arc.

## 70. Shape-mismatch error message diagnostic track for matmul/det/inv/transpose — PARKED

**Surfaced 2026-05-13** during cycle 2 of the Matrix-surface diagnostic-quality arc, in scope-trimming. Shipped in the same cycle: ragged-matrix parse-time validation (#14 follow-up) and the #69 cross-file resolution cycle fix. The third candidate — improving the runtime undefined-propagation story for shape mismatches — was deferred as the lowest-leverage of the three.

**Today's behavior:** `matmul`, `det`, `inv`, `transpose` return `Var("undefined")` on shape mismatch per fwiz's domain-boundary idiom. The runtime `undefined` then silently propagates through arithmetic. User sees `R = undefined` (or `Cannot solve for 'R'`) with no hint that the mismatch was at the matmul/det/inv/transpose call site, much less which operand was wrongly shaped.

**Examples of the friction:**
- `matmul([[1, 2]], [[3], [4], [5]])` → inner dims 2 vs 3 don't match → `undefined`. User can't tell whether the bug is the left operand (should be 1x3) or the right (should be 2x1).
- `inv([[1, 2, 3], [4, 5, 6]])` → not square → `undefined`. Same opacity.
- `det([[1, 2], [3, 4], [5, 6]])` → not square → `undefined`. Same.

**Why deferred:** Improving the story requires either (a) changing the `Var("undefined")` propagation idiom to carry context (substantial design — domain-boundary propagation is intentional, used by piecewise rewrite rules `x/x = undefined iff x = 0` for exhaustiveness checking, and changing it ripples into the ValueSet machinery), or (b) a parallel diagnostic-track that accumulates context alongside the propagating undefined (a second carrier, plumbed through every simplifier path). Both shapes are 100+ LOC and need a vision-level call on which idiom owns the diagnostic provenance.

**Cycle 2 of the matrix arc deferred this** in favor of two higher-leverage deliverables: ragged-literal parse-time validation (catches the most common authoring mistake before the program reaches the runtime undefined path) and the #69 SIGSEGV fix (replaces a crash with an actionable error, which is strictly better than improving an existing-but-opaque error).

**Reopen trigger:** user reports being confused by `undefined` output on a runtime shape mismatch (i.e. an issue or a benchmark question of shape "why does fwiz say `R = undefined` instead of telling me which matrix was the wrong shape"), OR cycle 3/4 of the diagnostic arc surfaces it as a downstream blocker for the LLM-collaboration story.

## 82. Consolidate binding-side metadata (type_map_, solved_symbolic_, aliases_) — PARKED

**Surfaced 2026-05-14** during gen-3 cycle 1 design (visionary risk #6b).

**Today**: `FormulaSystem` carries three parallel mutable maps for per-variable metadata: `solved_symbolic_` (post-solve ExprPtrs for `--steps`/`--calc` rendering), `aliases_` (file-defined constants for `--derive` output), and `type_map_` (replaces `dim_map_` from cycle 2 — upgraded to `BindingType{dim, sets}` value type in gen-5 cycle 3a). Each new code path that creates or modifies a binding must remember to update each parallel map. `solved_symbolic_` is the precedent and it works fine; `type_map_` joins it as the third; the system stays manageable.

**Important distinction (V2 — gen-5 cycle 3b, 2026-05-16)**: `set_definitions_` (the registry of `SetDef` entries keyed on set NAMES) is NOT a fourth parallel-binding-map and does NOT count toward this trigger. The trigger spirit is "binding-side metadata pile-up" — registries keyed on set-names / function-names / section-names are structurally distinct. `set_definitions_` lives on `FormulaSystem` but its key space (set names like `"int"`, `"mass"`, `"whole_number"`) does not overlap with binding names. Cycle 3b's USER_PREDICATE addition extends the registry's value type to carry `Condition` + `ExprPtr` arena handles, but does NOT introduce a fourth parallel-BINDING-map. The trigger count remains at 3.

**Concern**: when a *fourth* parallel-binding-map appears (e.g. for type categories, source provenance, simplification flags), the maintenance friction becomes a real cost. Each new binding-side metadata kind costs N call-site updates.

**The refactor**: collapse parallel maps into a single binding-representation:
```cpp
struct Binding {
    ExprPtr value;          // currently in bindings_ map
    ExprPtr symbolic;       // currently in solved_symbolic_
    double  alias_value;    // currently in aliases_
    std::string dim;        // currently in dim_map_
    // future fields slot in here
};
std::map<std::string, Binding> bindings_;
```
Single map, single update site per binding mutation.

**Reopen trigger**: a fourth parallel-map proposal lands on `FormulaSystem` (after `solved_symbolic_`, `aliases_`, `dim_map_`). Triggers a refactor cycle to consolidate. Approximate scale: ~50-80 LOC refactor + careful test verification of all existing binding-mutation sites.

**Vision principle**: per CLAUDE.md "Remove > Add" — consolidating the parallel-map pattern removes the implicit invariant ("update N maps in lockstep") and replaces it with a structural one (single Binding owns all per-variable state).

## 83. Extract `copy_metadata_to_sub` helper from `load_sub_system` — DONE (gen-5 cycle 3h 2026-05-16)

**Surfaced gen-5 cycle 3a (2026-05-15)** when the cross-file propagation bug at the auto-section branch (lines 2892-2900, `system.h`) was fixed. The pre-existing bug (auto-section branch silently omitted `dim_map_` propagation while the manual branch included it) is precisely the class of error a named helper would prevent: `load_sub_system` had two branches that each duplicated the metadata copy block.

**Cycle 3h close (2026-05-16)**: `copy_metadata_to_sub(FormulaSystem& sub) const` extracted as a private member just above `load_sub_system`. Three call sites converted to single-line invocations: `load_sub_system` normal path, `load_sub_system` auto-section path, AND `register_function_section` (NEW call site — the pre-cached sub was previously NOT inheriting parent settings, suppressing numeric_mode on recursive FUNCTION_SECTION subs; this gap was the load-bearing piece for Fix A in the #92 close). Net LOC: -4 (12 inline lines deleted, 8 lines added across helper + docstring). Reopen-trigger comment moved into the helper docstring.

## 84. `NumberDomain` enum deletion — DONE (gen-5 cleanup cycle, 2026-06-21)

**Surfaced gen-5 cycle 3a (2026-05-15)** as a residual artifact. `NumberDomain` enum (or equivalent discriminator) was the last surviving remnant of the pre-cycle-3a three-way specialization (dim / domain / predicate).

**Actioned cleanup cycle (2026-06-21):** Confirmed zero consumers via grep (only the three declaration sites in `src/expr.h`). Deleted: the `NumberDomain : uint8_t { REAL, INTEGER, RATIONAL, COMPLEX, COUNT_ }` enum, the `domain_` field on `ValueSet`, and the `domain()` getter. Suite stayed 3831/3831 green. Net -3 LOC.

## 85. Predicate complexity profiling — PARKED

**Surfaced gen-5 cycle 3b visionary (2026-05-16, V4)** as a non-blocking concern for user-defined predicate sets.

**Today**: USER_PREDICATE evaluation goes through `check_condition` with the parameter bound and recursive Condition evaluation. Decidable terminating predicates work correctly regardless of how expensive their `iff` body is. The engine offers a deterministic result; speed is the stdlib-author's quality-of-implementation concern. The recursion guard (cycle 3b D7) catches infinite self-recursion + chains; expensive-but-terminating predicates (factorial-time membership tests, deep recursive enumerations) are NOT caught.

**Concern**: an LLM-generated `.fw` file might include predicates like `[has_prime_factor(n)] iff <expensive iteration>` that slow `simplify()` measurably. Without profiling, this fails silently as "slow" rather than as "wrong."

**Reopen trigger**: user reports a measurably slow `simplify()` pass traceable to USER_PREDICATE evaluation, OR a stdlib predicate exceeds 1ms per call in profiling, OR a benchmark cycle (LLM-ergonomics arc, queued) surfaces predicate-eval as a measured bottleneck.

**Possible fixes**: (a) per-predicate-call timing in debug mode + warning if cumulative time per `simplify()` exceeds threshold; (b) memoization layer on USER_PREDICATE results; (c) stdlib author tooling (`fwiz --benchmark-predicates`).

**Extension (gen-5 cycle 3d 2026-05-16, visionary V3)**: FUNCTION_SECTION dispatch is the highest-cost predicate flavor in the named-set family — each `is_in(x, sec_name)` invocation triggers a full solver call via `exists_for_function_section`. For rewrite rules consulting function-sections in tight matching loops (e.g. stdlib dimensional-rejection rules that reference function-section sets), this is likely the FIRST predicate kind to surface measurably. When this trigger fires, FUNCTION_SECTION dispatch is the primary measurement target alongside USER_PREDICATE. **Note**: this is the PERF-MEMOIZATION axis. The separate CORRECTNESS axis (recursive function-sections that legitimately need memoization to terminate, like `is_in(8, fibonacci)`) is tracked at Future #90.

**Cycle 3d update (2026-05-16)**: FUNCTION_SECTION dispatch is a likely measurement target. Every `is_in(x, sec)` for FUNCTION_SECTION-Kind sets triggers a full numeric reverse-solve via `sub.resolve(parameter, {{return_var, x}})`. For rules consulting function-sections in tight matching loops, this can dominate `simplify()` time. Memoization (fix-b) becomes more compelling — both USER_PREDICATE and FUNCTION_SECTION results share the same `(set_name, value) → bool` cache shape. Also: recursive function-section reverse-solve (e.g. `is_in(8, fibonacci)`) requires this memoization to terminate at all — the cycle-3d test scaffolding documents this gap by reformulating C4/C5 from recursive fibonacci to non-recursive `double_it` + `sqp1`.

## 86. Mutual-recursion full handling for predicate sets — PARKED

**Surfaced gen-5 cycle 3b (2026-05-16, D7 + V5)** as a known limitation of the cycle-3b recursion guard.

**Today**: `static thread_local std::set<std::string> evaluating_predicates_` keyed on set name (NOT on parameter value). Self-recursion (`[foo(n)] iff is_in(n, foo)`) returns false (fail-safe). Mutual recursion (`[a(n)] iff is_in(n, b)` + `[b(n)] iff is_in(n, a)`) partially guarded — the deeper call returns false, propagates up; the user gets a false (wrong but consistent) result rather than a hang.

**Concern**: a well-formed mutually-recursive definition (e.g. classical even/odd: `[even(n)] iff n = 0 || is_in(n-1, odd); [odd(n)] iff n = 0 ? false : is_in(n-1, even)`) returns wrong-but-fail-safe results. Users would expect either correctness or a clear "mutual recursion detected" diagnostic.

**Reopen trigger**: user reports a benign mutual-recursion definition returning unexpected false (with concrete reproducer), OR gen-5 cycle 3d (function-section sets — `[fibonacci(n) -> result]`) enters planning. Recursive sets become genuinely valuable in 3d; that's the natural cycle to invest in full mutual-recursion handling.

**Possible fixes**: (a) Tarjan-style cycle detection on the predicate dependency graph built at registration time; (b) thread-local set keyed on `(set_name, parameter_value)` tuple (allows different parameter values; blocks identical re-entry); (c) explicit user opt-in via `@recursive` section annotation that switches to a memoization-backed evaluator.

## 87. Cross-system arena lifetime for USER_PREDICATE Conditions — PARKED

**Surfaced gen-5 cycle 3b (2026-05-16, D11)** as a latent risk under future API changes.

**Today**: USER_PREDICATE SetDef entries (cycle 3b) carry `Condition` objects with `ExprPtr` fields pointing into the PARENT system's `arena`. `load_sub_system` propagates `set_definitions_` (including USER_PREDICATE entries with their parent-arena ExprPtrs) to sub-systems. **Safe today** because: (a) parent owns sub via `sub_systems` shared_ptr; (b) `load_sub_system` returns `FormulaSystem&` (raw reference, NOT `shared_ptr`) — caller cannot extend sub > parent lifetime through the public API.

**Concern**: any future change that allows sub-systems to outlive their parent breaks the assumption silently. Possible future changes: sub-system cache eviction; parallel sub-system execution with thread-local arena handoffs; an API that returns `std::shared_ptr<FormulaSystem>` (currently returns reference); detached sub-system clones for distributed solving.

**Reopen trigger**: `load_sub_system` API changes to expose `shared_ptr<FormulaSystem>` (or equivalent ownership-extending shape), OR sub-system cache eviction is introduced, OR any path where sub > parent lifetime becomes reachable through the public API. **See also #82** — when binding-side metadata consolidation eventually fires, USER_PREDICATE's Condition+ExprPtr cross-system semantics must be preserved (Condition contents may need deep-copy into the sub's arena, or the SetDef may need explicit arena ownership tracking).

**Possible fixes**: (a) deep-copy `Condition` ExprPtrs into sub's arena during propagation (cost: per-USER_PREDICATE arena alloc per sub load); (b) explicit arena-handle tracking on SetDef with refcounted parent-arena reference; (c) restrict USER_PREDICATE entries from cross-file propagation entirely (subs declare their own predicates).

## 88. `is_in` predicate in equation-condition context — discoverable trap — PARKED

**Surfaced gen-5 cycle 3b (2026-05-16, M5 implementer COLLECTED ISSUES + reviewer follow-up #4)** as a design-emergent constraint on where USER_PREDICATE-based rules can fire.

**Today**: `is_in` (and any typed-binding predicate) requires complex-LHS rewrite-rule context to fire correctly. The `expr_bindings` parameter of `check_condition` carries pattern-match wildcards from `apply_rewrite_rules`; equation-condition `check_condition` invocations pass `expr_bindings = nullptr` (equations don't have wildcards — their conditions evaluate against the solver's `bindings` map directly).

**Concrete impact**: stdlib authors writing dimensional-rejection rules using `is_in` predicates MUST use rewrite-rule shape, NOT equation-conditional shape:

```fw
# WORKS — complex-LHS rewrite rule, expr_bindings populated by pattern matcher
x + y = undefined iff is_in(x, mass) && is_in(y, time)

# SILENTLY FAILS — simple-LHS equation, expr_bindings is null at check_condition
result = 0 if is_in(input, int)
```

The simple-LHS form parses, loads, and runs — but the `is_in` predicate clause always returns false (fail-safe on null `expr_bindings`), so the condition never fires. There is no compiler or runtime warning.

**Concern**: this is a silent trap. The R5/R6 end-to-end test (cycle 3b M5) discovered this constraint during implementation; the implementer adapted the test to rewrite-rule shape. Stdlib authors and LLM-generated `.fw` files writing `var = expr if is_in(var, set)` will encounter the same silent failure with no diagnostic.

**Reopen trigger**: first stdlib author or user reports confusion that `var = expr if is_in(...)` silently doesn't fire, OR cycle that ships dimensional-rejection stdlib rules using equation-conditional shape, OR LLM-benchmark arc (queued) surfaces this as a measured failure mode.

**Possible fixes**: (a) parse-time warning when `is_in` (or any predicate clause) appears in an equation-condition context; (b) runtime warning at the first equation-condition `check_condition` call where a predicate clause is skipped due to null `expr_bindings`; (c) lift the equation-condition path to ALSO populate `expr_bindings` from the equation's variables (semantic change — predicates would fire in equation conditions too; needs design call on whether predicate semantics make sense for equations).

**Documentation**: Language.md §17.4 "Context requirement" subsection (filed by cycle-3b doc-updater) captures the current constraint user-facing-ly. This Future.md entry tracks the resolution work.

## 89. `std::function` carrier migration for solver-boundary erasure — PARKED

**Surfaced gen-5 cycle 3d (2026-05-16, visionary V2)** as a perf-driven follow-up to the two existing `std::function`-based solver-boundary erasure carriers.

**Today**: two `std::function` thread-locals live next to each other in `src/expr.h` (around line 3613):
- `FuncInverter` (cycle 1 / Future #12 era) — invokes inverse equations from `.fw` sub-system definitions.
- `ExistenceChecker` (cycle 3d) — invokes `sub.resolve(parameter, {{return_var, v}})` for FUNCTION_SECTION sets.

Both use `std::function` for boundary erasure (type-erased lambda capturing `this` from `FormulaSystem` in `system.h`; the carrier lives in `expr.h` which is the lower layer and cannot name `FormulaSystem`).

**Concern**: `std::function` has known overhead — heap allocation for non-SBO captures, virtual dispatch on call, exception-spec erasure. For cold/rare paths (rule-firing during simplify), the cost is invisible. For hot paths — particularly FUNCTION_SECTION dispatch in tight matching loops — the std::function cost could become measurable. Visionary V2 framed this as a paired migration: if EITHER carrier shows up as a measured hot path, BOTH should migrate together to keep architectural consistency.

**Reopen trigger**: FUNCTION_SECTION dispatch OR FuncInverter shows up as >1% of solve time in a perf-auditor reproducer. Then BOTH `std::function` carriers should be migrated to `fn-ptr + opaque-void*` shape together (single trigger, single migration).

**Possible fixes**: (a) replace both `std::function<bool(string, double)>` and `std::function<vector<ExprPtr>(string, ExprPtr)>` with `bool (*)(void* ctx, string, double)` + opaque ctx pointer; FormulaSystem* threads through as ctx; (b) PIMPL-style erasure with a virtual base class; (c) accept the overhead and document it.

## 90. Recursive FUNCTION_SECTION reverse-solve (`is_in` over recursive sections) — DONE (gen-5 cycle 3h 2026-05-16)

**Surfaced gen-5 cycle 3d (2026-05-16, implementer architecture-emergent #1)** as a capability gap.

**Cycle 3h close (2026-05-16)**: shipped three coordinated fixes (A: `copy_metadata_to_sub` helper propagates parent settings to the pre-cached sub built by `register_function_section`; B: Strategy 5 self-circular filter skips compound `sub_var == target` bindings; C: Strategy 6 condition-aware emission via new `contains_var_in_condition(cond, var)` helper in `expr.h`). With all three applied, `is_in(8, fibonacci)` returns true via the canonical helper-equation body — NO `n = n` workaround required. Tests C1 (`is_in(8, fibonacci) == true`), C2 (`is_in(4, fibonacci) == false`), and C5 (forward `fibonacci(6) == 8`) all pass.

**Canonical body form**: helper-equation split (one helper per FORMULA_CALL term in the recurrence):
```
[fibonacci(n) -> result]
prev1 = fibonacci(result=?prev1, n=n-1)
prev2 = fibonacci(result=?prev2, n=n-2)
result = prev1 + prev2 if n >= 2
result = n if n <= 1
```
Direct-body form (`result = fibonacci(n=n-1) + fibonacci(n=n-2)`) was a PARSE ERROR at cycle 3h close; **superseded by cycle 3i (2026-05-17)** — both named-arg and positional direct-body forms now work (see Future #91 DONE). The helper-equation workaround above is no longer canonical; both forms are equivalent.

**Scope limit known at close (Future #94 NEW PARKED)**: `is_in(6, factorial)` is structurally blocked by a separate solver-strategy-ordering issue (factorial's first body equation has `n` on the RHS alongside a formula-output, triggering an algebraic Strategy 2 candidate that blows the depth budget before Strategy 6 fires). Forward `factorial(3) = 6` works; reverse parks. Sentinel test asserts the forward direction.

**Cycle 3g substrate that made cycle 3h possible**: M1 self_name_ field (enables forward recursive lookup), M2 try_formula resolve_memoized switch (prevents O(2^n) budget exhaustion on forward eval), M3-X currently_inverting guard (prevents the inverter cycle that self_name_ inadvertently exposed). These remain in place; cycle 3h fixes are layered on top.

## 91. Named-arg syntax in arithmetic expressions (`func(arg=value)` inside `+`/`-`/`*`/`/`) — DONE (gen-5 cycle 3i 2026-05-17)

**Surfaced gen-5 cycle 3g (2026-05-16, implementer architecture-emergent #2)** during M3 fibonacci test attempt; closed in cycle 3i via two coordinated fixes plus a trace-quality improvement.

**Cycle 3i close (2026-05-17)**:

1. **Fix Y — UNIFIED `extract_formula_calls`**: rather than spawn a parallel `extract_named_calls_no_query` function (planner's original proposal), the cycle 3i critic+visionary trio recommended unifying the two extraction paths. `extract_formula_calls` is now a non-static member that accepts an optional `FormulaSystem* self`; when provided, it handles BOTH `?`-form calls (existing path, unchanged) AND no-`?` named-arg calls (new path). The named-arg dispatch is hoisted into a `try_extract_named_call` helper for stack-frame economy. `parse_call_args` hard throw on empty `query_var` removed — callers now decide which flavor they got. **Net delta**: +30 LOC across helpers + main loop, vs the planner's +95 LOC duplicate path. Replaced an asymmetric pair (`extract_query_calls` and a would-be `extract_value_calls`) with one general primitive.
2. **Fix Z — `register_function_section` calls `resolve_positional_calls` on the pre-cached sub**: closes the parallel positional-form gap. Pre-cycle-3i, direct-body positional recursion like `result = fibonacci(n-1) + fibonacci(n-2)` parsed cleanly but never extracted the recursive calls into `FormulaCall` entries — solver then NaN'd. Same shape of substrate gap as cycle 3h Fix A (`copy_metadata_to_sub`) — see Future #96 PARKED for the consolidation trigger.
3. **Fix W — `load_lines` parse-error trace warning includes line content**: previously the catch emitted only "warning: skipping line ..." without the offending text; LLM consumers reading `--steps` output had no signal about which line vanished. Now: "warning: skipping malformed line in <source>: <line>: <parser-error>". Trace-only (not stderr) per visionary's LLM-consumer-debuggability rubric.

**Direct body forms now both work** (cycle-3g's helper-equation pattern is no longer canonical — both are equivalent):

```fw
[fibonacci(n) -> result]
result = fibonacci(n=n-1) + fibonacci(n=n-2) if n >= 2     # named-arg form (Fix Y)
result = n if n <= 1

[fibonacci(n) -> result]
result = fibonacci(n-1) + fibonacci(n-2) if n >= 2          # positional form (Fix Z)
result = n if n <= 1
```

**Tests**: 3i C1/C2 BLOCKING (is_in reverse for named-arg + positional forms), C3/C4 BLOCKING (forward eval for both forms), D1 DESIRABLE (multi-function arithmetic: `f(x=1) + g(y=2)`). All pass.

**Followups filed**: Future #95 (silent fall-through surface), Future #96 (substrate consolidation when third post-load pass is added).

## 92. Solver strategy ordering for self-referential FUNCTION_SECTION subs — DONE (gen-5 cycle 3h 2026-05-16)

**Surfaced gen-5 cycle 3g (2026-05-16, implementer architecture-emergent #3)** during M3 fibonacci test attempt.

**Cycle 3h close (2026-05-16)**: rather than reorder Strategy 6 ahead of Strategy 2, the cycle closed this via two complementary structural fixes that prevent the algebraic chain from entering its trap in the first place — and a third fix that lets Strategy 6 see the equations it needs:

1. **Fix A — `copy_metadata_to_sub` helper (closes Future #83 as part of the same cycle)**: propagates parent settings (notably `numeric_mode=true`) to the pre-cached FUNCTION_SECTION sub built by `register_function_section`. Without this, Strategy 6 was disabled on the sub even when the parent had numeric mode on, suppressing the only path that could solve recursive reverse-cases.
2. **Fix B — Strategy 5 self-circular filter**: in `enumerate_candidates`, skip the compound `sub_var == target` case (`n = n-1` inside `[fibonacci(n)->result]`). The downstream `prepare_sub_bindings` skip-logic only handles pure `Var(target)` bindings (`is_var(expr) && expr->name == skip_parent_var`); compound bindings slipped through and caused the FORMULA_REV candidate to re-enter the same sub with the same target. The pure-Var case (positional-arg sugar like `tpa_sq2(x)` expanding to `x=x`) is preserved.
3. **Fix C — Strategy 6 condition-aware emission** + new `contains_var_in_condition(cond, var)` helper in `expr.h`: extends Strategy 6's skip predicate to ALSO consult the equation's condition. An equation like `result = n if n <= 1` is structurally probeable for `n` (the condition constrains n's domain), but the original `lhs_var != target && !contains_var(rhs, target)` skip suppressed emission because `n` does not appear in the RHS literally. Cycle-3g's `currently_inverting` guard becomes a secondary safety net rather than the primary defense.

**Tests preserved**: cycle-3g C10 regression (`is_in(5, bad) == false` for non-terminating bodies) still passes — the currently_inverting guard catches the case that Fix B does not (`bad(n+1)` is a single FORMULA_FWD, not a Strategy 5 reverse). New cycle-3h tests C1/C2/C5 BLOCKING + C3/C4 DESIRABLE all pass; D3 (factorial reverse) parked as Future #94 (different structural blocker).

## 93. Strategy 6 emission predicate audit — PARKED

**Surfaced gen-5 cycle 3h (2026-05-16)** as a follow-up to the cycle 3h #92 close.

**Today**: cycle 3h extended Strategy 6's skip predicate to consult equation conditions (`target_in_cond`). The same emission predicate has two parallel checks downstream — `has_target` (line ~4079) and `has_formula_vars` (line ~4087) inside `try_resolve_numeric` — that may have the same condition-blindness. The cycle 3h audit did not extend those because no test surfaced a regression, but the structural symmetry suggests they could.

**Reopen trigger**: a SECOND case surfaces where an equation is structurally probeable but emission-filtered.

## 94. Self-referential FUNCTION_SECTION reverse-solve when body lacks a base-case `result = target` equation — PARKED

**Surfaced gen-5 cycle 3h (2026-05-16)** during cycle 3h D3 attempt (`is_in(6, factorial)`).

**Today**: fibonacci's reverse-solve works because its base-case equation `result = n if n <= 1` is directly solvable for `n` (Strategy 2 emits `n = result` with no recursion needed). Factorial's analogous base case is `result = 1 if n <= 0` — `n` does not appear in this equation at all. The only Strategy 2 candidate is from `result = n * prev if n >= 1`, which solves to `n = result / prev`; resolving `prev` recursively probes the FORMULA_FWD on `prev = factorial(...)`.

**Cycle 3j discovery (2026-05-26)**: cycle 3j attempted closure via a depth-swallow at depth=0 (let Strategy 6 NUMERIC fire after Strategy 2 EXPR depth-exhausts). The fix was implemented cleanly as the typed `FormulaDepthExceededError` sibling exception — but the negative-case tests (`is_in(4, factorial) == false`, `is_in(7, factorial) == false`) FAILED, and the positive cases passed for the WRONG reason. The deeper structural blocker was discovered: the visited-set Circular guard fires before depth exhaustion, the `runtime_error` is silently caught in `prepare_sub_bindings`, `sub_binds` becomes empty, `sub.resolve("result", {})` succeeds via the base-case `result = 1 if n <= 0` because `check_condition` (`expr.h:~2104`) defaults clauses with unbound variables to TRUE, and Strategy 2 then computes `n = result / prev = X / 1 = X` — a coincidental wrong answer that round-trips false but is non-throwing. `exists_for_function_section` returns `true` for ALL inputs that survive the EXPR path. **Strategy 6 NUMERIC never gets a turn because Strategy 2 "succeeds".** See Future #97 for the deeper structural issue and cycle 3j's four candidate fixes.

**Cycle 3h sentinel test**: forward `factorial(3) = 6` is asserted as confirmation that Fix A's settings propagation reaches the sub correctly; the reverse direction is documented in the test comment as the structural blocker.

**Possible fixes (cycle 3h original)**: (a) detect Strategy 2 candidates that require resolving a free variable equal to the formula's positional arg and demote them; (b) catch depth-budget exhaustion inside try_formula and continue to next candidate instead of propagating — **superseded by cycle 3j's typed exception, but does NOT close #94 because the actual blocker is the Circular-guard interception, not depth exhaustion**; (c) make Strategy 6 fire BEFORE Strategy 2 for self-referential FUNCTION_SECTION subs (the design path originally considered for #92, deferred because A+B+C closed fibonacci without it).

**Reopen trigger**: (a) user reports `is_in(<value>, <recursive_function_section>)` returns true-without-existence-proof or false-incorrectly for a function whose body lacks a base case directly solvable for the parameter; (b) cycle that broadens recursive-sequence support beyond fibonacci-shape; (c) Future #97 ships a structural fix that makes the API-boundary forward-verify reliable.

## 95. Silent parse-error discard surface — `--steps` warning vs. stderr promotion — PARKED

**Surfaced gen-5 cycle 3i (2026-05-17)** as a follow-up to Fix W's trace-warning addition.

**Today**: `load_lines` per-line resilience swallows `runtime_error` from parser/extractor failures and emits a trace.step warning (post-Fix-W: with the offending line + parser error). The warning is visible via `--steps` but NOT via stderr — silently-discarded equations appear only as "Cannot solve for X" downstream with no surface hint that a body line was dropped.

**Why parked, not promoted now**: stdlib loading involves intentional silent skips (dimension sections, predicate sections that the loader doesn't recognize until pass 2). Promoting to stderr would punish the common case with noise. Trace level is the right default; promotion is a per-line judgment that needs evidence.

**Reopen trigger**: (a) stdlib author or LLM consumer reports the silent discard as surprising in a real debugging session; (b) a single test reproducer shows the discard cost wall-clock minutes of confusion that an stderr line would have prevented; (c) `--strict` mode is added (would promote ALL parser warnings to errors).

**Possible fixes**: (a) promote the trace warning to a single stderr line per discarded equation (gated by `--strict` or `FWIZ_VERBOSE_PARSER`); (b) accumulate discards into a single summary line per load (`warning: N equations discarded in <source>`); (c) leave as trace-only.

## 96. `register_function_section` post-load pass consolidation — extract `finalize_sub_after_load_lines(sub)` helper — PARKED

**Surfaced gen-5 cycle 3i (2026-05-17)** via critic analysis of Fix Z as a second patch to the same substrate gap.

**Today**: `register_function_section` (system.h:~1140) now contains TWO patches across two cycles to bring the pre-cached FUNCTION_SECTION sub up to parity with a normally-loaded sub: cycle 3h Fix A (`copy_metadata_to_sub`), cycle 3i Fix Z (`resolve_positional_calls`). The normal path runs these (plus `compute_rewrite_groups`, `resolve_diff_in_equations`, `resolve_integral_in_equations`) from `load_with_sections` (system.h:~1236) after a sub's `load_lines` completes. The pre-cache path silently bypasses all of them; each new post-load pass needs to be manually mirrored.

**Why parked, not refactored now**: two patches is "queue it"; three is "ship it" per demand-pull abstraction. Reworking the post-load substrate now would couple a clean 2-LOC fix (Fix Z) to a multi-touchpoint refactor needing its own design round. The substrate shape isn't yet observed enough to design well.

**Reopen trigger**: a THIRD post-load pass is added in `load_with_sections` that must also fire in `register_function_section`'s pre-cache path. Concretely: `git log -p src/system.h | grep -B 2 'finalize\|post-load\|load_with_sections' | grep '+' | grep -v '^+++ '` should reveal the next addition. When it does, extract `void finalize_sub_after_load_lines(FormulaSystem& sub)` from the inline block at `load_with_sections:~1236` and call it from both `load_with_sections` and `register_function_section`. Current count: 2 sites.

## 97. `exists_for_function_section` returns wrong-answer-coincidence for non-fibonacci-shape recurrences — PARKED

**Surfaced gen-5 cycle 3j (2026-05-26)** mid-implementation via the dry-run rule. The cycle attempted to close Future #94 via a typed `FormulaDepthExceededError` sibling exception with depth=0 swallow semantics; the fix was implemented cleanly but failed to close #94 because the actual blocker is structurally distinct from depth exhaustion.

**Today**: `exists_for_function_section(name, X)` calls `sub.resolve("n", {result: X})` and returns `true` iff the resolve does not throw. For factorial, triangular, pow2, fib3, and any other recurrence whose base case does NOT contain the parameter (`result = 1 if n <= 0` instead of `result = n if n <= 0`), Strategy 2 EXPR emits `n = result/prev` (or analogous), descends to resolve `prev`, hits the visited-set Circular guard at depth ~2 (the `runtime_error` thrown at `solve_recursive:~4380`), and `prepare_sub_bindings` silently catches it. `sub_binds` becomes empty. `sub.resolve("result", {})` then succeeds via the base-case clause because `check_condition` (`expr.h:~2104`) defaults clauses with unbound variables to TRUE — picks `result = 1 if n <= 0`, returns 1. Strategy 2 then computes `n = X / 1 = X`. Wrong answer; non-throwing. `exists_for_function_section` returns `true` for ALL inputs that survive this path.

**Cycle 3j typed-exception refactor (shipped)**: replaces `msg.find("depth") != std::string::npos` stringly-typed match at `try_formula:~4455` and `try_resolve:~4577` with a typed catch on `FormulaDepthExceededError` (sibling, derives from `runtime_error`). Net structural-legibility improvement. Does NOT close #94, but does NOT regress either: the failure mode is unchanged at the API surface (cycle 3h's D3 sentinel `forward factorial(3) = 6` still passes; reverse-solve is still parked).

**Four candidate fixes** (from cycle 3j implementer log AE-1, none implemented):

1. **Forward-verify at API boundary**: `exists_for_function_section` runs `sub.resolve("n", {result: X})` to get candidate `n_val`, then runs `sub.resolve("result", {n: n_val})` to verify it matches `X`. ~5 LOC at one site. RISK: rejects the wrong-coincidence answer cleanly but does NOT find the right answer for positive cases — Strategy 6 NUMERIC still doesn't fire. Would need to be combined with retry-on-verify-fail (try alternate strategy candidates) or with Fix (4).

2. **Tighten `check_condition` unknown-clause default**: clauses with unbound variables should NOT default to TRUE. Currently `clause_result = !val.has_value() || val.value();` at `expr.h:~2104`. RISK: this default is load-bearing for piecewise functions evaluated at parse-time / partial-binding; broad-impact change requiring its own design round.

3. **Tighten `prepare_sub_bindings` empty-binding response**: when both the binding expression AND the bridge are unresolvable, refuse to invoke the sub at all. RISK: same as (2) — many legitimate FORMULA_FWD calls deliberately under-bind during exploration.

4. **Strategy 6 NUMERIC before Strategy 2 EXPR for FUNCTION_SECTION queries**: re-order candidate emission so NUMERIC fires first when the target is the section's parameter and the binding is the return_var (a known integer-image existential query). Most direct. RISK: candidate ordering is global; re-ordering for one shape may slow others — requires careful test corpus audit.

**Most likely path** (per cycle 3j orchestrator + implementer notes): combine **(1) + (4)** — Strategy 6 fires first for the existential-query shape; forward-verify at the API boundary as a defense in depth. ~30-50 LOC across `exists_for_function_section` + `enumerate_candidates` strategy ordering.

**Reopen trigger**: (a) a user files `is_in(<value>, <recursive_function_section>)` returns true-without-existence-proof OR false-incorrectly for a non-fibonacci-shape recurrence; (b) a future cycle takes on the GENERALITY-axis closure for recursive set membership; (c) a closely-related Future surfaces (multi-parameter recurrence, mutual recursion) that would benefit from the same forward-verify substrate.

## 98. `make sanitize` stack-overflow — DONE (2026-06-06, cycle 3k)

**Root cause**: `solve_recursive` and `try_resolve` passed `std::set<std::string> visited` by value — each recursive call copied the full set, producing O(depth²) frame growth. Under ASan's fat-frame instrumentation this overflowed the stack at `max_formula_depth=20` in the mutual-recursion test (trigger: `test_recursion_depth_guard`). Recursion was always bounded by the depth guard; only the physical stack overflowed under ASan.

**Fix A — Makefile stack budget** (`Makefile:36`): `ulimit -s unlimited;` prepended to the asan recipe run line, same subshell as the test invocation. Sized for the `max_formula_depth=1000` contract (not the depth-20 trigger artifact). Fix A alone restores the gate; Fix B is the structural improvement.

**Fix B — by-reference `visited` + RAII** (`src/system.h`): `visited` changed from by-value to `std::set<std::string>&` in both `solve_recursive` and `try_resolve`. A `VisitedGuard` RAII struct (insert-on-enter / erase-on-exit) placed after the early-throws preserves the path-from-root invariant the prior by-value copy gave, including correct restore on exception unwind. Call sites updated (7 sites audited from the design list). Removes O(depth²) set-copy on the hot solve path → O(depth log depth).

**Gates**: `make sanitize` 3805/3805 PASS (back online), `make test` 3805/3805 PASS, `make analyze-fast` exit 0.

**Sibling by-value pattern** not implicated in #98: `solve_all` / `derive_recursive` / `try_derive` retain by-value `visited` copies with structural differences that make the RAII recipe non-trivial to apply (see Future #99).

## 99. Extend by-reference `visited` to `solve_all` / `derive_recursive` / `try_derive` — PARKED

**Surfaced gen-5 cycle 3k (2026-06-06)** during the #98 fix audit.

**Today**: the by-value `visited` pattern exists in `solve_all`, `derive_recursive`, and `try_derive` (7+ additional `{}`-temporary call sites: lines 1823, 1993, 2024, 2030, 2040, 2462, 4058). These were NOT implicated in #98's stack-overflow (that triggered exclusively through `solve_recursive` ↔ `try_resolve`), but carry the same O(depth²) set-copy cost.

**Structural differences** that make the RAII recipe non-trivial: `solve_all` returns an empty vector on failure (no dead-key pre-check throw before insert — different guard sequence than `solve_recursive`); `derive_recursive` and `try_derive` signal failure via `nullptr` return rather than exceptions (RAII-on-unwind is therefore not load-bearing for them — only the frame-shrink applies).

**Same `&`+RAII recipe applies** once the structural differences are scoped per function. The frame-shrink win is real even without the exception-unwind requirement.

**Reopen trigger**: perf-auditor flags the by-value `visited` copy on the `derive_all` / `solve_all` families as a measurable hot-path cost, OR a deep-recursion derive query surfaces the O(depth²) copy cost in profiling.

## 81. Named compound-dimension aliases (`[speed] := length/time`) — PARKED

**Surfaced 2026-05-14** during gen-3 cycle 1 design (Answer C staging deferral).

**Today** (after gen-5 cycle 3c lands): atomic dimension annotations work — `m:mass = 10kg`, `t:time = 5s`. Intersection works — `n:(int, mass) = 5kg`. Compound dimension propagation works via `compute_dim` (cycle 3c substrate) — `v = distance / duration` automatically has dim `{length:1, time:-1}` as a `DimMap`. Named compound-dimension aliases (`[speed] := length/time`) remain the missing piece.

**Missing**: there's no syntax to NAME a compound dimension. A user wanting `v:speed = 10m/s` cannot — `speed` isn't declared as a dimension. They must either (a) leave `v` unannotated (engine still tracks via propagation) or (b) use intersection with both atoms `v:(length, time)` (semantically wrong — that's a multi-typed binding, not a length-per-time quantity).

**Proposed syntax**: `[speed] := length/time`. Declares `speed` as an alias for the dimension `length/time`. After declaration, `v:speed = ...` works the same as `v:(...)` would if compound dim arithmetic existed in type position.

**Why deferred to demand-pull**: the Answer C staging from gen-3 cycle 1 commits to "atomic + intersection only" at cycle 2. Compound-dim naming is the natural escape valve when users find atomic + intersection insufficient — but it MIGHT not be needed in practice. Most physics calculations don't need to NAME the intermediate compound dims; propagation tracks them silently.

**Reopen trigger**: first user-filed issue or doc PR referencing compound dim names (e.g. "how do I declare `v` as a velocity without naming it?"). At that point, demand is concrete and the syntax is straightforward to add (~20-30 LOC `parse_line` + `dim_map_` extension).

**NOT proposed**: ad-hoc type-position arithmetic (`v:length/time` directly in annotation without prior `:=` declaration). Speculative; meta-trigger ("evaluate after #81 ships"). May surface organically post-#81 — re-propose then.

## 80. Multi-file CLI load / `@include` directive — IN-PROGRESS (M1+M2 shipped 2026-06-23)

**M2 SHIPPED 2026-06-23 (opt-in `--strict-includes`, default-OFF — zero breaking changes).** Delivered:
- `strict_includes_` bool on `FormulaSystem` (default `false`), propagated via `copy_metadata_to_sub`
  so a strict parent's sub-systems inherit strict resolution.
- `--strict-includes` CLI flag (`main.cpp`) → sets `sys.strict_includes_ = true` before load.
- `load_sub_system` strict gate: in strict mode the base_dir filesystem auto-probe is SKIPPED
  entirely. A cross-file call resolves ONLY via (a) the in-system / `custom_function_defs_` `@def:`
  cache, (b) the `@include` allow-list (`resolve_from_included()` — stem-scan of `included_files_`),
  or (c) the `-I`/`FWIZ_PATH` search path (`resolve_file_path(..., exclude_base_dir=true)` — base_dir
  excluded so co-location alone is not a channel).
- Explicit-systems model (Option C second layer): a strict-mode callable system MUST declare an
  explicit `[name(args)->ret]` section. A FLAT `@include`'d file (bare equations, no header) merges
  its equations but is NOT callable by stem — the strict gate throws when the resolved file has no
  named section.
- `StrictIncludeError` sibling exception (NOT derived from `std::runtime_error`) so the helpful
  "add `@include`" message propagates past the solver's silent `catch (const std::runtime_error&)`
  sites instead of being downgraded to "Cannot solve for X". `build_strict_include_error()` names
  the call, lists the searched dirs, and lists currently-`@include`'d stems.
- 8 new asserts (`test_include_m2`). All 4008 tests pass; sanitize + analyze-fast clean.
- `--legacy-implicit` flag DEFERRED to M3 — it only becomes load-bearing once the default flips
  (a no-op stub now would be dead surface).

**M1 SHIPPED 2026-06-23 (COEXIST infra, additive — zero breaking changes).** Delivered:
- `include_dirs` (`std::vector<std::string>`) + `included_files_` (`std::set<std::string>`)
  fields on `FormulaSystem`, both propagated via `copy_metadata_to_sub`.
- `-I <dir>` CLI flag (repeatable, order-preserving; attached `-Idir` form also accepted)
  and `FWIZ_PATH` env var (split on `:`/`;`, appended after `-I` dirs) → populate `include_dirs`.
- `@include "path.fw"` directive: `process_includes()` pre-pass in `load_with_sections()`
  (runs before `split_sections`), resolves each path via search order, recursively `load_file`s
  it (merging definitions into the current system), records the abs_path in `included_files_`,
  and blanks the line. Quoted form primary; unquoted tolerated. `BaseDirGuard` RAII restores
  `base_dir` around each recursive load; `currently_including` thread-local + RAII guard detect
  include cycles (distinct from #69's `currently_loading`).
- Search order: (1) file-relative (`base_dir`), (2) each `-I` dir, (3) each `FWIZ_PATH` dir.
  File-not-found error names every searched directory.
- COEXIST hook: `include_dirs` ALSO feeds `load_sub_system` as a fallback (only when the
  base_dir probe points at a missing file), so a cross-file formula call resolves a section
  file found via `-I`/`FWIZ_PATH` WITHOUT co-location and WITHOUT `@include`. The pre-existing
  base_dir auto-probe is unchanged (removal is M3).
- 12 new tests (`test_include_m1`). All 4000 tests pass; sanitize + analyze-fast clean.

**STILL PENDING:**
- **M3**: migrate `examples/box.fw` + `stdlib/combinatorics/hyp_pmf.fw` (add `@include`); add
  explicit headers to flat callable example files; migrate ~32 `tests.cpp` cross-file fixtures;
  flip `strict_includes_` default to `true`; add the `--legacy-implicit` opt-out flag (deferred
  from M2 — load-bearing only after the default flips); remove implicit base_dir auto-probe +
  flat-file-as-implicit-system; close #80 DONE. See the staging design for the full plan.

**Surfaced 2026-05-13** during Units cycle 3 implementation. The implementer flagged: docs/Language.md previously documented `fwiz stdlib/units/si-minimal.fw my_formula.fw(mass=?, length=9km)` as a CLI usage pattern, but the current CLI accepts exactly ONE filename. `main.cpp:129` concatenates non-flag args into a single `query_str` and `parse_cli_query` takes a single filename. The multi-file form silently fails. Cycle 3 worked around this by inlining the SI-base bindings into `stdlib/physics/mechanics.fw`. Cycle 4 retracted the misleading example in docs/Language.md.

**The real fix** — and the question the design needs to settle — is whether fwiz should support library composition at the language level. Two candidate mechanisms:

1. **CLI-level multi-file load**: `fwiz file_a.fw file_b.fw(query_args)` parses multiple files, concatenates their equations, then runs the query. Lowest-impact change. ~30-50 LOC in `main.cpp` argument parsing + `parse_cli_query` extension. Risk: ambiguity when a non-file arg is intended (probably resolved by requiring all-but-last args to NOT contain `(`).

2. **`.fw`-level `@include` directive**: `@include stdlib/units/si-minimal.fw` at the top of any `.fw` file recursively loads the named file. Mirrors `#include` in C. Bigger surface; requires path resolution rules (cwd? file-relative? search-path?), cycle detection (same problem solved for cross-file resolution by Future #69), and a clear semantic for shadowing (does an `@include`d binding override a parent binding?).

**Today's workaround**: inline the catalog into each consuming `.fw` file (the `stdlib/physics/mechanics.fw` pattern). Cheap but it duplicates bindings, and if the units catalog changes its values the consumer must be updated in lockstep.

**Reopen trigger**: a second stdlib library file needs the units catalog (e.g. `stdlib/physics/thermo.fw` or `stdlib/finance/compound.fw`), and the inline-bindings workaround becomes unworkable, OR a user reports the multi-file load not working with a concrete reproducer.

## 79. Deferred-identifier error-quality (cycle-2 follow-up) — IN-SCOPE

**Surfaced 2026-05-13** during Units cycle 2 review.

**Behavior change introduced in cycle 2**: Future #73's fix routes any RHS expression that parses cleanly but evaluates empty to `synthetic_equations`. Before #73, common typo patterns like `y=abc` or `y=e5` produced an early diagnostic at CLI-parse time: `Error: Invalid value 'abc' for variable 'y'`. After #73, the binding becomes a synthetic equation `y = abc`, the post-load resolution fails to bind `abc`, and the user sees one of:
- `Error: Cannot solve for 'y'` (when querying `y=?`).
- `y = abc` (silent symbolic fallback via `synthetic_aliases` rendering path).

Either is less diagnostic than the old "Invalid value" message for *clear typos* (the user wrote a typo, not a deferred reference).

**Reproducer**:
```bash
fwiz '(y=?, y=abc)'       # Before #73: Error: Invalid value 'abc' for variable 'y'
                          # After #73: Error: Cannot solve for 'y'
fwiz '(y=?, y=e5)'        # Before:    Error: Invalid value 'e5' for variable 'y'
                          # After:     y = e5  (silent symbolic fallback)
```

The problem is **discrimination**: at CLI-parse time, we cannot distinguish "user typed a typo" from "user wrote a deferred reference that the file will resolve." The cycle-2 fix correctly defers — but the user-visible error quality regresses on the typo case.

**Fix candidates** (un-evaluated):
1. **Heuristic detection at parse time**: if the parsed RHS is a single Var that's NOT a known builtin AND looks like a typo (e.g. `e<digits>` resembling incomplete scientific notation), emit a warning to stderr alongside the synthetic equation. ~10 LOC.
2. **Post-load diagnostic**: after `sys.load_string(synthetic_equations)`, walk each synthetic_alias and check if its dependencies are now bound; if any are still free, emit a "deferred identifier 'abc' is not defined in the loaded file" warning. ~15 LOC.
3. **Lazy error**: keep both the typo case and the deferred-reference case erroring; differentiate by checking if the unresolved Var name appears in the loaded file's equations. Most precise but most code.

**Reopen trigger**: user reports being confused by the new error-quality, OR LLM-ergonomics benchmark (queued arc) flags the deferred-identifier shape as a friction point, OR a follow-up Units cycle opens parser.h for unrelated work and the heuristic-warning bundle is cheap.

## 78. Are constants units? Unify the constants/units catalog. — ✅ DONE-by-design (2026-05-14, gen-3 cycle 1)

**Decision (gen-3 cycle 1)**: HYBRID model + Answer C staging.

**Model**: bare `[name]` sections declare dimension categories; their bindings inside ARE units of that dimension. `:` is the binding-annotation operator (`m:mass = 10kg`). Intersection grammar `(t1, t2, ...)` for multi-typing. **Dimension propagates through MUL/DIV/POW/NEG in value-world** (Approach A semantics) — both `10kg` and `10 * mass.kg` carry dimension `[mass]` automatically. Constants like `pi`, `e`, `phi` remain C++ builtins with implicit `dimensionless` (no section membership). The C++/stdlib split for constants becomes a performance concern, not a semantic concern. All scalar bindings are uniformly "named bindings in some namespace, possibly dimensioned."

**Cycle-2 substrate scope** (Answer C staging — defers higher grammar):
- `:` lexer token + `parse_line` annotation extension.
- Bare `[name]` sections register as in-file sub-systems (`@dim:` cache-key prefix in `sub_systems`).
- `dim_map_` registry on `FormulaSystem` (cycle 2: `std::string` per variable; cycle 3 promotes to `map<DimName, int>` exponent algebra).
- `is_in_dimension(expr, dim)` predicate (#65 schedule, 2nd consumer after `is_neg_num`).
- Intersection grammar `(t1, t2, ...)` — atomic type names ONLY, no operators inside parens.

**Deferred to demand-pull future cycles**:
- **Cycle 3 (gen-3 arc)**: `compute_dim` propagation algorithm + `BuiltinMeta.dim_propagate` callback field + Dim algebra (`map<string,int>` exponent representation). Triggered by ADD/SUB mismatch enforcement OR compound-expression rejection rules in stdlib `.fw`.
- **Future #81 (NEW PARKED)**: named compound-dimension aliases (`[speed] := length/time`).
- **Implicitly rejected (NOT filed)**: ad-hoc type-position arithmetic (`v:length/time` directly in annotation) — speculative, no concrete consumer.

**Resolves**: Future #77 → REJECTED (see REJECTED.md), Future #7b → two-step DONE framing (atomic-Var rejection rules unblocked after cycle 2; compound-expression rejection requires cycle 3).

**Design doc**: `.fwiz-workflow/design-proposal.md` (gen-3 cycle 1, 2026-05-14). Plan-mode refinement log in `/home/izzo/.claude/plans/quiet-roaming-quiche.md`.

---

### Historical context (#78 original framing — superseded)

**Surfaced 2026-05-13** by the user immediately after the #76 denylist shipped: "if we're going to keep `i` and `pi` and `phi` as valid suffixes we have to define them as units... OR this surfaces an interesting question — can all constants be defined as units?"

**The question.** Today's mental model:
- **Builtin constants**: `pi`, `e`, `phi`, `i` are bound in fwiz's C++ source as scalar Vars with specific values.
- **Units**: `kg`, `m`, `s`, etc. live in `stdlib/units/si-minimal.fw` as plain `kg = 1` bindings.

But under cycle 1's NUMBER-IDENT desugar (Option C), both groups behave identically: `2pi` and `2kg` parse to `MUL(Num(2), Var(...))` and resolve through the standard Var-binding pipeline. **There is no language-level distinction between "constant" and "unit."** They are both "named scalar bindings the user can multiply against numbers."

Three plausible framings:
1. **Constants are organizational, not structural.** `pi`/`e`/`phi`/`i` live in C++ for performance / always-available reasons. `kg`/`m` live in stdlib for user-extensibility. The NUMBER-IDENT desugar treats both as multiplicands and that's correct. Today's situation.
2. **All scalar bindings ARE units; constants are a subset.** A `stdlib/units/constants.fw` (or `stdlib/constants/math.fw`) catalog could declare `pi = 3.14159...`, `e = 2.71828...`, etc., mirroring the user-extensibility model. The C++ builtin bindings become a performance optimization, not a conceptual layer.
3. **Units have dimension tags; constants don't.** Future #7b dim-analysis would distinguish `kg` (mass) from `pi` (dimensionless). Under this framing, constants ARE units — they just carry the dimensionless tag.

**Why a design cycle is needed.**
- The #76 denylist (`e` reserved) was justified as "looks like incomplete scientific notation." Under framing 2 or 3, the rationale is weaker — `e` is a "dimensionless unit," same as `pi`. Is the disambiguation still warranted, or should `2e` mean `2 * Euler` consistently with `2pi`?
- Migrating `pi`/`e`/`phi`/`i` from C++ builtins to `stdlib/.fw` files is structurally clean but introduces a load-order dependency (stdlib must load before any user expression). Mitigations: auto-load, embed-as-source, etc.
- Future #7b dim-analysis cannot proceed without settling this question — does `force = mass * pi` carry units `kg`, or `kg * dimensionless = kg`? The answer is "trivially the same" only if constants are dimensionless units.
- Cycle 4 of the Units arc (dim-analysis) WILL need this resolved.

**Recommended design pass timing**: before Units arc cycle 4 (dim-analysis), or as cycle 4's design phase itself.

**Reopen trigger.** User requests `/plan-campaign`-style design cycle for this question, OR Units cycle 4 (dim-analysis) brief enters planning.

## 77. `_` as multiplication separator for reserved-prefix identifiers — ✅ REJECTED (2026-05-14, gen-3 cycle 1)

**Disposition**: REJECTED. Moved to `docs/REJECTED.md`. The hybrid dim model (Future #78) covers both motivating use cases without requiring a `_` separator: `i * km` works via dim propagation (`dim(i) = {}`, `dim(km) = {length:1}`); `c:(complex, length) = i * km` works via intersection annotation. Vision violation: adds a specialization (new separator syntax) where the general mechanism (`*` and `:`) already covers it. Per "Remove > Add."

**Reopen trigger**: enough user reports of complex-number arithmetic friction accumulate to push priority back up AND the hybrid surface (cycle 2 of gen-3 arc) demonstrates the friction remains in practice. The user's "I'm not saying it's correct, it just sort of makes sense" framing acknowledged this might not be worth the design cost; the gen-3 cycle 1 verdict is that it isn't.

---

### Historical context (#77 original framing — superseded)

**Surfaced 2026-05-13** by the user immediately after Future #76 shipped: "for `i` we might have to accept `_` as a valid suffix so `i_km` or `i_f`". Follow-up clarification: "I'm not saying it's correct btw, it just sort of makes sense — we should have a proper plan-critic-visionary cycle for that."

**Problem.** `ikm` parses as a single IDENT (`read_ident` is greedy). A user wanting `i * km` (imaginary unit × kilometer) must write `i*km` or `i * km` — there's no implicit-multiplication path for IDENT-IDENT adjacency. Same for `if` (imaginary × farad) which is also a language keyword. The Units arc's NUMBER-IDENT desugar (`100kg → 100 * km`) handles number-prefixed cases but offers nothing for the imaginary-unit-prefixed case that's common in complex/AC-circuit math.

**Sketch (un-evaluated).** Reserve `_` as a prefix-multiplication separator for specific reserved single-character builtins: `i_km → i * km`, `e_X → e * X`, possibly `pi_X` etc. The lexer or parser would split the IDENT at the first `_` if the prefix is a reserved single-char.

**Why this needs a design cycle.**
- Conflicts with the established `<thing>_<thing>` convention for user-defined unit names (`km_per_hr = km / hr` — the cycle-1 stdlib pattern). If `e_X` means `e * X`, can users still define `e_squared` as a variable? Probably yes (the rule only fires for `e_` at the START of an expression). But the disambiguation needs careful design.
- Asymmetric: `i_km` works but `i.km` doesn't. Why one symbol and not the other?
- Scope creep risk: once `i_` and `e_` are reserved prefixes, does the same rule extend to `pi_`, `phi_`? What about user-defined constants like `mu0` (vacuum permeability)?
- Backward-compat: existing `.fw` files may contain `i_*` identifiers (user-defined). Rule must not silently break them.
- Better alternative may exist: just require explicit `*` in these cases (status quo). The "sort of makes sense" framing is honest — this may not be worth the design cost.

**Design questions** (for the plan-critic-visionary cycle):
1. Which prefixes are reserved? `{i, e}` only, or wider?
2. Lexer-level split vs parser-level split — which is structurally cleaner?
3. Backward-compat: how do we audit existing user `.fw` files for collision?
4. Does the rule extend to NUMBER-IDENT cases (`2i_km → 2 * i * km`)?
5. Is the rule worth the cost? (Status-quo alternative: require `i * km`.)

**Reopen trigger.** User requests a `/plan-campaign` or design cycle specifically for this question, OR enough user reports of complex-number arithmetic friction accumulate to push the priority up.

## 76. Reserved-word denylist for NUMBER-IDENT desugar — ✅ DONE (2026-05-13, cycle 1.1)

**User direction at the cycle-1 close (2026-05-13)**: "e like if is a keyword not usable as unit, another one is i the imaginary number ikm is a hard parse, a unit called f is even trickier because it becomes if".

**Shipped denylist**: `{if, iff, e}`. The NUMBER-IDENT desugar in `parser.h primary()` returns `Num(v)` (without advancing the IDENT) when the trailing IDENT matches one of these reserved words. Pre-cycle silent-drop behavior is restored for these cases:
- `2if`  → `Num(2)` (was post-cycle: `MUL(2, Var("if"))` → error)
- `2iff` → `Num(2)`
- `2e`   → `Num(2)` (was post-cycle: `MUL(2, Var("e"))` = 2*Euler — silently surprising for users typing `2e0` etc.)

`i`, `pi`, `phi` are intentionally LEFT OFF the denylist:
- `2i`  → `MUL(2, Var("i"))` — canonical complex-literal pattern, kept.
- `2pi` → `MUL(2, Var("pi"))` — common math shorthand, kept.
- `3phi` → `MUL(3, Var("phi"))` — same.

Tests pinned in `test_unit_suffix`: 7 new ASSERTs covering each denylisted case + non-denylisted regressions. Tests 3430 → 3437 (+7).

---

**Original filing context** (preserved for trace):

**Surfaced 2026-05-13** during cycle 1 of the Units arc review (reviewer NIT #1 + #2).

**Behavior change introduced in cycle 1**: Previously, `y = 2if`, `y = x + 1 iffy`, and similar typo-tolerant inputs were silently dropped at parse time — the trailing `IDENT("if")` or `IDENT("iffy")` was consumed by the lexer but discarded by the expression parser. After Units cycle 1's NUMBER-IDENT desugar, these inputs now produce `y = 2 * if` and `y = x + 1 * iffy` respectively. Since `if` / `iffy` are unbound variables, the solver loudly errors with "Cannot solve for 'y'".

**Reproducer**:
```bash
fwiz '(y=?) y = 2if'                # pre-cycle: y = 2 (silent). post-cycle: error.
fwiz '(y=?) y = x + 1 iffy; x = 5'  # pre-cycle: y = 6 (silent). post-cycle: error.
```

**Is this a bug?** Two framings:
1. **Loud-error is correct**: silently dropping typos contradicts the "deterministic, perfectly logical reasoning tool" claim. Same shape as the matrix-arc's ragged-literal fix (silent-propagation → loud-error). This framing says: ship it, don't add a denylist.
2. **Reserved-word denylist is correct**: `if` and `iff` are language keywords; absorbing them into a multiplication is structurally wrong (the user's intent is keyword-related, not arithmetic). A small denylist (~5 LOC) on `{"if", "iff"}` in `parser.h` `primary()`'s NUMBER-IDENT branch restores the keyword status while keeping the unit-desugar working for non-reserved IDENTs.

**Recommended fix** (cycle 2 opener candidate, ~5 LOC + 4 tests):
```cpp
// In src/parser.h primary() NUMBER branch, before the desugar:
if (ident_name == "if" || ident_name == "iff") {
    return Expr::Num(v);  // restore pre-cycle behavior: drop the keyword token
}
```

**Why this is cycle-2 work, not a cycle-1 in-cycle fix**:
- Reviewer flagged it as APPROVE WITH NOTES, not BLOCK. Cycle 1 ships.
- The decision is a real semantic call (silently-tolerate-typo vs loudly-fail-on-typo) and benefits from an explicit user signal that the loud-error behavior is undesirable. Pre-emptive denylist may not match user preference.
- Cycle 2's Future #73 work (CLI-arg unit-suffix evaluation) is in the same parser.h neighborhood — bundling these is clean.

**Reopen trigger**: user reports a real `.fw` file or `.fwiz` invocation where the new loud-error behavior is unwelcome, OR cycle 2 opens parser.h for the #73 CLI-arg work and bundles this for free.

## 72. `make analyze-full` (clang-tidy) hang re-occurrence — IN-SCOPE

**Surfaced 2026-05-13** during ROADMAP gen-2 planning. User reported: "clang-tidy hangs that's why we don't run it anymore — we can try at the end but if it takes longer than 2 hours it's not going to complete."

**Context.** The 2026-05-07 bisection (`debug-analyze-full-hang.md`) excluded `bugprone-exception-escape` and `bugprone-unchecked-optional-access` from the check set, dropping wall-clock from infinite-hang to ~10s. CLAUDE.md and `.claude/agents/fwiz-orchestrator-ops.md` both claim "~10 s post-fix." User observation contradicts that claim — in practice the tool still hangs to the point where it's been retired from the per-cycle workflow.

**What changed between the 2026-05-07 fix and now (unknown).** Possibilities:
- A newer check (e.g. `clang-analyzer-*` or `performance-*` family) introduced an analogous whole-program hang.
- A code change introduced a pattern that triggers a different pathological check.
- The 2026-05-07 measurement was on a different surface; cumulative diff since then exposes a new hang.

**Fix surface (next time `make analyze-full` is investigated):**
- Re-bisect against the current source. `git checkout 2026-05-07-baseline && make analyze-full` should still be ~10s; deltas since then are the bisection target.
- Probably another bugprone-* or clang-analyzer-* check has a similar whole-call-graph dataflow explosion.
- Either narrow the check set further or migrate to a different static-analysis tool (e.g. cppcheck with extended scope, or include-what-you-use for the dependency-direction checks).

**Process implication.** Per the orchestrator-ops cross-cycle escalation rule ("3+ cycles with zero successful runs → escalate to debugger"), the threshold tripped quietly some time ago — the rule didn't fire because the orchestrator profile claims the tool works. Updating the profile to acknowledge the hang is overdue.

**Reopen trigger.** Either (a) the user wants to run `make analyze-full` as a one-shot oracle (timeout 2hr) and the result informs whether re-bisection is worth a debugger cycle; OR (b) `make analyze-full` becomes important for a release / CI gating; OR (c) a parallel arc surfaces analyze-full evidence as a prerequisite.

**Adjacent action**: update CLAUDE.md and `.claude/agents/fwiz-orchestrator-ops.md` to acknowledge the hang re-occurrence and retire clang-tidy as a per-cycle oracle recommendation. The cycles-since-last-clang-tidy tracking should retire too — the right counter is "cycles since debugger investigation" now.

## 68. True structs / systems-as-structs — PARKED

**Surfaced 2026-05-13** during completeness-arc planning. The matrix-arc work (#14 vec/mat, future quaternions) clarified that vec/quat are arrays — their identity is "ordered tuple with algebraic operations", not "aggregate of named fields". #15 (flat-naming) handles the "named scalar components" case for formulas where `position_x`, `velocity_y` etc. read naturally. So the simple "add `.x` swizzle access to vec literals" path is deliberately not taken.

But true encapsulating structs are a legitimate longer-term ask:
- Nested aggregate paths read better than flat naming in some domains.
- Multi-equation aggregates (rigid body = mass + position + velocity + inertia tensor) want one logical home, not six naming conventions.
- Sum types / variants long-term.

**The dot operator is already half-implemented.** Fwiz's CLI already uses `.` as the path-into-aggregate operator: `geometry.triangle(...)` reads "section `triangle` inside `geometry.fw`" and the parser at `system.h:3927` splits the filename on the first dot. This is a fully-shipped convention.

**Hypothesis — systems-as-structs.** `FormulaSystem` is already an aggregate of equations, bindings, and section refs. Cross-file calls already aggregate a sub-system into the parent scope. If sub-system access is promoted from CLI-level to in-equation level, "structs" fall out by generalizing an existing convention rather than introducing a new primitive:

```
# rigid_body.fw — defines a system with public state fields
mass = ?
position_x = ?, position_y = ?, position_z = ?
velocity_x = ?, velocity_y = ?, velocity_z = ?

# main.fw
body = rigid_body(mass=10, position_x=0, ...)
body.velocity_y = body.velocity_y + gravity * dt   # dot routes into sub-system
```

The dot in `body.velocity_y` is the same dot that today routes `geometry.triangle` to a section — just at expression level instead of CLI level. Implementation cost would be largely parser-level; the systems-aggregate already exists, and the dot already means this thing.

**Open design questions** (need a research phase before reopening):

1. **Static or dynamic shape**: is `body` a fixed-schema aggregate (fields declared via the sub-system's variables) or a dynamic bag (any `body.field` is a new binding)?
2. **Equation routing**: when `body.velocity_y = body.velocity_y + ...` writes to a sub-system field, does the parent equation REPLACE, ADD to, or LAYER OVER the sub-system's own equation?
3. **Instance lifetime**: do `body_a` and `body_b` from the same `rigid_body.fw` share state? Current cross-file machinery already creates one sub-system per call site — that may be the right answer.
4. **Algebraic interop**: does `body.position` auto-aggregate into a vec literal `[body.position_x, body.position_y, body.position_z]` for use with vec builtins? Or stay separate?
5. **Schema declaration**: is the source `.fw` file the schema (declared implicitly via its top-level variables), or does fwiz need an explicit `[struct ...]` declaration form?
6. **Sum types / variants**: orthogonal feature, or natural fall-out of system-aggregate composition?
7. **Disambiguation from `filename.section` CLI form**: at expression level `body.velocity_y` is unambiguous (no `.fw` suffix, no filename context) — but the parser may need explicit signaling to avoid collision with float literals (`body.5` lexing weirdness).

**Prior art to survey on reopen**: Modelica (system composition), Mathematica Association, Julia struct + StaticArrays, SymPy Symbol vs Matrix.

**Non-overlap with #15.** #15 (flat-naming) stays the default for the "named scalar components in a single formula" case. #68 is for true encapsulation across multi-equation aggregates.

**Reopen trigger.** User picks up the design — needs research phase and a fresh design trio.

## 71. `diff` / `integral` don't distribute over vec/mat — IN-SCOPE

**Surfaced 2026-05-13** during cycle 3 of the Matrix-surface diagnostic-quality arc.

**Reproducer:**

```bash
# diff of matrix-valued integrand returns 0 (wrong; should distribute element-wise)
$ echo 'M = [[t, t^2], [1, t]]' > /tmp/repro.fw
$ fwiz --derive '/tmp/repro.fw(diff(M, t)=?, t)'
# → diff_t = 0
# expected: [[1, 2*t], [0, 1]]

# integral of matrix-valued integrand treats matrix as constant
$ fwiz --derive '/tmp/repro.fw(integral(M, t)=?, t)'
# → integral_t = [[t, t^2], [1, t]] * t
# expected: [[t^2/2, t^3/3], [t, t^2/2]]
```

Same shape applies to vec literals. `diff(v, t)` returns 0; `integral(v, t)` returns `v * t`.

**Cause:** `symbolic_diff` (`src/expr.h`) and `symbolic_integrate` (`src/expr.h`) don't recognize `vec`/`mat` FUNC_CALL nodes as containers to distribute over. The dispatch table sees `FUNC_CALL("mat", ...)` as opaque — falls to the constant-w.r.t.-x default branch (because the FUNC_CALL doesn't appear in the `BuiltinMeta` registry for differentiation/integration).

**Fix surface:** add a vec/mat branch at the top of `symbolic_diff` and `symbolic_integrate` that recurses over `args[i]` element-wise and rebuilds the container. Same pattern as `try_simplify_vec_mat_binop` uses for ADD/SUB/scalar-MUL. Roughly 10–20 LOC each.

**Why deferred to the completeness arc:** the matrix-surface diagnostic-quality arc (cycles 1–4) is about *substrate quality* — fuzz coverage, error messages, round-trip safety. Adding vec/mat distribution to `diff` / `integral` is a *capability extension* and belongs in the queued Linear-algebra completeness arc. Cycle 3's exit criterion (b) check is satisfied — no structural escalation needed for the diagnostic arc to proceed to cycle 4 (round-trip safety). #71 is the natural cycle-1 or cycle-2 opener for the completeness arc.

**Reopen trigger:** queued Linear-algebra completeness arc becomes active, OR user reports the gap with a concrete domain reproducer.

## 73. CLI-value `var=100kg` cannot resolve unit identifiers (cycle-1 follow-up) — DONE (2026-05-13, cycle 2)

**Surfaced 2026-05-13** during cycle 1 of the Units arc (parser desugar shipped).

**Reproducer:**

```bash
$ ./bin/fwiz 'stdlib/units/si-minimal.fw(mass=100kg, mass=?)'
Error: Invalid value '100kg' for variable 'mass'
```

The new `<number><identifier>` desugar correctly produces `MUL(Num(100), Var("kg"))` from `100kg`. But `parse_cli_query` evaluates each CLI-arg RHS expression IMMEDIATELY (line 4234: `if (auto val_opt = evaluate(*simplify(expr)))`), BEFORE the file is loaded. At that moment `kg` is an unbound variable, `evaluate` returns empty, the fallback fails, and the user sees an "Invalid value" error.

**In-file usage works correctly today** — e.g. `mass = 100kg` inside a `.fw` file with `kg = 1` resolves to `mass = 100`. Verified by cycle-1 test `(5) Bound unit eval: mass = 100kg with kg = 1`. The gap is solely on the CLI-arg evaluation path.

**Fix surfaces (3 candidates, picked by next cycle):**

(a) **Defer CLI-arg evaluation past file load**. `parse_cli_query` stores the raw expression string for any arg whose immediate `evaluate` returns empty; the main dispatch evaluates the deferred arg after `sys.load_file`. ~30 LOC. Cleanest semantics — CLI args see exactly the same scope as in-file equations. Risk: changes the error-emission timing of malformed args (was "at CLI parse", becomes "at solve time").

(b) **Synthetic-equation rewrite**. `mass=100kg` becomes a synthetic equation `mass = 100kg` injected into the system's equation list (same pattern `integral(...)`/`diff(...)` CLI args use today, at system.h:4213). ~5 LOC. Cleanest mechanism, no semantics drift, but the binding becomes an EQUATION not a binding — affects `--steps` trace shape and any code that walks `q.bindings`.

(c) **Pre-bind known unit identifiers from a tiny built-in catalog**. Parse `kg`, `m`, `s` etc. directly inside `parse_cli_query` as numeric 1. ~10 LOC. Quick fix, but special-cases units in a layer that should be unit-agnostic. Reject.

**Recommended on reopen:** (b). Smallest change, no semantics drift; the resolve-at-load primitive already handles this exact shape for diff/integral.

**Reopen trigger:** Units cycle 2 (or user reports the gap with a `(<var>=<num><unit>, <var>=?)` reproducer).

## 74. `100m^2` precedence quirk — parked behind the cycle-1 parse-time warning — PARKED

**Surfaced 2026-05-13** during cycle 1 of the Units arc.

**Behavior:** `parse("100m^2")` returns `(100 * m)^2`, not `100 * m^2`. Cause: the NUMBER+IDENT desugar wraps the entire pair into a `MUL` node BEFORE the `^` operator binds. So `^` applies to `MUL(100, m)`, not `m`.

**Mitigation shipped in cycle 1:** parse-time warning fires on stderr whenever a NUMBER+IDENT pair is immediately followed by `^`. Distinguishes the unit-precedence case (warning) from the function-call case (`100sin(x)^2` — no warning, since it's `100 * sin(x)^2` which IS what the user wants).

**Real fix (deferred):** make the desugar bind tighter than `^`. Concretely, in `parse_pow`, after parsing the base, if the base is `MUL(Num, X)` produced by the cycle-1 desugar AND a `^` follows, restructure as `MUL(Num, POW(X, exp))` before returning. ~10 LOC change in `parser.h` `parse_pow`. Risk: low — the structural pattern is unambiguous because regular `(100 * m)` would not match (we'd see an LPAREN token).

**Reopen trigger:** dim-analysis cycle lands (units cycle 3+) and the warning becomes user-facing noise, OR user reports the quirk surprise with a concrete `<num><unit>^<exp>` reproducer.

## 75. `parse_line` EOL `if`/`iff` detection bug — DONE 2026-05-13

**Surfaced 2026-05-13** during cycle 1 of the Units arc.

**Pre-fix bug:** the line-level `if`/`iff` keyword detector at `src/system.h:2522-2543` required `line[i+2] == ' '` AND `i + 2 < line.size()`. But `load_lines` runs `trim()` BEFORE `parse_line`, so a file line ending in `if` or `iff` (no trailing space) reached `parse_line` with the trailing whitespace already stripped. The keyword detector then failed to fire, the `if`/`iff` token remained in the equation portion, and the equation parser silently dropped the unconsumed trailing IDENT. Existing tests covered this happy-path drop (`test_condition_errors` `tce1.fw`).

**How it surfaced:** the Units cycle-1 NUMBER+IDENT desugar removed the silent-drop behavior — the trailing IDENT now gets absorbed into `MUL(Num, Var("if"))`, then `evaluate(Var("if"))` errors with "no value for 'if'".

**Fix:** accept end-of-line as a valid keyword terminator alongside space. Predicate becomes `(i + 2 == line.size() || line[i+2] == ' ')` for `if` and `(i + 3 == line.size() || line[i+3] == ' ')` for `iff`. `cond_part` defaults to empty string when at EOL. ~6 LOC. Regression-pinned in `test_unit_suffix()` (trailing `if` and trailing `iff`).

## Refactors

Readability-driven refactor candidates filed by the blind-spot critic. Each carries a **From** (source cycle + grader-tier failure), a **Proposed** (concrete change), and a **Reopen trigger**. The visionary audit tier-classifies these on the next cycle.

## #R1. Refactor: `try_u_sub_integrate` cse_replace naming hijack — DONE 2026-05-11

**Shipped 2026-05-11**: `cse_replace` renamed to `replace_subtree_by_name`; function parameter `helpers` → `replacements`; section header at expr.h:1240-1256 rewritten to reflect the generic contract (CSE + u-sub both consume); load-bearing comment at try_u_sub_integrate (expr.h:3155-3156) collapsed since the new name carries the meaning. 11 touch sites across 3 files. All tests green (3279/3279).

**From:** Cycle I-M2 blind-spot critic (function-scope). Haiku-B failed at T1 (wrong-on-detail) on `cse_replace(residual, {{u_name, g}})` — the helper's name signaled "common-subexpression extraction" but the call site used it as a generic structural-equality replace.

**Resolved by:** Pure rename per primary proposal. Tests stay (rename-followers). Name now reads truthful at every site.

**Reopen trigger:** Any future cycle's blind-spot critic re-flagging the structural-replace usage anywhere in the codebase.

## #R2. Refactor: `resolve_integral_calls` 4-arg control-flow restructure

**DONE-by-retest 2026-05-11** — Two consecutive floor-grader passes (cycle-cleanup-bundle close + F22 retest) constitute resolution per floor-vs-supplementary discipline. See #R2 status update below.

**From:** Cycle I-M2 blind-spot critic (function-scope). Haiku-B failed at T1 (wrong-on-detail) on the symbolic-vs-numeric dispatch in the 4-arg branch: the `if (val) { if (finite) return Num; /* fall through to numeric */ } else { return diff; }` shape has asymmetric early-return — the symbolic-bounds path returns directly, the NaN/inf path silently falls through. T3 only passes because the comment explicitly labels "fall through to numeric path" and "symbolic bounds — keep the closed form".

**Proposed:** Extract the 4-arg branch into a helper `resolve_definite_integral(antideriv, target, var, lo_expr, hi_expr) → ExprPtr` that hides the dispatch behind explicit named returns:
1. If `antideriv` and bounds collapse to a finite numeric → return `Num`.
2. If `antideriv` and bounds stay symbolic (free vars) → return symbolic difference.
3. Else if both bounds evaluate to finite → return adaptive_simpson result.
4. Else → return `nullptr` (caller surfaces unevaluated).

The fall-through-on-NaN/inf becomes an explicit early-fall-through `if (val && std::isfinite(val.value())) return Num;` followed by a shared numeric path. The asymmetry disappears because each terminal case is a named return; no comment-as-control-flow-marker needed.

**Pattern coverage:** Single-site. The `resolve_diff_calls` neighbour was rewritten cleanly in Cycle I-M1 with no analogous fall-through asymmetry — this is a 4-arg-specific shape.

**Reopen trigger:** When integration adds a 4th symbolic-fallback strategy (e.g., trigonometric substitution, partial fractions, or Risch) — at that point the dispatch chain becomes hard enough to warrant the refactor. (Original M3-IBP trigger fired; the refactor was deferred and remains scheduled for the next dispatch-strategy addition.)

## #R3. Refactor: `vec_mat_det` `en` vs `e` comment-code mismatch

**From:** Cycle I-M2 blind-spot critic (function-scope). Haiku-B at T3 scored wrong-on-detail: comment header reads textbook `a(ei - fh) - b(di - fg) + c(dh - eg)` using `e` for the (1,1) entry, but the code uses `en` (presumably renamed to avoid `e = Euler's number` collision). A reader trying to verify the cofactor expansion against the comment will find an apparent transcription bug. T3 fails *worse* than T1/T2 — the comment misleads.

**Proposed:** Either rename `en` → `e_` (trailing-underscore disambiguation) and update the comment correspondingly, OR keep `en` and rewrite the comment to use `en`: `// 3x3 cofactor: a(en*k - f*h) - b(d*k - f*g) + c(d*h - en*g)`. The latter is mechanical; the former is more idiomatic. Either eliminates the comment-code drift.

**Pattern coverage:** Single-site. Drift introduced because `e` is a reserved symbolic name (Euler's number) in this codebase, but no convention exists for "what to rename a `e`-collision local to". Watch for analogous collisions on `i` (imaginary unit, since 2026-05-09) and `pi`/`phi`.

**Reopen trigger:** Any third site renaming `e`/`i`/`pi`/`phi` locally without comment updates; OR a future cycle's grader re-flagging `vec_mat_det`. If the pattern recurs, extract a rule: "when renaming a builtin-constant collision, the comment must use the renamed identifier."

## #R4. Refactor: symbolic / numeric integration cross-references — DONE 2026-05-11

**Shipped 2026-05-11**: Cross-reference comments added at both ends. Symbolic-integration section header (expr.h:2785-2791) now points at `adaptive_simpson` as the numeric counterpart; `adaptive_simpson` intro comment (expr.h:3741-3744) now points back at `symbolic_integrate` and names `resolve_integral_calls` as the dispatch site. No code moves. The structural gap is closed by making the cross-file-region link explicit.

**From:** Cycle I-M2 blind-spot critic (file-scope, src/expr.h §Symbolic integration). file-explainer at T3 scored vague-but-correct on Components/Relationships/Pattern: the symbolic helpers cluster at lines 2616–2878 and the numeric counterpart lives at lines 3329–3391, separated by ~430 LOC of unrelated solver / Newton / bisection code. A reader of the symbolic section did not realise the numeric block is the same Future #16 milestone's other half.

**Resolved by:** Mutual cross-reference comments — closing the structural gap without moving code.

**Pattern coverage:** This is one site of a broader pattern in expr.h (3500+ LOC, multiple feature-areas interleaved). The file-organisation rule extracted from this finding (Code-Style.md §File-organisation rules) generalises the principle.

**Reopen trigger:** Any future cycle's file-scope critic re-flagging the symbolic-integration section; OR a third milestone shipping a non-contiguous surface in expr.h (`symbolic_diff` already has `diff()`-as-builtin in expr.h + post-load resolver in system.h, but those are cross-file by design — a same-file split is the trigger).

## #R5. Refactor: `try_ibp_integrate` render-order branch extraction — DONE 2026-05-11

**Shipped 2026-05-11**: `canonicalize_ibp_product(u, V, var) → ExprPtr` extracted (expr.h, beside `mul_through_div` — the symmetric pair). The 13-line render-order docstring moved into the helper; `try_ibp_integrate` now reads as `u_V = canonicalize_ibp_product(u, V, var); result = SUB(u_V, int_V_du); return simplify(result)`. Forward declaration added beside `try_u_sub_integrate` / `try_ibp_integrate`. Code-Style.md §Empirically-derived rules updated to cite all three sites (R1/R2/R5). All tests green (3279/3279); sanitize + analyze-fast clean.

**From:** Cycle I-M3 blind-spot critic (function-scope). Haiku-B failed at T1+T2 (wrong-on-detail) on the three-way render-order branch at function tail (`if V_is_div ... else if V_is_var && u_is_call ... else ...`). T3 only passes because the comment block enumerates each render-order case explicitly. **Third occurrence of the load-bearing-comment-papers-over-structural-complexity pattern in the integration arc** (cf. R1 `try_u_sub_integrate` cse_replace naming, R2 `resolve_integral_calls` 4-arg fall-through). Cycle-I-M2 rule "load-bearing comments must point at a structural cause" applies — the structural cause is mixed-responsibility: `try_ibp_integrate` does both IBP (math) AND canonical-render-order shaping (presentation).

**Proposed:** Extract the render-order branch into a named helper `canonicalize_ibp_product(u, V, var) → ExprPtr` that returns the `u*V` term with the correct operand order. The IBP function then reads as:
```cpp
const ExprPtr u_V        = canonicalize_ibp_product(u, V, var);
const ExprPtr result_raw = Expr::BinOpExpr(BinOp::SUB, u_V, int_V_du);
return simplify(result_raw);
```
The helper carries the three-case docstring; the IBP function carries only the IBP algorithm. The `mul_through_div` helper is already extracted — this is the symmetric move on the render-order side.

**Pattern coverage:** Three sites in the integration arc all exhibit the same shape (load-bearing comment at function tail compensating for branched complexity): `try_u_sub_integrate` (R1 — cse_replace naming), `resolve_integral_calls` 4-arg (R2 — control-flow asymmetry), `try_ibp_integrate` (R5 — render-order shaping). N≥3 met for rule extraction; the Cycle-I-M2 rule (Code-Style.md §Empirically-derived rules) is **validated** by this third occurrence, no new rule needed but its origin should reference all three sites.

**Reopen trigger:** when M-future adds a 4th integration strategy (e.g. partial-fractions, trig substitution), the render-order branch will grow further — refactor BEFORE adding the 4th case. Or when a future blind-spot cycle re-flags any of R1/R2/R5 under the same diagnosis.

## #R6. Refactor: `liate_priority` magic-rank constants → `enum class LiateRank`

**From:** Cycle I-M3 blind-spot critic (function-scope). Haiku-B failed at T1 (wrong-on-detail) on the rank values 5/4/3/2/1 — without the LIATE table comment, the values are pure magic. T2 (signature lifts the LIATE acronym) helped purpose but mechanics still failed. T3 passes only because the comment table explicitly enumerates which-rank-for-which-category.

**Proposed:** Replace inline magic ranks with an `enum class LiateRank` (declared near `liate_priority`):
```cpp
enum class LiateRank : int {
    None          = 0,  // does not qualify for LIATE
    Exponential   = 1,  // e^Var(var)
    Trigonometric = 2,  // sin / cos / tan
    Algebraic     = 3,  // x, x^n, c*x, c (var-free or numeric)
    InverseTrig   = 4,  // asin / acos / atan
    Logarithmic   = 5,  // log(...)
};
[[nodiscard]] inline int liate_priority(const Expr& e, const std::string& var) {
    if (e.type == ExprType::FUNC_CALL && e.args.size() == 1) {
        if (e.name == "log") return static_cast<int>(LiateRank::Logarithmic);
        if (e.name == "asin" || e.name == "acos" || e.name == "atan") return static_cast<int>(LiateRank::InverseTrig);
        ...
    }
    ...
}
```
The call site in `try_ibp_integrate` continues to use ints (rank comparison only); the FUNCTION BODY uses named constants. The comment-table burden disappears because each return statement IS the table.

**Pattern coverage:** Magic-rank/priority returns appear in **3+ sites** in the codebase:
1. `liate_priority` (this finding) — returns 0-5 by category.
2. `precedence` (expr.h:1081) — returns 0-5 for operator precedence (with the constant `5` falling out as the literal "atom" precedence).
3. `canonicity_score` (expr.h:1357) — returns `pair<int, int>` for derive ordering (leaves, depth) — the int values themselves are computed, not magic, but the *ordering* of returns from various branches is comment-driven.

N≥3 met (with `precedence` being the closest analogue). Rule promoted directly — see Code-Style.md addition.

**Reopen trigger:** any new heuristic-priority function added to the codebase that returns small magic-int ranks. Or a future cycle's grader re-flagging `liate_priority` under the same diagnosis.

**Status (Cycle Cleanup-Bundle, 2026-05-10):** **shipped.** Cleanup-bundle's A2 implemented this refactor exactly as proposed. Cycle Cleanup-Bundle blind-spot critic re-ran `liate_priority` at all three tiers — clean throughout (T1 mechanics now passes; was wrong-on-detail in M3). Visionary audit may close this entry on next firing.

## #R7. Refactor: `flatten_multiplicative` int-frac short-circuit position-priority — DONE 2026-05-10

**From:** Cycle Cleanup-Bundle blind-spot critic (function-scope, random codebase sample). Haiku-B at T1+T2 scored vague-but-correct on the `is_int_frac(e)` short-circuit at function head — without the comment, the position-priority over the DIV-decomposition branch reads as "test-then-push" optimization rather than what it is (a *correctness* guard: DIV decomposition would split `3/4` into "factor 3 exp 1, factor 4 exp -1", losing the structural-fraction shape that derive output relies on). T3 passes only because the comment "must short-circuit before DIV decomposition would split it apart" lifts the rationale. Soft load-bearing-comment instance — the structural cause is real (correctness invariant) but addressable.

**Proposed:** Extract the int-frac short-circuit as a clearly-named guard helper. Two options:
1. **Named guard helper** — extract to `bool try_emit_int_frac_factor(ExprPtr e, vector<pair<ExprPtr, double>>& factors)` returning true if pushed. The function head reads `if (try_emit_int_frac_factor(e, factors)) return;` — the name itself says "preserve structural fractions". One-line at the call site replaces the multi-line comment.
2. **Move int-frac test inside the DIV branch** — keep the short-circuit local to where the decomposition would happen: in the `BINOP::DIV with NUM denom` branch, test `is_int_frac(e)` before recursing into NUM coeff division. The short-circuit becomes a local invariant of the DIV branch rather than a head-of-function position-priority.

Option 1 is more idiomatic with the codebase's existing helper-extraction pattern (`mul_through_div`, `cse_extract`); option 2 is more locality-preserving. Either lifts the load-bearing comment.

**Pattern coverage:** Single-site for now. The "head-of-function correctness guard" shape is rare in expr.h (most simplifier helpers are case-driven without head guards). If a second simplifier helper acquires a similar head-of-function correctness guard (e.g. for matrix shapes, complex-coefficient fractions), promote to a rule.

**Reopen trigger:** any second simplifier function in expr.h growing a head-of-function correctness guard with comment-only justification; OR a future cycle's grader re-flagging `flatten_multiplicative` under the same diagnosis.

## #R8. Refactor: `class FormulaSystem` intra-class section dividers — DONE 2026-05-10

**From:** Cycle Cleanup-Bundle blind-spot critic (file-scope, src/system.h). file-explainer scored vague-but-correct on Components and Relationships: the file's top-level layout is clean (5 sections with `// ============ ============`-style dividers) but the central `class FormulaSystem` (~3400 LOC, lines 293–3700) has 5 conceptually separable sub-areas (Builtins / Loading-Parsing / Resolution-Solving / Derive / CLI orchestration helpers) interleaved without internal section delimiters. A reader cannot identify "where loading ends and solving begins" without reading method bodies.

This is a generalisation of the Cycle I-M2 file-organisation rule (cross-region milestone references in a >2000-line file): the same readability principle applies *intra-class* when a class itself exceeds ~1500 LOC.

**Proposed:** Add nested section dividers inside the class body, mirroring the existing top-level style but visually subordinated:

```cpp
class FormulaSystem {
public:
    std::vector<Equation> equations;
    // ... fields ...

    // ────────────── Subsection: Loading and parsing ──────────────
    void load_lines(...);
    void load_section(...);
    // ...

    // ────────────── Subsection: Builtins and rewrite rules ──────────────
    void load_builtins();
    void compute_rewrite_groups();
    // ...

    // ────────────── Subsection: Resolution / solving ──────────────
    [[nodiscard]] double resolve(...);
    [[nodiscard]] ValueSet resolve_all(...);
    // ...

    // ────────────── Subsection: Derive ──────────────
    [[nodiscard]] std::string derive(...);
    [[nodiscard]] std::vector<std::string> derive_all(...);
    // ...

    // ────────────── Subsection: CLI orchestration helpers ──────────────
    [[nodiscard]] std::map<std::string, ExprPtr> prepare_derive_bindings(...);
    [[nodiscard]] std::string format_derived(...);
    // ...

private:
    // ────────────── Subsection: private parsing helpers ──────────────
    // ...

    // ────────────── Subsection: private solver ──────────────
    [[nodiscard]] double solve_recursive(...);
    [[nodiscard]] bool try_resolve(...);
    // ...
};
```

No code moves; just structural delimiters. Box-drawing-character style (`──────`) visually subordinates them below the file-level `============` dividers, mirroring the existing visual hierarchy.

**Pattern coverage:** `class FormulaSystem` is the codebase's clearest single instance. `class Solver` / `class Lexer` / `class Parser` are all <500 LOC and don't trigger the threshold. No second class in the codebase exceeds the ~1500-LOC threshold.

**Reopen trigger:** any future cycle's file-scope critic re-flagging src/system.h Components or Relationships score; OR a third internal-section refactor at scale (e.g. if FormulaSystem grows past 4000 LOC and a sub-area emerges as a split candidate).


## #R9. Refactor: `try_resolve_numeric` `charge_budget()` call-site clarity — DONE 2026-05-10

**From:** Cycle blind-spot 2026-05-10 F6-F10 batch (function-scope, F8). Initial T1 mechanics score: H=match/specific, **G=wrong-on-detail/specific**. Gemma confabulated `charge_budget()` as a "Local variable" with role "Executes budget management logic" — pattern-matched past missing names per the verdict matrix's "wrong-on-detail/specific" diagnosis. The function calls `charge_budget()` at its head as a side-effect-only checkpoint; on comment-stripped read, no name signals "this is a budget guard, not a local with side effects."

**Diagnosis (T1-only):** comment-borne opacity. The `// Part C: insurance — per-candidate-evaluation` comment is what makes the call-site readable. Without it (T1), a reader misclassifies the call as a value-producing local.

**Proposed (soft, two options):**
- **Option A — rename** `charge_budget()` → `enforce_solve_budget()` or `assert_solve_budget_remaining()`. Reads as a guard at every call site without a comment crutch. Touches 4 sites in `system.h` (lines 2155, 3293, 3555, 3643) plus the function definition.
- **Option B — comment-at-call-site** every call site with `// guard:` or similar, leaving the function name unchanged.

Recommend Option A — naming carries the intent without comments.

**Pattern coverage:** 4 call sites in `system.h`, all using the same idiom with the same `// Part C: insurance` annotation. No other functions in the codebase have a similar side-effect-only checkpoint pattern that surfaced in the blind-spot scan.

**Reopen trigger:** post-rename, blind-spot re-runs `try_resolve_numeric` → mechanics scores match/specific on Gemma without confabulation. Alternatively: any future blind-spot cycle flags the same name pattern in another function.

## #R10. Refactor: `parse_line` `is_iff` flag DSL-token semantic misread — DONE 2026-05-10

**From:** Cycle blind-spot 2026-05-10 F11-F15 batch (function-scope, F11). Initial T1 mechanics score: H=match/specific, **G=wrong-on-detail/specific**. Gemma described `is_iff` as "A flag indicating whether the line structure suggests an 'if-then-if' conditional relationship." The DSL token `iff` is "if-and-only-if" (bidirectional condition); Gemma pattern-matched the doubled `f` to "if-then-if" — a confabulation that's substantively wrong about the language semantics.

**Diagnosis (T1-only):** domain-token-vs-C++-identifier collision. The DSL keyword `iff` is intrinsic and load-bearing (appears in `.fw` files everywhere; can't be renamed). But the C++ flag `is_iff` inherits the cryptic two-letter abbreviation, and a reader without DSL context misreads the semantic. The identifier carries no English-language signal of bidirectionality.

**Proposed:**
- **Rename** the C++ flag `is_iff` → `is_bidirectional` (or `iff_bidirectional`). The `.fw` keyword `iff` is preserved (DSL-level invariant); only the C++ identifier reflecting it changes. Touches `parse_line` (one local + one Equation field write) and the `Equation` struct's `bidirectional` field — which is already named correctly! So actually only `parse_line`'s local needs renaming.
- Verify the `Equation` struct field is already `bidirectional` (it is — see line 483: `equations.push_back({lhs, p.parse_expr(), std::move(cond), is_iff})` writes `is_iff` into a struct slot whose member is `bidirectional`). The asymmetry (local `is_iff` → field `bidirectional`) is itself the smell — pick one name.

Recommend renaming the local to `is_bidirectional` to match the destination field. Touches 4 lines in `parse_line` (declaration + 3 reads).

**Pattern coverage:** 1 site in `system.h` (`parse_line`). The `Equation::bidirectional` field is already correctly named. The only opacity is the local-flag-name asymmetry. Below the cross-cycle rule-extraction threshold; tracked as a single-function rename.

**Reopen trigger:** post-rename, blind-spot re-runs `parse_line` → Gemma mechanics scores match/specific on Gemma without "if-then-if" misreading. Alternatively: any future blind-spot cycle flags a similar DSL-token-vs-C++-identifier collision in another function.

## #R11. Refactor: `simplify_div` `all_cancel` and `lc`/`rc` semantic-name confabulation — DONE 2026-05-10

**From:** Cycle blind-spot 2026-05-10 F11-F15 batch (function-scope, F15). Initial T1 mechanics score: H=match/specific, **G=wrong-on-detail/specific**. Two confabulations on Gemma:

1. **`all_cancel`** described as "A flag indicating whether all terms in the additive expression `l` simplify to zero." The flag's actual role: tracks whether every additive term of `l` divides cleanly into `r` (i.e. the resulting per-term `simplify_div` did NOT return a residual `DIV` node). "Cancel" was misread as "evaluate-to-zero" rather than "factors-cancel-out."

2. **`lc` / `rc`** described as "leading coefficient" of `l` / `r`. The actual role: multiplicative-coefficient accumulator from `flatten_multiplicative()` — not "leading" in any polynomial sense, but the running scalar product carried out of the factorization. "lc" pattern-matched to "leading coefficient" because it's a familiar mathematical term.

A downstream agent reading Gemma's interpretation would conclude: "`all_cancel` is true when the sum collapses to zero" (wrong — would never branch on this code path correctly). And "`lc`/`rc` are the leading polynomial coefficients" (wrong — they're scalar carry-out from multiplicative flattening).

**Diagnosis (T1-only):** semantic-name overload. `all_cancel` carries two readings ("everything cancels-to-zero" vs "every term divides cleanly out"); `lc` is too short to disambiguate "leading coefficient" from "left coefficient" (and even "left coefficient" wouldn't be quite right — it's a scalar accumulator, not a polynomial coefficient).

**Proposed (rename-driven):**
- **`all_cancel` → `all_terms_divide_cleanly`** (or `every_term_simplified`). The new name explicitly says "every term, when divided by `r`, produced a non-DIV result" — no ambiguity with the cancel-to-zero reading. Touches 4 lines (declaration + 1 mutation + 1 read in `if` + scope of usage).
- **`lc` → `l_scalar` / `rc` → `r_scalar`** (or `l_coeff_acc` / `r_coeff_acc`). Avoids the "leading polynomial coefficient" pattern-match entirely. Touches 4 lines (2 declarations + 2 reads in `rebuild_multiplicative`).

**Pattern coverage:** 1 site in `expr.h::simplify_div`. The `all_cancel` naming pattern doesn't appear elsewhere in the codebase. `lc`/`rc` two-letter accumulator names appear in similar shape (`mc` in `solve_for_all` is the same pattern — multiplicative-carry scalar — and Gemma described `mc` correctly there as "running numeric coefficient accumulation"). The difference: `mc` is named near a comment-stripped expansion in F14 where the mathematical context shapes the read; `lc`/`rc` are at file scope of `simplify_div` where the surrounding pattern is multiplicative-flattening which could plausibly be polynomial work.

**Below the cross-cycle rule-extraction threshold** but worth tracking — alongside #R10 (`is_iff`), this is the second confabulation in this batch on identifier-semantics, and the third across 15 sampled (F8 `charge_budget`, F11 `is_iff`, F15 `all_cancel`/`lc`/`rc`). Pattern: cryptic abbreviations or short names that pattern-match to nearest-common-meaning trip Gemma's reading. Logged as soft pattern; rule extraction defers until a 4th instance appears in a future cycle.

**Reopen trigger:** post-rename, blind-spot re-runs `simplify_div` → Gemma mechanics scores match/specific without `all_cancel` zero-misread or `lc`/`rc` polynomial-misread. Alternatively: any future blind-spot cycle flags `all_cancel` semantics in another function (or similar two-letter coefficient names).

## #R12. Refactor: `nuanced-refactor-candidate` — missing query/engine API abstraction layer

**From:** Cycle blind-spot 2026-05-10 architecture-scope batch (first architecture-scope ANALYZE in the staged sweep). Floor (Haiku + Gemma) **passed** the comprehension gate on all four axes (purpose, module roles, dependency graph, pattern). However, **Gemma surfaced an unprompted concern** that Haiku did not flag: *"the manifest does not reveal if there is a higher-level abstraction layer (e.g., a specific API or interface) that cleanly separates the user-facing query logic from the underlying symbolic computation."*

Per the floor-vs-supplementary discipline (one floor grader flagging concerns the other missed = `nuanced-refactor-candidate`, not gate failure), this is **logged for tracking** rather than treated as a structural failure.

**Diagnosis:** the `FormulaSystem` class currently absorbs both **engine concerns** (rewrite rules, simplify, evaluate, candidate enumeration, numeric solving) AND **query/CLI concerns** (`parse_cli_query`, `CLIQuery`, derive_all output formatting, format_derived). A reader navigating from `main.cpp` cannot distinguish "what's the engine's public API" from "what's the CLI's view of the engine." The concerns share one class, with no interface boundary between them. (`CLIIntegralQuery` / `CLIDiffQuery` were deleted in #67, but the broader mixed-concern pattern remains.)

This is the architecture-scope sibling of **#R8** (FormulaSystem intra-class section dividers): #R8 makes the intra-class structure visible without moving code; this entry asks whether the engine's *public surface* should be split into a thin interface (`Engine` or `Solver`) consumed by a separate query/orchestration layer (`FormulaSystem` or `QueryEngine`).

**Proposed (low-priority, design-track):**
- Sketch an `Engine` (or `Solver`) class containing the substrate: rewrite rules, equations, builtins, `try_resolve`, `enumerate_candidates`, `simplify`, `evaluate`. Keep `FormulaSystem` as the orchestrator that owns CLI query types, derive output, format helpers, and the engine.
- Section dividers (#R8) is the cheap step toward this — making intra-class boundaries visible is prerequisite to extraction.
- The full extraction is a multi-cycle arc, not a single-cycle refactor.

**Pattern coverage:** 1 codebase site (`class FormulaSystem` — the only place the conflation exists). Below cross-cycle rule-extraction threshold; this is a single design call, not a generalisable rule.

**Reopen trigger:** Either of:
1. **#R8 is shipped** AND a follow-up architecture-scope critic still flags the engine/query mixed-concern reading. The dividers refactor is the cheap test — if intra-class structure visibility resolves the flag, no extraction needed.
2. A new contributor / agent asks "where does the engine end and the CLI begin?" — the question is itself the reopen condition.
3. The orchestrator considers an arc proposal (`/plan-campaign`) for a public-API surface — this entry becomes its starting point.

**Locked:** No — open for the visionary's tier classification. This is borderline wrapper-tool (since the user's vision emphasises "tiny fast core, infinite extendability via .fw rules" — a clean engine API would advance that), and borderline parked (the extraction is multi-cycle and currently has no concrete trigger).

## #R13. Refactor: `expr.h` 6-concern wall-of-code (file-scope split candidate, sharpens T4.1) — DONE 2026-05-10 (interim section dividers only; full split deferred per reopen-trigger)

**From:** Cycle blind-spot 2026-05-10 file-scope batch (first file-scope ANALYZE in the staged sweep). Floor verdict on **Pattern axis: vague-but-correct/vague** — gate fails per worst-of-two on this axis.

- **Haiku** (file-explainer with full body, comments stripped, 3861 LOC): scored Pattern as "Not a 'wall of code' but a large monolithic file appropriate for header-only design," confidence "mostly-clear", but Notes explicitly flag size (`exceeds comfortable single-pass reading`) and that "Sections on flattening, simplification loops, quadratic decomposition, differentiation/integration operators ... remain unread. Complete coherence verification would require reading the full file." Haiku tolerated the structure but explicitly admits incomplete reading.
- **Gemma** (same prompt, same body): scored Pattern as **"a 'wall of code' where concerns are heavily mashed together"** with 6 distinct mashed concerns enumerated unprompted: AST Definition, Numerical/Set Theory (`ValueSet`), Symbolic Algebra, Symbolic Calculus, Numerical Solvers, Linear Algebra. Verbatim: *"feels like a monolithic library rather than a cleanly separated set of modules."*

Worst-of-two on Pattern axis = Gemma's wall-of-code flag. Per the comprehension-gate principle, **the floor's flag is authoritative**; Haiku's tolerance does not override.

**Diagnosis:** **size + cohesion**, in that order. The 3861 LOC has reached the threshold where even file-explainer reads cannot complete in a single pass (Haiku's own admission). Within that size, Gemma read 6 distinct concerns — and the existing architecture rule already names ~3000 LOC as the split-by-responsibility threshold (`docs/Code-Style.md` §"a codebase whose two largest files together exceed ~80% of source LOC"). expr.h sits 28% over that threshold.

**Proposed (low-priority, design-track — sharpens existing T4.1):**

The existing T4.1 trigger names `numeric.h` extraction from `system.h` first, `query.h` second. **This entry adds: `expr.h` is also a multi-concern split candidate.** Concretely, Gemma's 6 enumerated concerns suggest 2-3 extractable surfaces:

1. **`valueset.h`** — extract `ValueSet` + `Interval` + `PeriodicFamily` + `Condition`/`CondClause`. ValueSet is a self-contained set-theoretic library used by rewrite-rule exhaustiveness checking and numeric-solver range narrowing. Imports nothing from expr.h that doesn't go through ExprPtr.
2. **`calculus.h`** — extract `symbolic_diff`, `symbolic_integrate`, `try_u_sub_integrate`, `try_ibp_integrate`, `BuiltinMeta` registry. Tightly coupled to Expr (uses ExprArena, builds AST), so this is a header-include split, not a deep dependency cleave. The benefit: a developer reading `expr.h` no longer encounters integration on the way to evaluation.
3. **(Optional) `linalg.h`** — extract `vec_mat_matmul`, `vec_mat_inv`, `vec_mat_transpose`, `vec_mat_det`. Smallest of the three concerns (~150 LOC).

Order of operations matters. The split must preserve dependency direction (per existing architecture rule): each new file imports from `expr.h` only, never the other way. ValueSet extraction is the cheapest test (clean boundary, no calculus/linalg coupling); calculus extraction is the heaviest (450+ LOC across the diff/integrate surfaces and growing per Future #16/#48/#49); linalg is borderline (small enough that file-scope cohesion may not require split).

**Pattern coverage:** 1 codebase site. The pattern of "single header file containing 6+ enumerated concerns" applies only to expr.h today. system.h's "monolithic-mixed-concerns" is already addressed by #R8 (cheap interim) + T4.1 (split path).

**Reopen trigger:** Either of:
1. **#R8 ships** (FormulaSystem section dividers) AND a follow-up file-scope critic re-runs expr.h → Pattern still scores "wall of code" by either floor grader. Section dividers in expr.h itself (analogous to #R8's intra-class style applied to free-function regions) is the cheaper interim test before any extraction.
2. **T4.1's `numeric.h` extraction lands**, validating that single-file extraction works in this codebase, AND a contributor/agent independently asks "where does ValueSet end and Expr arithmetic begin in expr.h?" — the question is itself the reopen condition.
3. **expr.h grows past ~4500 LOC** (further +600 from current 3861). At that size the size-only argument crosses the line independent of cohesion.

**Locked:** No — open for the visionary's tier classification. Borderline parked (multi-cycle, no immediate trigger; the cheaper interim — section dividers in expr.h — can fire first); borderline in-scope (the user's vision emphasises tiny fast core, and file-scope split-by-responsibility advances modularity without changing semantics).

## #R14. Refactor: `fit.h` cohesion-friction at sub-1000-LOC (new finding from file-scope; below split threshold) — DONE 2026-05-10

**From:** Cycle blind-spot 2026-05-10 file-scope batch. Floor verdict on **Pattern axis: vague-but-correct/vague** — borderline gate failure (worst-of-two = Gemma's wall-of-code flag).

- **Haiku** (file-explainer with full body, 999 LOC): scored Pattern as **"Pipeline-stage library"** with confidence "mostly-clear" — read it as well-organized for what it is. Notes flag "cognitive friction" of "6+ fitter implementations in parallel" and "interleaving of coefficient snapping + constant recognition + AST building throughout each fitter."
- **Gemma** (same prompt): scored Pattern as **"monolithic utility header — a single file containing a large collection of specialized mathematical tools"** + "the sheer volume of specialized functions ... makes it feel like a 'wall of code' rather than a cleanly separated module."

Disagreement on Pattern: Haiku tolerated as "Pipeline-stage library," Gemma flagged "wall of code." Worst-of-two = Gemma. Haiku's "cognitive friction" Notes provide soft alignment with Gemma's read — both surfaced cohesion concern, just at different severity.

**Important calibration:** fit.h is **999 LOC** — well below the architecture rule's ~3000 LOC split threshold. This is **NOT a split candidate** under existing rules. The flag is **cohesion-at-sub-split-size** — a different category from #R13 (which has both size AND cohesion).

**Diagnosis:** **structure**, not size. The file lacks intra-file section delimiters between its 4 logical regions:
1. **Numerical primitives** (Vandermonde, least-squares, polynomial eval).
2. **Per-model fitters** (polynomial, power, exp, log, sin, reciprocal — 6 self-contained functions with similar internal flow).
3. **Constant recognition** (`recognize_fraction`, `constant_form_to_expr`, `expr_recognize_constants`, `fmt_exact_double`).
4. **Composition orchestration** (`fit_base`, `compose_level`, `fit_all`, `sort_and_dedup`).

A file-explainer reading fit.h cold encounters these four concerns interleaved without visual separation. Haiku names the friction explicitly: "A reader must mentally track 6+ fitter implementations in parallel."

**Proposed (cheap, single-pass — analogous to #R8 but at file-scope rather than intra-class):**

Add 4 file-level section dividers at the boundaries of the 4 regions, in the same `// ============ ============`-style as system.h's existing top-level dividers:

```cpp
// ============================================================
// Section: Numerical primitives (Vandermonde, least squares, eval)
// ============================================================

// ============================================================
// Section: Per-model fitters (polynomial / power / exp / log / sin / reciprocal)
// ============================================================

// ============================================================
// Section: Constant recognition (fractions, sqrt/log constants, AST)
// ============================================================

// ============================================================
// Section: Composition and orchestration (fit_base, compose_level, fit_all)
// ============================================================
```

No code moves. Goal: a file-explainer reading fit.h cold can navigate the 4 regions without parsing function bodies. This mirrors the existing intra-class section-dividers rule (`docs/Code-Style.md` §"intra-class section dividers in classes exceeding ~1500 LOC") applied to a file with multi-region cohesion-friction.

**Pattern coverage:** 1 codebase site at this size threshold. expr.h would benefit from the same treatment but at a different scale (#R13 covers expr.h's case with deeper proposed action). system.h has top-level dividers already; #R8 covers the intra-class case there.

**Reopen trigger:** Either of:
1. **Dividers ship** AND a follow-up file-scope critic re-runs fit.h → Pattern still scores "wall of code" or worse on either floor grader. Then the diagnosis was wrong; reopen as a deeper structural concern.
2. **fit.h grows past ~1500 LOC** without dividers landing — escalate to split candidate (analogous to expr.h's trajectory).
3. **A second sub-1500-LOC file in the codebase** flags the same structure-without-size pattern in a future cycle. At N≥2 distinct sites, promote to a file-organisation rule (currently below the rule-extraction threshold; this is a single-file refactor item).

**Locked:** No — open for the visionary's tier classification. Cheap, low-risk, single-cycle work — likely in-scope.

## #R15. Refactor: `extract_positional_calls` — `nuanced-refactor-candidate` — dead `eq_lhs` parameter — DONE 2026-05-10

**From:** Cycle blind-spot 2026-05-10 F21-F25 function-scope batch. Floor (Haiku + Gemma) **passed** the comprehension gate on both axes (purpose, mechanics) cleanly. However, **Haiku surfaced a principled hedge that Gemma did not flag**: on the `eq_lhs` parameter, Haiku's mechanics response said *"role: cannot be determined — it is passed through to recursive calls but never read or modified in the visible body."*

Gemma, in contrast, confabulated a "context or comparison" role for the same parameter — confidence without evidence at the same param. Per the floor-vs-supplementary discipline (one floor grader flagging concerns the other missed = `nuanced-refactor-candidate`, not gate failure), this is **logged for tracking** rather than treated as a gate failure.

**Verification (grep, src/system.h:783-862):** The body of `extract_positional_calls` references `eq_lhs` only in five places, ALL pass-through recursive self-calls. The parameter is never compared, stored, written into `FormulaCall` fields, used in section lookup, or read in any branch. Sole caller (line 868) passes `eq.lhs_var` — but the parameter is genuinely dead within this function.

**Diagnosis:** **naming + dead-parameter.** Either:
1. The parameter was intended for future use (e.g. recording the LHS context in `FormulaCall` for error reporting) and the wiring was never completed — a soft TODO with no comment.
2. The parameter is a vestige of an earlier design where the LHS was used (e.g. to disambiguate call sites, to detect self-reference) and was removed without parameter cleanup.
3. The parameter is required by an interface contract that the visible body doesn't exercise — but no override or virtual signature exists.

Without comment or git-blame archaeology, the floor-grader read cannot disambiguate. Haiku's hedge is the strongest signal: *the parameter exists, it doesn't visibly do anything, the function still compiles and works correctly.*

**Proposed (low-priority, single-pass):** Choose one of:
1. **Remove the parameter.** Update the sole caller (system.h:868) to drop the `eq.lhs_var` argument. Cheapest action; preserves all current behaviour. Trivially verifiable.
2. **Wire it into `FormulaCall`.** Add `FormulaCall::source_lhs` (or similar) field, set `call.source_lhs = eq_lhs` at line 821-ish. Useful if future error messages or derive-trace need to know "which equation this call came from."
3. **Document the intent with a comment.** If the parameter is required for some future use, name the future use in a `// TODO:` or `// NOTE:` comment so the next reader (Haiku, future agent, human) doesn't hedge on it again.

**Recommendation:** **Option 1 (remove)**. The parameter has been dead for at least one major refactor cycle (the Integrals arc didn't touch this function). If a future use emerges, re-adding a parameter is a 2-line change. Keeping a dead parameter is a permanent legibility tax.

**Pattern coverage:** 1 codebase site. Below cross-cycle rule-extraction threshold; this is a single-function refactor. However, if future blind-spot batches surface 1-2 more dead-parameter hedges from Haiku, promote to a Code-Style.md rule under "Empirically-derived rules": *unused parameters in private/internal functions should be removed; if interface-required, name the requirement in a `// NOTE:` comment.*

**Reopen trigger:** Either of:
1. A future blind-spot floor-grader sweep re-flags `extract_positional_calls` for any reason — even unrelated — then revisit and decide.
2. A second blind-spot batch surfaces a Haiku "cannot be determined — passed through but never used" hedge on a different function. At N≥2 sites, promote to rule.
3. Implementer touches `extract_positional_calls` for any reason; apply the cheap fix as part of that change.

**Locked:** No — nuanced channel, low-priority. Open for visionary tier classification. Likely in-scope as cheap hygiene.

## #R16. Refactor: `expr.h` missing top-of-file SECTION table — `nuanced-refactor-candidate` (sharpens #R13)

**From:** Cycle targeted-sweep-3c-3k blind-spot critic (2026-06-21, file-scope ANALYZE). Floor (Haiku + Gemma) **PASSED** the comprehension gate on all four file-explainer axes (purpose / components / relationships / pattern) for src/expr.h (4383 LOC, 2909 comment-stripped). This is NOT a gate failure.

However, **both floor graders independently flagged the same structural gap unprompted**: the file has rich per-section `// ====` dividers but no top-of-file navigational table-of-contents. Haiku verbatim: *"No explicit section delimiters or table of contents, but function clustering is logical ... first-time readers may need to skip around."* Gemma tagged the structure "Monolithic Mixed-Concerns." Two-grader corroboration on an otherwise-clean read.

**Distinction from #R13 (load-bearing):** #R13 was a genuine *gate failure* — 2026-05-10 Gemma scored the Pattern axis `vague-but-correct/vague` (wall-of-code). This cycle's read is `match/specific` on every axis — the file is now read *accurately* by both graders. #R13's reopen-trigger #1 (re-run scores wall-of-code by either floor grader) therefore **did NOT fire**. This entry is the *interim sharpening* #R13 itself names: "Section dividers in expr.h itself ... is the cheaper interim test before any extraction." Per-section dividers shipped (the #R13 interim); the top-of-file map is the remaining cheap structural win.

**Diagnosis:** **structure** (navigation), not size or cohesion. The graders followed the file — the cost is *finding* a concern in 4383 lines, not *understanding* it once found. The per-section `// ====` boxes answer "what is this region?" once you are in it; a top-of-file table answers "where do I go?" before you start.

**Proposed (low-priority, single-pass, zero code moves):** Add a `FILE MAP` table-of-contents comment immediately after the includes/constants block (after expr.h:36), listing each major concern → approximate start line: `Checked<T>/Expr AST/ExprArena`, `ValueSet/Interval/PeriodicFamily`, `Condition/check_condition`, `simplify/flatten/rewrite`, `symbolic_diff/integrate/IBP`, `compute_dim`, numeric solvers, vec/mat builtins. This is the convention newly adopted in Code-Style.md §File-organisation rules ("files exceeding ~1500 LOC must carry a top-of-file SECTION table").

**Pattern coverage:** expr.h is the only >1500-LOC file flagged this cycle for the missing-table issue specifically. system.h (~3580 LOC) is a parallel candidate (it has intra-class dividers per the adopted intra-class rule but should be checked for a top-of-file table next file-scope sweep). The adopted Code-Style rule applies codebase-wide — any current or future file crossing ~1500 LOC.

**Reopen trigger:** Either of:
1. Implementer touches the head of expr.h (includes, top constants, or any §-divider region) for any reason — add the FILE MAP as part of that change (cheap, co-located).
2. A future file-scope sweep re-flags expr.h navigation, OR flags a second >1500-LOC file (e.g. system.h) for the same missing-table reason. At N≥2 sites the cheap interim should be applied across all flagged files in one pass.
3. #R13's split path activates (any of its three triggers) — at that point the FILE MAP becomes part of the split's per-file headers and #R16 folds into #R13.

**Locked:** No — nuanced channel, low-priority. Open for visionary tier classification. Cheap hygiene that directly implements a newly-adopted Code-Style rule.

## #R17. Refactor: `try_unroll_aggregate_with_calls` three-shape dispatch → named helpers — `nuanced-refactor-candidate`

**From:** FULL-SWEEP Batch 1 (2026-06-22, gen-6 aggregation surface, function-scope ANALYZE, T1). Floor (Haiku + Gemma) **PASSED** the comprehension gate worst-of-two (purpose vague-but-correct/vague via Gemma; mechanics match/specific via both). This is NOT a gate failure.

However, **both floor graders merged Shape A-named into Shape A** at T1 (comment-stripped). The function is a 96-line dispatcher over three structurally-distinct unroll shapes that the author has already delimited with `// Shape A` / `// Shape A-named` / `// Shape B` comments: (A) explicit-iterator body substitution, (A-named) body is a bare `Var("_fcN")` naming a parse-time-extracted formula call → clone-with-subst on its bindings + drop the template call, (B) broadcast arity-1 reducer over a formula-call output var → clone per range value. Haiku resolved more shapes than Gemma (it named the template-call-drop and the lockstep branch) but still folded A-named under A in the purpose read; Gemma stayed generic.

**Diagnosis:** **structure** (the three shapes are real, internally commented, but live inline in one function body) compounded by **size** (96 LOC). The per-shape comments are doing the work that named helpers should do — the author conceptualizes three units, but a T1-stripped reader sees one wall and merges them.

**Proposed (structural, low-priority):** Extract each comment-delimited block into a named private helper, turning the body into a three-line dispatcher:
```cpp
if (auto r = unroll_explicit_iterator(node))   return r;  // Shape A
if (auto r = unroll_named_call_iterator(node))  return r;  // Shape A-named
if (auto r = unroll_broadcast_over_call(node))  return r;  // Shape B
return node;
```
The helper names carry the shape taxonomy the comments currently carry; the dispatcher becomes self-documenting and each helper is independently comprehensible (all three are < 40 LOC). This is the size+structure diagnosis from the comprehension-gate ladder (extract by responsibility boundary, where the boundary is already comment-marked).

**Pattern coverage:** single site. The sibling `try_unroll_aggregate` (expr.h) is the simplify-path entry and is already small + clean (it passed match/specific on both graders) — it does NOT need the same treatment; only the system.h call-handling variant carries all three shapes.

**Reopen trigger:** Either of:
1. Implementer touches `try_unroll_aggregate_with_calls` for any reason (bug fix, cartesian-product extension for the 2+ ranges case) — extract the helpers as part of that change (the cartesian extension will add a fourth shape, at which point the inline form becomes untenable).
2. A future function-scope sweep re-flags this function with a purpose read that merges or misattributes the shapes by either floor grader.

**Locked:** No — nuanced channel, low-priority.

## #R18. Refactor: `fold_aggregate` mean branch → explicit `is_mean` flag — naming

**From:** FULL-SWEEP Batch 1 (2026-06-22, gen-6 aggregation surface, function-scope ANALYZE, T1). Floor **PASSED** worst-of-two (purpose match/vague worst-case via Gemma; mechanics match/vague worst-case via Haiku). Not a gate failure — both graders got the substance (it computes the arithmetic mean).

**The signal:** the function names four reducer flags — `is_sum`, `is_product`, `is_max`, `is_min` — and handles each in an explicitly-flagged branch. The fifth reducer, `mean`, is handled by the **unflagged final fall-through** (sum the terms, divide by count). Haiku, unable to see `is_aggregate_reducer`'s set (`sum/product/max/min/mean/count`) at T1, read the unlabeled fall-through as *"the final unreachable branch ... may represent a bug, dead code, or an aggregate type whose identifier is not documented."* That is the **too-smart failure mode** (confident wrong-on-detail hedge) triggered precisely by the missing name — the one branch without a flag is the one the reader cannot identify.

**Intervention (empirical):** adding an explicit `const bool is_mean = (name == "mean");` flag and an `if (is_mean) { ... }` guard flips Gemma's intervention-check read to confident *"arithmetic mean"* with the dead-code hedge gone (Gemma, /tmp/f3-iv-out, 2026-06-22). Naming was the entire bottleneck — intervention #1 (rename/explicit-flag) resolves it.

**Proposed (single-pass, near-zero risk):** Mirror the other four reducers — add `const bool is_mean = (name == "mean");` alongside the existing four flags, and wrap the final mean computation in `if (is_mean) { ... }` (with the trailing `return nullptr;` as the genuine no-match fall-through, which then correctly reads as "unknown reducer that passed `is_aggregate_reducer` but isn't handled" — a defensive guard, not a mystery). The five-flag symmetry makes the fold-policy table read as the exhaustive dispatch it is.

**Pattern coverage:** single site. This is the canonical "the unflagged branch is the unreadable branch" pattern — worth the Code-Style note below.

**Reopen trigger:** Implementer touches `fold_aggregate` for any reason, OR a sixth reducer is added (at which point the implicit-fall-through-is-mean assumption silently breaks and the new reducer would hit the mean path).

**Locked:** No — naming, low-priority but cheap and risk-free.

## 100. `DimMap` flat-vector optimization for small-dimension systems — PARKED

**Surfaced gen-5 cycle 3c (2026-06-06)** as a perf-auditor observation.

**Today**: `DimMap = std::map<std::string,int>` is an RB-tree allocating heap nodes even for the common 1-3 base-dimension case (e.g. `{mass:1}`, `{length:1,time:-1}`). Every `compute_dim` call on a compound expression allocates and deallocates small maps during the recursive fold.

**Proposed fix**: replace `DimMap` with a sorted `std::vector<std::pair<std::string,int>>`. Allocation-free for small N via SSO-analogue (or stack-local working storage); equality comparison stays O(N) scan; merge (add-exponents, sub-exponents, scale) is O(N log N) sort — same asymptotic as map ops for small N but with zero heap allocations on the common path.

**Reopen trigger**: user latency report on a system with >5 distinct base dimensions AND >1 rewrite rule consulting a `DIM_SECTION` predicate in a hot simplify loop, OR `compute_dim` appears at ≥3% of solve-phase wall time in a perf-auditor profile.

## #R2 status update — 2026-05-10 F21-F25 retest

**F22 `resolve_integral_calls` no longer flags at floor-grader read.** The 4-arg control-flow restructure (#R2) was filed at Cycle I-M2 and **deferred at meta-review consensus** (cycle-cleanup-bundle close, ~5 cycles back). At F21-F25 retest, BOTH floor graders (Haiku + Gemma) read the 2-arg-vs-4-arg dispatch, the three-tier antiderivative resolution, AND the FTC-with-Simpson-fallback path cleanly. Haiku names all 22 local vars including the FTC carriers; Gemma names 19 of 20 with no confabulations.

**Disposition:** the meta-review deferral was correct. R2's "structurally confusing" diagnosis was Opus-the-scorer's call from a position of full context; floor graders without that context find the function legible. Per floor-vs-supplementary discipline, this means the gate verdict is **pass**, and the deferred action is effectively a `nuanced-refactor-candidate` (same channel as #R12, #R15).

**Recommended action on #R2:** Either:
- **Downgrade R2 to `nuanced-refactor-candidate` channel** (mirror #R12's framing) with reopen trigger "next blind-spot floor-grader failure on `resolve_integral_calls`."
- **Close R2 as DONE-by-retest** with a note that 2x floor-grader passes constitute resolution. Defer to visionary's tier classification.

No new R2-tracked refactor work is recommended. Two consecutive floor-grader passes (cycle-cleanup-bundle close + F22 retest) is strong evidence that the function reads cleanly without intervention.
