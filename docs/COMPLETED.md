# Completed: Shipped Features and Cleanup Cycles

Archive of `Future.md` entries that have shipped. Numbering matches the original `Future.md` numbering so existing cross-references (commits, agent profiles, research artifacts) stay valid. New work and remaining enhancements live in `Future.md`.

## Units arc — IN PROGRESS (cycle 1 shipped 2026-05-13, ROADMAP gen-2)

N-cycle campaign to add unit-of-measure support. Goal: `100kg` parses and binds; stdlib provides unit catalogs; eventually: dimensional analysis rejection.

**Cycle 1** (2026-05-13): Parser desugar `<number><identifier>` → `MUL(Num, Var)`.
- `src/lexer.h` `read_number` extended: scientific notation (`[eE][+-]?[0-9]+` tail) consumed — `1.5e3` now `Num(1500)`, not `Num(1.5)` + silent-drop. ~12 LOC.
- `src/parser.h` `primary()`: NUMBER-IDENT desugar emits `MUL(Num, Var)` for `100kg`, `MUL(Num, FUNC_CALL)` for `100sin(x)`. `^`-suffix emits a parse-time warning (`100m^2` quirk). ~40 LOC.
- `src/system.h` `parse_line`: EOL `if`/`iff` keyword detection bug fixed (Future #75 DONE). ~+6/-4 LOC.
- `stdlib/units/si-minimal.fw`: 7 SI base units (`m, kg, s, A, K, mol, cd`) all bound to 1 (scalar dimensionless placeholder). 10 lines.
- Tests: 3395 → 3430 (+35). All gates green (test + sanitize + analyze-fast).
- Future #73 filed: CLI-arg `var=100kg` does not yet resolve `kg` (deferred to cycle 2).
- Future #74 filed (PARKED): `100m^2` precedence quirk real fix.
- Future #75 filed: `parse_line` EOL keyword bug (DONE in this cycle).

**Cycle 2** (2026-05-13): CLI-arg unit-suffix + stdlib expansion.
- `parse_cli_query` value-side path now defers unresolved-Var expressions to post-load resolution via the `synthetic_equations` channel (Future #73 DONE). `mass=100kg` end-to-end works. `allow_symbolic` check runs before the synthetic-equation branch so `--derive`/`--fit` retain their existing contract.
- `stdlib/units/si-minimal.fw` grown 16→42 lines: SI prefixes (km/mm/um/nm/Mm/Gm for length; g/mg for mass; ms/us/ns/min/hr/day for time) and 5 derived units (N/J/W/Pa/Hz), all defined in terms of SI base.
- Tests: 3443 → 3493 (+50). All gates green.

**Cycle 3** (2026-05-13): Physics formula catalog — first end-to-end demonstration that unit-bearing CLI inputs flow through real formulas.
- `stdlib/physics/mechanics.fw` (~48 lines): Newton's 2nd law, weight, kinetic energy, momentum, work, power, pressure, frequency/period reciprocity. Inlines SI-base bindings it depends on (`m, kg, s, N, Pa, J, W`) so no separate units load is required.
- Variable-naming hygiene: verbose names (`mass`, `displacement`, `area`, ...) so the formulas never shadow the SI base-unit symbols (`m`, `s`, `A`, `kg`) reserved by the units catalog. File header documents the rule.
- `test_physics_mechanics` (8 assertions, ~80 LOC): 6 in-process `FormulaSystem` cases (Newton, weight, KE, momentum, pressure, bidirectional mass-solve) + 2 popen end-to-end CLI cases (force, kinetic-energy via unit-suffix args).
- `docs/Language.md` §14.4: stdlib reference entry for the new catalog.
- Tests: 3495 → 3505 (+10). All gates green.

**Remaining cycles (queued):** `100m^2` fix (#74); dim-analysis stdlib; dimensional analysis rejection (#7b).

## Matrix-surface diagnostic-quality arc — ✅ COMPLETE (2026-05-13, ROADMAP gen-1)

4-cycle quality-oracle arc on the #14 vec/mat surface (shipped 2026-05-10). Goal: audit and harden before extending. Each cycle shipped either concrete diagnostic improvements or trigger-tied Future.md filings for the queued completeness arc.

**Cycle 1** (2026-05-13 `2d63b77`): Matrix-flavored fuzz baseline.
- 6 new corpus seeds; clean 60s fuzz baseline (12,519 execs / 0 crashes).
- Future #69 filed: cross-file resolution cycle SIGSEGV (surfaced via smoke-testing).

**Cycle 2** (2026-05-13 `65101ec`): User-facing error messages + #69 fix.
- Ragged-matrix-literal parse-time validation via `RaggedMatrixError` sibling exception.
- Future #69 cross-file SIGSEGV closed: `CrossFileResolutionCycleError` sibling exception + thread-local `currently_loading` guard.
- `load_lines` catch narrowed to `runtime_error`; sibling-exception convention codified.

**Cycle 3** (2026-05-13 `79661aa`): `--derive` determination + #71 filing.
- Determination: matrix `--derive` works today via `solved_symbolic_`; `#10a` structural escalation NOT needed.
- 8 regression tests pinning the working `--derive` corners.
- Future #71 filed: `diff`/`integral` don't distribute over vec/mat (routed to queued completeness arc).

**Cycle 4** (2026-05-13 — this cycle): Round-trip safety + arc-exit.
- 4 round-trip regression tests + 1 known-limitation regression test (5 cases / 10 ASSERTs in `test_vec_mat_roundtrip`).
- Cumulative fuzz re-run on the matrix-flavored corpus: 9028 execs / 0 crashes / no regressions.
- `make valgrind` re-run on the post-cycle-2 thread_local cycle guard: **3385/3385 tests, 0 errors, 0 leaks**. Memcheck confirms the `static thread_local std::set<std::string>` addition in `load_sub_system` is correctly initialised once per thread and cleaned up on scope exit.
- Round-trip corners pinned: concrete matmul → `[[19, 22], [43, 50]]`; symbolic transpose with free vars → `[[a, c], [b, d]]`; concrete fractional inverse → `[[(-2), 1], [3 / 2, (-1) / 2]]` (parens and structural-fraction format preserved); `undefined` propagation across re-parse.

**Findings handed off to queued completeness arc**:
- Future #69 DONE in cycle 2 (substrate fix).
- Future #71 IN-SCOPE: `diff`/`integral` over vec/mat distribution.
- Future #14 deferred items (N×N det/inv, eigenvalues, quaternions, complex-element matrices) all remain queued.
- Known limitation: matrix-valued results require `--derive`. Solve-mode (without `--derive`) cannot bind a matrix to a variable because `evaluate()` returns empty on vec/mat by design. Pinned by RT5 of `test_vec_mat_roundtrip`. Documented in CLAUDE.md / Language.md §15.4; not a bug.

**Arc-exit-criterion (b)** never triggered. All 4 cycles closed clean. Tests: 3346 → 3395 (+49 across the arc). No regressions; net build-system addition (`make valgrind` baseline established earlier this week).

Net production source LOC across the arc: ~60-70 (parser.h ragged check + system.h cycle guard + `RaggedMatrixError` + `CrossFileResolutionCycleError` + the `load_lines` narrowing). Tests +~200 LOC. Documentation updates across CLAUDE.md, Developer.md, Language.md, COMPLETED.md, Future.md, Known-Issues.md.

## 0-3: Conditions, Ranges, Recursion, Numeric Solving — ✅ DONE

All implemented. `if`/`iff` conditions, ValueSet ranges, recursive formula calls with depth guard, numeric solving with adaptive grid scan + Newton/bisection. See Developer.md for details.

## 4. Numeric Solving — ✅ DONE

Adaptive grid scan + Newton/bisection refinement. Strategy 6 in `enumerate_candidates`. `try_resolve_numeric` handles equation-based root-finding and system-probe fallback (for recursive formulas). Re-entrance guard prevents stack overflow on recursive calls. Memoization via `numeric_memo_`. Results classified as exact (`=`) or approximate (`~`) via forward verification. Newton uses symbolic derivatives automatically when `symbolic_diff_simplified` succeeds (quadratic convergence, 2 evals/iteration); falls back to central finite-differences when it returns `nullptr`. See Developer.md.

Remaining enhancements tracked in Future.md #4-residual.

## 5. Batch/Table Mode — ✅ DONE (2026-05-11)

CLI-only `--table` flag emits a TSV table evaluating a query across one or more range-valued inputs. Single-milestone cycle (~310 LOC). Driver lives in `main.cpp` between nested-call injection and the `--derive` dispatch; range parsing lives in `system.h` next to `parse_cli_query`.

**Syntax:**
```bash
fwiz --table triangle(C=?, a=[1..10], b=4, c=5)
fwiz --table --zip f(z=?, a=[1..3], b=[10..12])
fwiz --table --output out.tsv f(z=?, a=[0..1 @ 0.1])
```

Range grammar (parsed by free function `parse_range` before any Lexer call): `[start..stop]` (integer step 1, both endpoints inclusive); `[start..stop @ step]` (custom step, count-based generation — `start + i*step` avoids float drift); `[range1, range2, ...]` (compound — concatenated values). Bounds may be expressions (`[0..2*pi @ pi/4]`) — reuses the existing `Parser + evaluate` idiom. Descending ranges require explicit negative step (`[10..1 @ -1]`); empty ranges, zero step, malformed inputs all throw.

Behaviour:
- **Cartesian product** (default) or **zip** (`--zip`).
- Header row: range vars (CLI order) then query aliases (CLI order).
- Per-row: copy `query.bindings`, overlay range values, call `resolve_all`, format with `fmt_solve_result`, emit tab-separated. Unsolvable → `?`.
- Soft cap at 1M cartesian rows (stderr warning, continues).
- Zip mismatched lengths: truncate to min, stderr warning.
- Mutual exclusion: `--table` is incompatible with `--derive/--verify/--fit/--explore` (inverted enumeration — one guard for all non-row-shaped output modes). `--zip` without `--table` errors.
- `--output FILE` redirects TSV to file. `--approximate`/`--exact`/`--precision` follow global flags per-cell.

Comma-splitter in `parse_cli_query` extended to track `[]` alongside `()` so `a=[1..5, 6..10]` parses as one arg (mirror of the integral inner scanner). `CLIQuery::range_bindings` is `vector<pair<string, vector<double>>>` — CLI-order preserving.

Four follow-ups parked in Future.md: `--table-max-rows N` (5a), in-file declarative range (5b), `--all-results` (5c), `--nan` sentinel (5d). Each has a concrete reopen trigger.

Tests: 3279 → 3330 (+51). LOC: ~280 (under the 310 budget). Gates: test + sanitize + cppcheck clean.

## 6. Symbolic Differentiation — ✅ DONE (2026-04-26)

`symbolic_diff(const Expr&, const std::string& var) → ExprPtr` (expr.h). Two-level dispatch: per-AST-class switch for ADD/SUB/MUL/DIV/POW/NEG, inline if-chain for FUNC_CALL covering 9 builtins (sin, cos, tan, asin, acos, atan, log, sqrt, abs). `symbolic_diff_simplified` wrapper calls `simplify()` on the result. Returns `nullptr` for unknown FUNC_CALLs — used as a "leave-symbolic" signal by the post-load pass.

Post-load pass `resolve_diff_in_equations` (system.h): rewrites `diff(named_var, x)` and `diff(formula_call_placeholder, x)` nodes after all equations and rewrite rules are loaded. Handles three target shapes: Var-as-equation RHS, Var-as-formula-call output, and literal expression.

Two API surfaces:
1. In-file builtin: `sensitivity = diff(force, mass)` — parser-level recognition replaces the call with the differentiated tree at load time.
2. CLI query: `fwiz kinematic.fw 'diff(distance, time)=?'` — synthesised as `<alias> = diff(...)` by `parse_cli_query` and loaded via `sys.load_string`; standard post-load pass resolves it. Falls back to printing the symbolic expression when free variables remain. (Unified with the `.fw` path in Future #67 — `CLIDiffQuery` struct and Pass 1.5 dispatcher deleted.)

`solved_symbolic_` carries derivative results (confirmed via `test_symbolic_diff_provenance`), validating the pre-positioned carrier.

Three new rewrite rules in `BUILTIN_REWRITE_RULES`: `x^a / x^b = x^(a-b) iff x != 0` (pulled forward from Future #5 scope), `abs(x) / x = sign(x) iff x != 0`, `abs(x) / x = undefined iff x = 0`. `sign(x)` registered as a new builtin with `sign_eval` numeric evaluator.

Tests: 2237 → 2272 (+35).

## 10. Fraction Representation — ✅ DONE

Structural fractions: `DIV(Num(a), Num(b))` preserved when result is non-integer. GCD normalization, sign normalization, rational arithmetic (add/sub/mul/div/pow). Constant recognition in derive output (`log(2)`, `log(3)`, `sqrt(N)`, `pi`, `e`). Rational display in solve output via `fmt_solve_result` (main.cpp) when the result is exact. No Expr struct changes — sizeof(Expr) unchanged.

Extended constant table shipped 2026-04-19 (ccacc8e / 43cbc0d) via `fmt_exact_double` alias threading and `build_alias_table()`: user-defined `.fw` constants (e.g. `deg = pi/180`) are now recognized in solve + derive output.

Remaining enhancements tracked in Future.md #10-residual.

## 11. Curve Fitting — ✅ DONE

Implemented as `--fit [N]` flag. Templates: polynomial, power law, exponential (including Gaussian/quadratic exponent), logarithmic, sinusoidal, reciprocal. Recursive composition (depth N, default 5) discovers nested forms like `sin(sin(x))`, `e^(x*log(x))` = `x^x`. Product inners (`x*log(x)`, `x*sin(x)`) enable complex decompositions. Constant recognition (pi, e, phi, sqrt(2), sqrt(3)) in fitted coefficients.

Remaining enhancements tracked in Future.md #11-residual.

## 19. `Checked<double>` — zero-overhead optional alternative — ✅ DONE

Implemented as `Checked<T>` (not `checked_value`). NaN-sentinel optional replacing `std::optional<double>` for `evaluate()` return type. `sizeof(Checked<double>) == sizeof(double)`; returns in one FP register; `operator*` deliberately absent — unwrap via `.value()`. Full three-file migration: `expr.h` (type definition + evaluate signature), `system.h` (~20 call sites + hot probe lambda), `fit.h` (2 probe lambdas). Commits 7095f95 (type + evaluate), 620c3d9 (hot probe), 6608bdd (fit.h). 1829/1829 tests passing post-migration.

## 22. Post-derive simplification and deduplication — ✅ DONE (2026-04-19, ccacc8e / 43cbc0d / 319c9e3)

Semantic fingerprint dedup shipped. `fingerprint_expr` + `canonicity_score` primitives in `expr.h`; streaming `winners` map in `derive_all`; `build_alias_table()` + `source_label_` on `FormulaSystem`; `RECOGNIZE_FRACTION_MAX_DEN` raised to 360 with `extra_constants` threading. Triangle reproducer: 294 → 159 output lines (46% reduction). Results now emitted ascending by `canonicity_score` — simplest formula first; sentinel-bucket forms last. `--derive N` caps at N results. Defect A fixed: `free_vars` in fingerprinting now uses alias values, not keys, so CLI alias queries fingerprint correctly. 1944/1944 tests pass.

The original problem was 294 distinct output lines from `fwiz --derive "examples/triangle(A=?, a=4, B=20, c, b)"`, many semantically equivalent but rendered differently. Two improvements were queued together:
- **Improvement A — division-over-addition distribution**: `(a + b) / k = a/k + b/k iff k != 0`. Allows the simplifier's like-term combiner to peek inside `DIV(ADD(...), Num)`.
- **Improvement B — semantic fingerprint dedup**: evaluate each candidate at random points in free-variable space; group by fingerprint match within `EPSILON_REL`; surface one canonical form per group.

Both shipped. Improvement A landed first; Improvement B (Schwartz–Zippel numeric fingerprinting) was the actual mechanism that collapsed 294 → 159.

## 24. Widen `is_one`/`is_neg_one`/`is_neg` pointer overloads to `const Expr*` — ✅ DONE 2026-04-19 (0708bf5)

Widened in the M6+M7+F24 micro-cycle. No caller cascade surfaced (unlike M3/M10).

## 25. M6/M7 deferred: `variableScope` and shadow renames — ✅ DONE 2026-04-19 (0708bf5)

All 24 warnings cleared (8 `variableScope` + 16 shadow renames). No behavior changes; test output byte-identical.

## 26. `system.h:1890` redundantAssignment bug-smell — ✅ DONE 2026-04-19 (6caf0a4)

Debugger round confirmed truly-dead code (inner branch fires 2× in test suite but the else-if below independently re-finds the builtin). Four lines deleted; semantically equivalent. Findings preserved in `.fwiz-workflow/debug-findings-system-1890.md`.

## 34. `x / (1/y) = x*y iff y != 0` rewrite rule — ✅ DONE (2026-04-24)

Shipped as a builtin rewrite rule alongside the related `k * x / (k * y) = x / y iff k != 0` cancellation rule. Now also handles **numeric** denominators — `a / (1/20) → a * 20` (canonical: `20 * a`) — eliminating all 57 instances of `/ (1 / 20)` in the triangle reproducer's derive output.

The original reopen-trigger ("`/(1/SYMBOL)` substring where SYMBOL is a non-numeric identifier") is now ACTIVE as a regression guard.

Open residual (composite-denominator patterns like `x / ((1/k) * Y)`) tracked separately as Future.md #36.

## 38. `x^(-n)` rendering as `1/x^n` for any integer n — ✅ DONE-BY-SIDE-EFFECT (2026-04-24)

Originally tracked as a residual: `simplify_pow`'s standalone case (`expr.h:1759-1765`) handled `x^(-n)` outside any MUL chain, but the moment the POW-with-negative-exponent was wrapped in a MUL chain, the `rebuild_multiplicative` factor-emit loop would re-emit `POW(base, Num(-n))` unconditionally, undoing the cleanup.

**Resolution:** `rebuild_multiplicative` (`src/expr.h`, ~lines 1296-1330) was rewritten to split factors by exponent sign: positive exponents → numerator product; negative exponents (with sign flipped) → denominator product; emit `DIV(num, denom)` when any negative-exp factors exist. Walker assertion (`tests.cpp` M3-6 block) pins the invariant: no `^(-` substring in derive output for the triangle reproducer. Triangle measurement: 66 `^(-` substrings → 0; 159 lines → 158; 42024 chars → 40983.

## T1 Cleanup Cycle — ✅ DONE (2026-04-28, net −279 LOC)

Three structural cleanup milestones shipped. Zero behavior changes except the `||` correctness fix in M3.

- **T1.1 (M3):** Condition parsing unified on the existing `Condition` AST. `RewriteRule::condition` changed from `std::string` to `std::optional<Condition>`. `condition_violated` and `substitute_condition` deleted (−114 LOC string-substitution reimplementations of `check_condition` and expression substitution). `Condition`/`CondClause`/`CondOp`/`CondLogic` structs and `check_condition` moved from `system.h` into `expr.h` (after `ValueSet`, before `RewriteRule`), mirroring the existing ValueSet split. `parse_condition` stays in `system.h` (uses Lexer/Parser). `condition_to_string(const Condition&, bindings)` helper added in `expr.h` for `--steps`/`--calc` assumption strings. Silent `||` bug in rewrite-rule conditions closed (old string scanner split only on `&&`; AST path correctly evaluates `||`). Test 11 in `test_rewrite_rules` is the witness case.
- **T1.2 (M2):** Five near-identical post-order tree walkers (`substitute`, `cse_replace`, `substitute_builtin_constants`, `expr_recognize_constants`, `resolve_diff_calls`) replaced by two narrow templates in `expr.h`: `tree_map<Fn>` (full-tree post-order, used by `cse_replace` and `resolve_diff_calls`) and `tree_map_leaf<Fn>` (leaf-only, used by the three VAR/NUM-matching walkers). Pointer-equality short-circuit is now universal. `expand_for_var` not migrated — its MUL-distribution logic inspects post-recurse sibling shapes, not a pure leaf transform. 4 new ASSERTs in `test_tree_map_primitives` pin the pointer-identity invariant.
- **T1.3 (M1):** `FWIZ_TRACE_SOLVER` instrumentation deleted from `system.h` (−185 LOC). Removed: `fwiz_trace_solver()`, `MAX_DIAGNOSTIC_SOLVE_DEPTH`, `diag_keyset_str`, `diag_set_str`, `diag_expr_preview`, `dump_dead_ends`, and ~45 `if (fwiz_trace_solver())` guard sites in `derive_recursive`, `solve_all`, `solve_recursive`, `try_resolve`, `try_resolve_numeric`.

Reopen triggers from this cycle: T2.1, T2.2, T2.3, T3.1 all landed in T2+T3 cycle (see below). T3.2, T3.3, T4.1, T4.2, T4.5, plus three guard triggers (`tree_map` creep, `Condition`-in-expr.h spread, M3 description-string regression) are tracked in Future.md.

## T2+T3 Cleanup Cycle — ✅ DONE (2026-04-29)

Correctness, migration, and style items from the code-quality audit. Zero user-visible behavior changes except the rewrite-rule parse-failure warning (Issue 1).

- **M1 (correctness):** T2.1 (`approximate_mode` propagation to sub-systems at construction), T2.2 (`next_call_id_` per-instance counter replacing `static int call_counter`), T2.3 (`expr_recognize_constants` signature widened to `const Expr*`), Issue 1 (parse-failed rewrite rules dropped at load with stderr warning; dead `all_have_conditions` variable removed). Tests: 2299 → 2307 (+8).
- **M2 (.fw rule migrations):** T3.4 PARTIAL — branch 2 of `simplify_div` negation chain deleted as structurally subsumed; branches 1, 3, 4 retained with named-test pinning comments. T3.5 and T3.6 RETREATED — `simplify_div` constant reassociation and `x^(-n)` rendering cannot migrate; see Future.md #53-55 for the typed-binding-predicates blocker and rationale comments in source.
- **M3 (style + perf):** T3.1 (`recognize_constant` static lookup tables — zero per-call allocation), T3.7 (`auto S = simplify` alias deleted), T3.9 (no-op `equations.size() >= 2` guard in Strategy 7 removed), T3.10 (`resolve_memoized` cache-key `reserve()` added), CondOp::COUNT_ convention compliance (`assert(false)` in switch). T3.2, T3.3 deferred (profiling-gated). T3.8 deferred to T4.1 file-split atomic diff.

Reopen triggers added: Future.md #53 (typed-binding predicates), #54 (T3.5 rationale), #55 (T3.6 rationale), #56 (Issue 1 option-b escalation), #57 (recognize_constant std::map → sorted std::array). T4.1 payload updated to include T3.8 rename.

## 69. Cross-file resolution cycle SIGSEGV — DONE (2026-05-13)

When a `.fw` file's body recursively called a function whose name matched its filename (e.g. `matmul.fw` containing `matmul(A, B)`), `load_sub_system` created a fresh sub-system whose own empty `sub_systems` cache missed the recursion check — infinite recursion → stack overflow → SIGSEGV. Surfaced in cycle 1 of the matrix-arc via corpus smoke-testing; fixed in cycle 2.

**Fix**: new `CrossFileResolutionCycleError` sibling exception (not derived from `std::runtime_error` so the many `catch (runtime_error)` sites in the solver don't swallow it). Thread-local `currently_loading` set keyed on `cache_key`, with RAII `LoadGuard` that erases on both success and exception paths. Cycle detection generalizes — not matmul-specific; same shape works for any builtin/user function name that shadows a `.fw` filename.

Net production: +33/-1 LOC in `system.h` + 2 regression tests. Tests: 3346 → 3372 (+26 including the ragged-matrix tests from Item A).

## 67. CLI / `.fw` dispatch-path unification for `integral`/`diff` queries — DONE (2026-05-12)

Two parallel dispatch paths for the same semantic operation collapsed into one. `parse_cli_query` now synthesises `<alias> = diff/integral(...)` equations into a new `CLIQuery::synthetic_equations` string (+ a `synthetic_aliases` set for the symbolic-fallback render path). main.cpp loads it via a single `sys.load_string` after the file/inline source; the standard query loop handles the alias as a regular query. `CLIDiffQuery` / `CLIIntegralQuery` structs deleted; Pass 1.5 / Pass 1.6 dispatcher blocks (~50 LOC each) in main.cpp deleted.

Net production LOC: -65 (-83 main.cpp, +18 system.h). Tests: 3340 → 3343 (+3 net).

**Two visible bugs closed**:
- `--table` + `integral(...) = ?A` now composes naturally — integral queries flow through `q.queries[]` which the table driver already iterates.
- CLI binding RHS form `F = integral(x^2, x)` now works (combined with `F=?` → `F = x^3 / 3`). Previously errored "Invalid value" because the value-side path tried `evaluate()` on the integral FUNC_CALL, which returns empty by design.
