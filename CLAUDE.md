# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

Bidirectional formula solver. Write equations once in `.fw` files, solve for any variable forwards or backwards. Supports conditions, recursion, cross-file formula calls, multiple returns, symbolic derivation, verification, and explore modes. Turing complete via recursive formula calls with conditional base cases.

## Build commands

```bash
make              # build (C++17, GCC 7+ or Clang 5+)
make test         # run all tests (4176+)
make sanitize     # ASan + UBSan
make test-clang   # optional: rebuild + run tests under clang++ (soft-skip if not on PATH)
make analyze-fast # cppcheck only (~1-2 min, per-cycle gate)
make analyze-full # clang-tidy (~10s after 2026-05-07 hang fix; was hung indefinitely before)
make analyze      # both tiers (analyze-fast + analyze-full)
make valgrind     # memcheck on full test suite (~5-8 min; user-triggered batch oracle)
make fuzz         # libFuzzer harness for parser; Clang-only; pre-release / post-parser-change check
```

Run: `./bin/fwiz [flags] <file>(<var>=?, <var>=?!, <var>=<value>, <var>=<expr>, ...)`

Flags: `--steps`, `--calc`, `--explore`, `--explore-full`, `--verify all`, `--verify A,B`, `--derive [N]`, `--cse [N]`, `--approximate`, `--exact`, `--fit [N]`, `--output FILE`, `--no-numeric`, `--precision N`, `--strict-includes` (default on since M3), `--legacy-implicit` (restores old base_dir auto-probe), `-I <dir>` (repeatable include-path entry)

## Architecture

Header-only, no external dependencies. Source in `src/`, examples in `examples/`.

**Pipeline:** source → `lexer.h` → `parser.h` → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI). `fit.h` provides curve fitting (sampling, templates, composition). `lexer.h` `read_number` handles scientific notation (`[eE][+-]?[0-9]+` tail, since 2026-05-13); bare `e`/`E` not followed by digits becomes a separate IDENT token. `DOTDOT` (`..`) and `AT` (`@`) tokens added (gen-6 cycle 1, 2026-06-22); `read_number` inner loop stops at the first `.` of a `..` so float ranges like `1.5..3` lex correctly; `..` check precedes the number-start dispatch.

**Memory:** Arena allocator (`ExprArena`). `ExprPtr` is raw `Expr*`. No shared_ptr. 100% cache-friendly traversal.

**Solver:** `enumerate_candidates()` generates candidates (7 strategies), shared by solve/derive/verify modes. `resolve()` returns first valid result. `resolve_all()` returns `ValueSet` (all solutions or range). `resolve_one()` errors on multiple results. Algebraic solver includes quadratic formula (`decompose_quadratic` detects `ax²+bx+c` form).

**Numeric solver:** Strategy 6 — adaptive grid scan with Newton/bisection refinement. Enabled by default. `try_resolve_numeric()` handles equation-based root-finding and system-probe fallback (for recursive formulas). Re-entrance guard (thread-local set) prevents stack overflow when numeric solver is called recursively. Memoization via `numeric_memo_`. Results classified as exact (`=`) or approximate (`~`) via forward verification. Newton uses symbolic derivatives automatically when `symbolic_diff_simplified` succeeds (quadratic convergence, 2 evals/iteration); falls back to central finite-differences when it returns `nullptr`.

**Cross-equation elimination:** Strategy 7 — for target T in equation E1 with unknown U, finds equation E2 that can express U, substitutes into E1, solves the reduced single-variable expression. Two-level elimination handles 3-variable chains. `expand_for_var()` in `expr.h` distributes MUL over ADD/SUB to enable quadratic decomposition of substituted expressions. `flatten_multiplicative()` handles non-numeric denominators (`a / expr`).

**Derive unfolding:** Formula call bodies are inlined into parent expressions when possible, enabling algebraic solving through formula calls. Detects self-referencing calls and falls back to direct sub-system derivation. `FormulaSystem::approximate_mode` (bool, default false) is set by `--approximate`; `format_derived` (system.h) reads it: exact path calls `fmt_exact_double` on collapsed numerics; approximate path runs `substitute_builtin_constants` (expr.h — replaces `pi`/`e`/`phi` Var nodes with their Num values) then re-simplifies so adjacent numerics fold, then stringifies without recognition. `derive_all` dedup: candidates are fingerprinted via `fingerprint_expr` (Schwartz–Zippel numeric evaluation at prime-cycled test points); one canonical form per fingerprint is retained using `canonicity_score` as tiebreaker. Results emitted in ascending `canonicity_score` order — simplest (fewest leaves) first; always-NaN sentinel candidates sort last but are still emitted. `--derive N` (N ≥ 1) caps output at N results after sorting. File-defined constants are injected via `build_alias_table()` (pure query — returns a copy of `aliases_`) or `populate_aliases_()` (side-effect mutator — writes `aliases_` in place, no return value) so user values like `deg` appear by name in output. Side-effect-only call sites use `populate_aliases_()` directly; query-only callers (`format_derived`, `derive_all`) use `build_alias_table()`.

**Derive CSE:** `--cse [N]` (default N=3) extracts AT MOST N helpers from the (canonicalized, capped) winner set, ranked by `value = (occurrences - 1) * (leaves - 1)` — the approximate character savings each helper would yield. Helpers are named `t1, t2, ...` in a `# Helpers` preamble. Two primitives: `cse_extract(exprs, cap, occupied)` (system.h, before the class) counts subtree occurrences, ranks candidates by value, takes the top-N, then re-sorts topologically (smallest first) for nested-helper composition; `cse_replace(e, helpers)` (expr.h) rewrites a tree post-order with a pointer-equality short-circuit so the no-match path costs zero allocations. Pass runs inside `derive_all` BEFORE `format_derived` calls. Pre-canonicalization via `simplify(distribute_over_sum(e))` mirrors the canonicalizer `format_derived` runs internally so structurally-equivalent winners count under the same key — gated to run ONLY when CSE is active (zero-overhead no-CSE path). `output_cap` (= `--derive N`) is applied INSIDE `derive_all` BEFORE the CSE pass so helpers reflect only printed equations. Output round-trips: piping `--cse` output back into fwiz parses and solves correctly (helpers become regular variables in the loaded system). Single-leaf atoms (`t = 2*b`) are never extracted — they have value 0.

**Rewrite rules:** Data-driven simplification via `.fw` patterns. 26 builtin rules (trig symmetry, inverse pairs, abs, log/exp, power rules, division / reciprocal cancellation, complex identity `i^2=-1`, negative-exponent normalization `x^n = 1/x^(-n) iff is_neg_num(n)`). Commutative flattened matching handles N-term additive/multiplicative permutations. Rules loaded from `BUILTIN_REWRITE_RULES` string; user `.fw` files can add more. `RewriteRule::condition` is `std::optional<Condition>`, parsed once at rule-load time by `parse_condition` (system.h) and evaluated per match by `check_condition` (expr.h free function). `||` in rule conditions works correctly. `Condition`/`CondClause`/`CondOp`/`CondLogic` structs and `check_condition` live in `expr.h` (after `ValueSet`, before `RewriteRule`). **Typed-binding predicates** (since 2026-05-10, Future #53; unified gen-5 cycle 3a 2026-05-15; user-defined sets cycle 3b 2026-05-16; infix `in` syntax cycle 3f 2026-05-16): rule conditions accept typed predicates that test the runtime binding of a wildcard. Two canonical predicates: `is_neg_num(n)` (structural — negative numeric literal) and `is_in(v, set_name)` (membership — canonical for all type/set/dimension predicates since cycle 3a). **Surface syntax (cycle 3f):** `v in set_name` is the preferred user-facing form — `iff x in int` reads as the math `x ∈ ℤ`. The function-call form `is_in(v, set_name)` remains backward-compatible; both lower to the same `FUNC_CALL("is_in", [v, set_name])` AST. The infix form is synthesised by a ` in ` string-scan in `parse_condition` (system.h) BEFORE the comparison-op loop, so `in` binds looser than `==`/`<` — parenthesise if `iff x == 5 in int` were ever intended (it would split at ` in ` first and error on `x == 5`; explicit `(x == 5) in int` is the parenthesised form). `in` is a true lexer keyword (`TokenType::IN`); this is structurally distinct from the cycle-1.1 `{if, iff, e}` NUMBER-IDENT desugar denylist, which is a parser-level identifier-shape guard. Reserved-word side effects: `in` cannot appear as a variable name OR as an expression-context identifier; it CAN appear as a parameter name in formula-call bindings (`foo(in=value)`) per Python-class/def precedent (see `parse_call_args` IDENT|IN widening at system.h:~2717). Chained `x in y in z` raises a clear "Infix 'in' does not chain" error. `is_int(v)` and `is_in_dimension(v, dim)` are **accepted aliases** that rewrite to `is_in` form at parse time via `parse_condition` (system.h); they are sugar, not distinct predicates. Users can still write `is_int(n)` in rewrite rules — the engine normalizes to `is_in(n, int)` internally. **User-defined predicate sets** (since cycle 3b): a `[name(param)] iff ...` section header declares a `USER_PREDICATE` kind `SetDef` — the body is parsed as a `Condition` stored in `SetDef::predicate`; `is_in(v, name)` then evaluates that condition with `param` bound to the queried value. Example: `[whole_number(n)] iff n >= 0 && is_in(n, int)`. `is_in(5, whole_number)` returns true; `is_in(3.7, whole_number)` returns false. Recursion guard (thread-local set keyed on set name) prevents self-referential sets from stack-overflowing. `is_in` dispatches via `SetDef::Kind` switch in `check_condition` (3 cases since cycle 3b — was 2): `BUILTIN_PREDICATE` → calls `sdef.membership(evaluate(bound_expr).value_or_nan())` (the `.value_or_nan()` boundary escape is intentional — NaN is meaningful for the `imaginary` built-in); `DIM_SECTION` → calls `compute_dim(*bound_expr, set_ctx)` and compares the result `DimMap` to `{{set_name, 1}}` (map-equality; works for both bare Vars and compound expressions like `kg*2` since cycle 3c; returns false on nullopt mismatch sentinel); `USER_PREDICATE` → evaluates `sdef.predicate` with `sdef.parameter` bound to the queried ExprPtr. `is_predicate_section` (system.h) + `register_predicate_section` (system.h) are the two-pass load helpers for predicate sections. Four built-in named sets: `int` (integer-valued), `real` (finite double), `rational` (currently same as real), `imaginary` (accepts NaN-sentinel for `i`-containing expressions — renamed from `complex` in cycle 3b; `complex` reserved for future mathematical-superset definition). Fail-safe throughout: unknown binding → false. Encoded as `CondClause{lhs=FUNC_CALL(name, args), rhs=nullptr, op=CondOp::EQ}` — FUNC_CALL-in-lhs with sentinel null rhs, NOT a new clause type. `is_predicate_clause` recognizes `is_neg_num` and `is_in` only (aliases are rewritten before it runs). `check_condition` 4th param is `const SimplifyContext* set_ctx = nullptr` (was `const map<string,string>* dim_map` — changed cycle 3a); `RewriteRulesGuard` 5th-arg type changed from `const map<string,string>*` to `const SimplifyContext*`; the 14 comparison-clause call sites are unchanged (param defaulted). **Dispatch constraint:** `is_in` predicate clauses require rewrite-rule context (`complex` LHS — where the pattern matcher provides `expr_bindings`); they cannot fire from equation-condition context (`var = expr if cond`) because that path provides null `expr_bindings`. Stdlib authors writing dimensional-rejection rules MUST use rewrite-rule shape. Predicate set extends per-consumer schedule (Future #65); remaining: `is_pos_num`, `is_num`.

**Function definitions:** Builtin functions (sin, cos, sqrt, log, abs, etc.) defined as embedded `.fw` sections with `@extern` for C++ evaluation and inverse equations for reverse solving. Custom functions registered via `register_function()` C++ API. Function inversion uses a thread-local callback resolved from `.fw` sub-system definitions. **Cross-file cycle detection** (since 2026-05-13): `load_sub_system` guards against recursive self-loading via a thread-local `currently_loading` set + RAII `LoadGuard`; re-entrant loads throw `CrossFileResolutionCycleError` (sibling exception — not `std::runtime_error`). Prevents SIGSEGV when a `.fw` file's body calls a function whose name matches its own filename (Future #69). **Settings propagation** (since gen-5 cycle 3h 2026-05-16, closes Future #83): `copy_metadata_to_sub(FormulaSystem&)` private helper mirrors parent solver-affecting state (`trace`, `numeric_mode`, `approximate_mode`, `custom_functions_`, `type_map_`, `set_definitions_`) to a freshly constructed sub. Called from THREE sites: `load_sub_system` normal path, `load_sub_system` auto-section path, AND `register_function_section` (the new call site closes the gap where the pre-cached recursive-section sub silently inherited `numeric_mode=false`). **Recursive FUNCTION_SECTION reverse-solve** (since gen-5 cycle 3h, closes Future #90 + #92): `is_in(8, fibonacci) == true` works via the canonical helper-equation body. Two solver fixes: Strategy 5 skips compound `sub_var == target` bindings (`n = n-1` inside a self-recursive section) while preserving the pure-Var `x = x` positional-arg case; Strategy 6's emission predicate gains a `contains_var_in_condition(cond, var)` check (expr.h) so equations like `result = n if n <= 1` are visible to numeric scan when solving for `n`. Direct-body form (`result = fibonacci(n=n-1) + ...`) remains a parse error tracked by Future #91; factorial reverse-solve parked as Future #94 (different structural blocker). **`@include` + strict-includes** (M1–M3, 2026-06-23, Future #80 DONE): `@include "path.fw"` directive — explicit cross-file inclusion; search order file-relative → `-I <dir>` entries → `FWIZ_PATH` env. `strict_includes_` (bool, default `true` since M3) — cross-file formula calls require `@include`; base_dir auto-probe and flat-file-as-implicit-system are both OFF by default. `--legacy-implicit` restores old behavior (one-release compat window). Callable cross-file systems must declare an explicit `[name(args)->ret]` section (name == file stem); a flat `@include`'d file merges equations into the parent but is not callable by stem. `process_includes()` pre-pass (in `load_with_sections`, before `split_sections`) resolves each `@include`, merges it via recursive `load_file`, and records the canonical path in `included_files_` (allow-list). `resolve_from_included(stem)` stem-scans `included_files_`; `resolve_file_path(ref, is_literal, searched*)` is the unified path resolver. A miss throws `StrictIncludeError` (sibling exception, NOT `std::runtime_error` — propagates past solver's silent `catch (const std::runtime_error&)` sites so the "add `@include`" hint reaches the user). `is_postload_builtin(name)` (expr.h, next to `is_aggregate_reducer`) is the discriminator in the `extract_positional_calls` `StrictIncludeError` catch arm: inline post-load/simplifier builtins (`diff`/`integral`/`range`/`vec`/`mat`/`matmul`/`det`/`inv`/`transpose`/`seq`/`map`/`foldl`) are returned for later passes rather than re-thrown as missing cross-file calls. All include-related fields propagate via `copy_metadata_to_sub`.

**Simplifier:** Additive and multiplicative flattening. `rebuild_multiplicative` splits factors by exponent sign: positive exponents → numerator product, negative exponents (sign-flipped) → denominator product, emitting `DIV(num, denom)` when any negative-exp factors exist. Effect: `MUL(a, POW(b, Num(-1)))` renders as `a / b`; `^(-n)` forms never appear in derive output (walker-tested). Structural fractions: `DIV(Num(a), Num(b))` preserved when non-integer, with GCD normalization and exact rational arithmetic (`to_rational()`, `make_rational()`). Most pattern-match rules migrated to `.fw` rewrite rules. Extend flattening logic for structural simplification; add new patterns as `.fw` rules. Structural fractions flow into solve output via `fmt_solve_result` in `main.cpp`. Default (exact) mode: `fmt_exact_double(v)` (fit.h) attempts constant recognition (`pi`, `e`, `phi`, `sqrt(2/3/5)`, `log(2/3/10)`, rational multiples) before falling back to `fmt_num`. `--approximate` mode: always `fmt_num`. The former `is_power_of_10` heuristic has been deleted — `--approximate` is the principled replacement. See Known-Issues #6 for remaining provenance scope (`--steps`/`--calc` traces).

**Symbolic provenance carrier:** `solved_symbolic_` (`mutable map<string, ExprPtr>`) and `aliases_` (`mutable map<string, double>`) on `FormulaSystem` carry symbolic forms and the alias-resolution table alongside the numeric `bindings` map, so `--steps`/`--calc` trace sites render from the same ExprPtr as final output — trace and final cannot disagree. `fmt_trace(double, ExprPtr=nullptr, key="")` is the single unified render helper. `type_map_` (`map<string, BindingType>` variable→type-record; replaces `dim_map_` from cycle 2 — gen-5 cycle 3a 2026-05-15) is the third parallel map. `BindingType { DimMap dim; std::set<std::string> sets; }` carries the dimension exponent map and any named-set memberships (e.g. `int`). `DimMap = std::map<std::string,int>` — maps each base-dimension name to its exponent (e.g. `{mass:1}` for kg, `{length:1,time:-1}` for m/s). `compute_dim(const Expr&, const SimplifyContext*) → std::optional<DimMap>` (expr.h, since gen-5 cycle 3c 2026-06-06) propagates dimension through MUL (add exponents), DIV (subtract), POW (integer-exponent scale), NEG (passthrough), ADD/SUB (match-or-nullopt mismatch — nullopt is the sentinel), Var (type_map lookup), Num (dimensionless empty-map). `BuiltinMeta.dim_propagate` callback (added cycle 3c) handles FUNC_CALL cases: `sqrt` halves exponents, `abs` passes through; unregistered functions yield dimensionless `{}`. `set_definitions_` (`map<string, SetDef>`) is the named-set registry: carries `BUILTIN_PREDICATE`, `DIM_SECTION`, and (since cycle 3b) `USER_PREDICATE` entries; `USER_PREDICATE` entries store a `Condition` and the parameter name — the `ExprPtr` arena for these conditions must outlive the `check_condition` call (Future #87 PARKED: cross-system arena lifetime invariant). Cross-file propagation: `load_sub_system` copies `type_map_` AND `set_definitions_` into both branches of the child system (pre-existing bug at lines 2892-2900 fixed in cycle 3a — the auto-section branch had silently omitted `dim_map_` propagation). Future #82 (PARKED): consolidate the parallel-map family when a 4th appears. Trigger unchanged.

**Two evaluators:** `Checked<double> evaluate(const Expr&)` — numeric projection, collapses tree to a `double`; empty (`!has_value()`) for structural failures (unresolved variable, unknown function, arg-count mismatch, `undefined`, null pointer). Division by zero returns empty via NaN sentinel — not a separate bool. `Checked<T>` (expr.h:30-89) is a NaN-sentinel optional: `sizeof(Checked<double>) == sizeof(double)` (8 bytes vs 16 for `std::optional<double>`); `has_value()` / `operator bool` to test; `.value()` to unwrap (asserts on empty in debug); `.value_or_nan()` is the deliberate boundary escape for handing off to the pure-double numerical root-finder layer — its use is grep-worthy and should stay rare. `ExprPtr evaluate_symbolic(const Expr&)` — exact projection, preserves integer rationals as `DIV(Num, Num)`; used by the simplifier's constant-folding paths. New number types (complex, matrix) extend `evaluate_symbolic`; `evaluate` stays real-valued. Vec/mat FUNC_CALL sugar (since 2026-05-10) follows this pattern: `evaluate()` returns empty for any `vec`/`mat` node; simplifier-level dispatch handles arithmetic instead. Ragged matrix literals (rows with differing column counts) are rejected at parse time with `RaggedMatrixError` (sibling exception) naming the first divergent row (since 2026-05-13).

**Symbolic differentiation:** `symbolic_diff(const Expr&, const std::string& var) → ExprPtr` (expr.h) — two-level dispatch: per-AST-class switch for ADD/SUB/MUL/DIV/POW/NEG, `builtin_meta()` registry lookup for FUNC_CALL (9 builtins: sin, cos, tan, asin, acos, atan, log, sqrt, abs — migrated from if-chain at M3). Returns `nullptr` for unrecognized FUNC_CALLs (signal to the post-load pass). `symbolic_diff_simplified` wrapper calls `simplify()` on the result. Post-load pass `resolve_diff_in_equations` (system.h) rewrites `diff(named_var, x)` and `diff(formula_call, x)` nodes after all equations parse and rewrite rules load. Two API surfaces: (1) `sensitivity = diff(force, mass)` as an in-file builtin; (2) `diff(target, var)=?[alias]` as a CLI query target (since #67: synthesised as `<alias> = diff(...)` in `parse_cli_query` and routed through the standard post-load passes — no parallel dispatch layer). `--derive` is unchanged — `diff(f, x)` is the symbolic-differentiation surface; no new `--diff` flag was added. `sign(x)` registered as a builtin with `sign_eval` numeric evaluator. Three new rules in `BUILTIN_REWRITE_RULES`: `x^a/x^b = x^(a-b) iff x != 0`, `abs(x)/x = sign(x) iff x != 0`, `abs(x)/x = undefined iff x = 0`.

**Symbolic integration:** `symbolic_integrate(const Expr&, const std::string& var) → ExprPtr` (expr.h) — sibling of `symbolic_diff`, same nullptr-on-miss contract. Per-AST-class switch + `BuiltinMeta` registry lookup for FUNC_CALL (M3-migrated, Future #49). ~25 atomic Tier 1 patterns (constants, `x^n` power rule, `1/x → log(x)`, `e^x`, `sin/cos/tan`, linearity over ADD/SUB, scalar MUL/DIV) plus M2 and M3 extensions. Unrecognized forms return `nullptr`; wrapper `symbolic_integrate_simplified` calls `simplify()` after. Post-load pass `resolve_integral_in_equations` (system.h) is a 4-line wrapper around the generic `resolve_at_load<Rewriter>(rewriter, up_to)` primitive (Future #48) — as is `resolve_diff_in_equations`. Two surfaces: inline `f = integral(g, x)` resolved at load time, and `integral(target, var)=?[alias]` or `integral(target, var, lo, hi)=?[alias]` CLI query (since #67: synthesised as `<alias> = integral(...)` in `parse_cli_query` and loaded via `sys.load_string` — no parallel dispatch layer). No `--integrate` flag (see Future #64). Unevaluated forms preserve the `integral(...)` FUNC_CALL, mirroring diff. **M2 additions (2026-05-10):** derivative-divides u-substitution via `try_u_sub_integrate` (MUL branch, depth ≤ `U_SUB_DEPTH=2`, candidates sorted by leaf count); definite 4-arg form `integral(f, x, a, b)` with symbolic F(b)-F(a) primary path and `adaptive_simpson` numeric fallback (`ADAPTIVE_SIMPSON_MAX_DEPTH=30`, tolerance=`NUMERIC_TOLERANCE`, NaN at any sample → preserve unevaluated). **M3 additions (2026-05-10, arc complete — Future #16 DONE):** `try_ibp_integrate` — integration by parts via LIATE heuristic (depth ≤ 3, thread-local `ibp_depth_` counter; cyclic IBP stays unevaluated by design). `liate_priority` ranks u/dv selection: Logarithmic(5) > Inverse-trig(4) > Algebraic(3) > Trig(2) > Exponential(1). `mul_through_div` helper preserves structural fractions in IBP products. DIV-branch extended: `c/(k*x) → (c/k)*log(x)`, both-contain-var → `try_u_sub_integrate`. `BuiltinMeta` registry (#49 DONE) consolidates diff and integrate per-builtin callbacks; `symbolic_diff`'s if-chain and `symbolic_integrate`'s if-chain both replaced. Migration of callbacks to `.fw` rules gated on Future #53.

**Tree walkers:** `tree_map<Fn>(ExprPtr, Fn)` and `tree_map_leaf<Fn>(ExprPtr, Fn)` in `expr.h` are the standard post-order rewrite primitives. Both use pointer-equality short-circuit (zero allocations on no-match). `tree_map` invokes `fn` on every node after children; `tree_map_leaf` invokes `fn` on `NUM`/`VAR` terminals only. Five in-tree consumers: `substitute`, `substitute_builtin_constants`, `expr_recognize_constants` (leaf), `cse_replace`, `resolve_diff_calls` (full). `expand_for_var` is the explicit non-consumer (post-recurse sibling shape inspection). New tree passes should prefer a `.fw` rewrite rule first; use `tree_map`/`tree_map_leaf` only when pattern-matching on a static LHS is insufficient.

**Pattern matcher:** `match_pattern()` with commutative flattened matching. Variables in patterns are wildcards; builtin constants match literally. Supports N-term additive permutation search and multiplicative coefficient extraction.

**ValueSet:** Unified representation for conditions, ranges, and solutions. Intervals + discrete points + periodic families (since 2026-05-08, Future.md #12) for trig solutions: `{double base; ExprPtr period}` parameterized by integer `k`. Set operations: intersect, union, filter. `covers_reals()` for rewrite rule exhaustiveness checking.

**Undefined:** Symbolic `undefined` keyword (`Var("undefined")`) for domain boundaries. Propagates through arithmetic. Rewrite rules can declare `x/x = undefined iff x = 0` for exhaustiveness checking.

## Language features

### Conditions (if / iff)
```
y = sqrt(x) if x >= 0                          # one-directional condition
y = 0 if x < 0                                 # piecewise branching
tax = income * 0.1 if income > 0 && income <= 50000  # compound
result = 1 iff x > 0                           # bidirectional (enables inverse reasoning)
y = x, if x > 0                                # optional comma
```
- `if` — one-directional: condition checked forward only
- `iff` — bidirectional: condition can be inverted (used in `stdlib/stdlib.fw` piecewise functions)
- Operators: `>`, `>=`, `<`, `<=`, `=`, `==`, `!=`. Compound: `&&`, `||`.

### Global conditions
```
side > 0                        # standalone line — constrains globally
area >= 0
```

### Inline comments
```
y = x + 1  # this is a comment
```

### Cross-file formula calls
```
rectangle(area=?floor, width=width, height=depth)
volume = floor * height
```
Expression bindings: `factorial(result=?prev, n=n-1)` — `n-1` evaluated in parent scope.

Positional arguments: `sin(3)` maps to `sin(x=3, result=?)` using `[sin(x) -> result]` header metadata.

**Direct-body recursive forms** (since gen-5 cycle 3i, 2026-05-17): both `result = fibonacci(n=n-1) + fibonacci(n=n-2) if n >= 2` (named-arg) and `result = fibonacci(n-1) + fibonacci(n-2) if n >= 2` (positional) parse and solve correctly inside function-section bodies. Pre-cycle-3i required a helper-equation workaround for both shapes. Engine: `extract_formula_calls` (system.h) is now unified for both `?`-form and no-`?` named-arg form (a non-static member accepting `FormulaSystem* self`); `register_function_section` calls `resolve_positional_calls` on the pre-cached sub to mirror `load_with_sections`'s post-load passes (Future #96 PARKED tracks consolidation when a third such pass is added).

### Sections and function definitions
```
[square(x) -> result] = x^2                    # single-line with = sugar
[abs(x) -> result]                              # multi-line piecewise
= x iff x >= 0
= -x iff x < 0
[sin(x) -> result] @extern sin; x = asin(result)  # @extern + inverse
```
- `[name(args) -> return]` declares a section with positional args and return variable
- `@extern func` bridges to C++ function pointer for fast numeric evaluation
- Lines starting with `=` in a section with `-> var` expand to `var = ...`
- `;` works as a line separator anywhere

### Rewrite rules
```
sin(-x) = -sin(x)                              # simplification pattern
log(x^n) = n * log(x) iff x != 0              # with condition
x/x = 1 iff x != 0                            # exhaustive pair...
x/x = undefined iff x = 0                     # ...covers full domain
```
Complex LHS (not `var = expr`) parsed as rewrite rules. Variables are wildcards; builtin constants match literally.

### Multiple returns
- `x=?` — all solutions (returns ValueSet)
- `x=?!` — exactly one solution (errors on multiple)
- `x=?alias` / `x=?!alias` — with alias
- CLI values can be expressions: `width=2^3, height=sqrt(9)`

### Recursion
```
result = 1 if n <= 0
result = n * factorial(result=?prev, n=n-1) if n > 0
```
Depth guard: `max_formula_depth` (default 1000).

### Built-in constants
`pi`, `e`, `phi`, `i` available in any equation. `pi`/`e`/`phi` are symbolic in derive, numeric in solve; `i` (imaginary unit, since 2026-05-09) has a NaN binding — `evaluate()` on `i`-containing expressions returns empty, and `i^2` simplifies to `-1` via rewrite rule. File defaults override builtins. The built-in named set for `i`-containing expressions is `imaginary` (renamed from `complex` in cycle 3b, 2026-05-16); the name `complex` is reserved for a future mathematical-superset definition.

### Dimension annotations (since gen-3 cycle 2, 2026-05-15; extended cycle 3a + 3b)

Three section flavors share the `[name...]` header syntax, distinguished by shape:

| Syntax | Kind | Example |
|--------|------|---------|
| `[name]` (no parens, no `->`) | `DIM_SECTION` — dimension category | `[mass]` |
| `[name(param)] iff ...` (param, body) | `USER_PREDICATE` — predicate set (since cycle 3b) | `[whole_number(n)] iff n >= 0 && is_in(n, int)` |
| `[name(args) -> ret]` (args + return) | formula section | `[f(x) -> result]` |

```
[mass]                          # dim section: all LHS names inside get dim=mass
g = 1
kg = 1000 * g

[whole_number(n)] iff n >= 0 && is_in(n, int)  # predicate section (cycle 3b)

m_obj:mass = 10 * kg            # : annotates a binding with its dimension atom
n:(int, mass) = 5               # intersection: n gets dim=mass AND sets={int}
q:(whole_number, mass) = 5      # intersection with user-defined predicate
mass.kg                         # dot-dispatch resolves into the [mass] dim section
```
- `[name]` with no parens and no `->` is a dimension section (`is_dimension_section` predicate). Its body registers all LHS names (from both `equations` and `defaults`) into `type_map_` with `.dim = "mass"`.
- `[name(param)] iff ...` is a predicate section (`is_predicate_section` predicate, since cycle 3b). `register_predicate_section` parses the body (inline single-line or implicit-AND multi-line) into a `Condition` stored in `SetDef{kind=USER_PREDICATE, parameter=param, predicate=cond}`. Empty body: silently inert (no `SetDef` registered). Forward references between predicate sections work because both passes of `load_with_sections` complete before `is_in` dispatch evaluates any condition.
- `:` is a COLON lexer token; `parse_line` detects `IDENT COLON IDENT EQUALS` (atomic) or `IDENT COLON LPAREN atom-list RPAREN EQUALS` (intersection). Operators (`*`, `/`, `^`) inside parens raise `BindingAnnotationError` (sibling exception).
- **Intersection classification (since cycle 3a):** each atom in the intersection is looked up in `set_definitions_`. `DIM_SECTION` atoms → `BindingType.dim`; `BUILTIN_PREDICATE` atoms → `BindingType.sets.insert(atom)`; `USER_PREDICATE` atoms → `BindingType.sets.insert(atom)`. Unknown atoms throw `BindingAnnotationError` naming the atom and listing built-in alternatives.
- **Four built-in named sets** registered in `set_definitions_` at `load_builtins()`: `int`, `real`, `rational`, `imaginary` (renamed from `complex` in cycle 3b — `complex` is reserved for a future mathematical-superset definition; error messages now say "imaginary"). `is_in(v, int)` in a rewrite-rule condition evaluates via `set_definitions_["int"].membership(value)`. **Design invariant (AC8):** `[my_int(n)] iff is_in(n, real) && is_in(n, int)` is functionally equivalent to the built-in `int` set — built-ins are optimized C++ fast-paths of what users can express in `.fw`.
- `is_in_dimension(v, dim)` and `is_int(v)` are **sugar**: `parse_condition` rewrites them to `is_in(v, dim)` and `is_in(v, int)` at parse time; users can still write either form.
- Dot-dispatch: `resolve("mass.kg", {})` routes through the dim section's sub-system via the `load_sub_system` / `custom_function_defs_` shim at the top of `resolve()`.
- `type_map_` and `set_definitions_` both propagate into child systems via both branches of `load_sub_system` (cycle 3a bug fix — the auto-section branch had silently omitted propagation).

### Unit-suffix desugar (since 2026-05-13)
```
mass = 100kg                    # parses as MUL(Num(100), Var("kg")) via parser primary() desugar
v = 1.5e3                       # 1500 — scientific notation in lexer (`read_number`)
f = 100sin(x)                   # MUL(Num(100), FUNC_CALL("sin", [Var("x")])) — function-call branch
```
Parser-only desugar (no new `ExprType`, no new `TokenType`). The identifier is an ordinary `Var`; unit semantics live in stdlib `.fw` bindings — `stdlib/units/si-minimal.fw` (since cycle 2: 42 lines) provides the 7 SI base units, common SI prefixes (km/mm/um/nm/Mm/Gm/g/mg/ms/us/ns/min/hr/day), and 5 derived units (N/J/W/Pa/Hz). Lexer's `read_number` consumes scientific notation (`[eE][+-]?[0-9]+`) so `100e3` stays numeric instead of becoming `MUL(100, Var("e3"))`. `100m^2` parses as `(100 * m)^2` (precedence quirk — Future #74) and emits a parse-time warning; `100sin(x)^2` does NOT warn (function-call branch is mathematically correct). **CLI-arg `var=100kg` works end-to-end since cycle 2** (Future #73 DONE): `parse_cli_query` defers any RHS whose `evaluate` returns empty (unbound `Var` at CLI-parse time) to post-load resolution via the `synthetic_equations` channel — same mechanism as `diff`/`integral` CLI args. `allow_symbolic` check runs before the defer branch so `--derive`/`--fit` retain their symbolic-RHS contract. **Gen-3 cycle 2 SHIPPED (2026-05-15):** hybrid dim model substrate — bare `[name]` sections, `:` COLON lexer token, intersection form `n:(int, mass)`, `dim_map_` on `FormulaSystem`, `is_in_dimension`/`is_int` predicates, `BindingAnnotationError` sibling exception. **Gen-5 cycle 3a SHIPPED (2026-05-15):** type-axis unification — `type_map_` (replaces `dim_map_`, value type `BindingType{dim, sets}`), `SetDef` registry with built-in named sets, `is_in` canonical predicate with parse-time rewrite of legacy aliases, `SimplifyContext` transport, intersection-atom classification via kind-dispatch. **Gen-5 cycle 3b SHIPPED (2026-05-16):** user-defined predicate sets — `[name(param)] iff ...` predicate section family, `USER_PREDICATE` Kind added (4 built-ins now: int/real/rational/imaginary; `complex` renamed to `imaginary` for NaN-sentinel naming honesty). Design victory (AC8): users can write `[my_int(n)] iff is_in(n, real) && is_in(n, int)` functionally equivalent to built-in `int`. **Gen-5 cycle 3c SHIPPED (2026-06-06):** `compute_dim` propagation algebra — `DimMap` exponent-map representation, `compute_dim` recursive fold, `BuiltinMeta.dim_propagate` callbacks (sqrt/abs), DIM_SECTION `is_in` arm lifted from bare-Var to compound-expression. Closes Future #7b FULL. Function-section sets deferred to cycle 3d.

### Bounded aggregation (since gen-6 cycle 1, 2026-06-22)

**Range literals** — `[lo..hi]` (step=1) and `[lo..hi @ step]` — are first-class expression-grammar constructs. Parsed by `parse_range_literal` (parser.h) as `FUNC_CALL("range", {lo, hi})` (arity 2, no step) or `FUNC_CALL("range", {lo, hi, step})` (arity 3). No new `ExprType`; `sizeof(Expr)` unchanged. New tokens: `DOTDOT` (`..`) and `AT` (`@`). `read_number` inner loop stops at the first `.` of a `..` so `1.5..3` scans correctly. `gen_range_values(lo, hi, step) → vector<double>` (expr.h, shared by simplifier and `parse_range` in system.h) uses count-based generation to avoid IEEE 754 drift.

**Reducers** — `sum`, `product`, `count`, `max`, `min`, `mean` — fold a body expression over a discrete domain. Two parse surfaces:
- **Explicit iterator**: `sum(body, var in [lo..hi])` — `parse_expr_or_iter_clause` detects `IDENT in` lookahead and emits `{body, Var(iter), range}` (3-arg shape).
- **Body-free count**: `count(i in [1..5])` — `{Var(iter), range}` (2-arg shape). No body expression.
- **Single-literal broadcast** (formula-call bodies): `sum(combat(atk=[1..6], def=5, dmg=?))` — the range literal appears as a FormulaCall binding; one anonymous iterator.
- **Lockstep**: `def=atk` inside the broadcast binds a second argument to follow the first.

**Unroll model** — static-domain aggregation is a macro-expansion at simplify/post-load time, NOT a new evaluator. Numeric/expression bodies: `try_unroll_aggregate(name, sa)` in `simplify_once_impl` (expr.h) — recognized by name, extracts range values via `extract_range_values`, folds with `fold_aggregate` (shared policy table for all 6 reducers). Symbolic bound → unevaluated (like `integral`), folds once bound. Formula-call bodies: post-load pass `resolve_aggregate_in_equations` (system.h, mirrors `resolve_diff`/`resolve_integral`) calls `try_unroll_aggregate_with_calls` — Shape A (explicit iterator, body is positional call or named-binding call via `clone_call_with_subst`) and Shape B (broadcast, clones the FormulaCall per domain value with `Num(v)` bindings). `is_aggregate_reducer(name)` is the single name-predicate used by both the simplifier and `extract_formula_calls`/`extract_positional_calls` guards. `fold_aggregate(name, values, make_term)` is the shared reducer policy; both the simplify path and the post-load path delegate to it. `agg_resolved_up_to_` is the dirty-flag member (same pattern as `diff_resolved_up_to_`).

**Reducer semantics**: `sum` → ADD fold (empty → 0); `product` → MUL fold (empty → 1); `count` → `Num(|values|)` (empty → 0); `max`/`min` → numeric-compare fold (empty domain → unevaluated); `mean` → sum/count as exact structural fraction via `to_rational`/`make_rational` (empty → unevaluated; `mean(i, i in [1..4])` → `5 / 2`, not `2.5`).

**Reverse-solve** — static-domain unroll produces an ordinary expression; the existing 7 strategies invert body parameters with no aggregation-specific reverse logic. Strategy 6 emission predicate extended (`formula_call_bindings_contain` helper, system.h) to emit a NUMERIC candidate when the solve target lives in a FormulaCall binding rather than in the equation RHS expression text (the post-unroll structure of formula-bodied aggregations). `resolve_all` is the sound path for piecewise-body reverse solves; `resolve()` first-wins is tracked as Future #102. **Cycle 1 result: all four steps A–D ship GREEN (3955/3955 tests).**

**Symbolic-bound aggregation via the resolve/binding path (since gen-6 cycle 2a, 2026-06-22):** `collect_vars` (expr.h) treats an aggregation reducer's iterator as a binder, not a free variable — when it encounters a reducer FUNC_CALL, it collects free vars of all args into a local set, erases the iterator name from that local set only (local-scope exclusion, not global erase, so a sibling occurrence of the same name outside the reducer survives), then merges into the caller's set. This enables `factorial(n) = product(i, i in [1..n])` to resolve via `resolve("result", {n=5})` — the solver no longer aborts trying to resolve `i` as an unknown. Forward declaration of `is_aggregate_reducer` before `collect_vars` (definition is far below in the file) makes the FUNC_CALL arm compile. **Stdlib (since gen-6 cycle 2 + 3):** `stdlib/combinatorics/` (7 files: factorial, nPr, nCr, falling, rising, hyp_pmf, hyp_at_least) and `stdlib/probability/` (5 files: expected_value, variance, hyp_mean, hyp_variance, order_statistic) are built on the aggregation primitive. Keystone: `P(≥2 clubs in 5 of 54) = 0.3467505241` (hyp_at_least) and `E[highest of 5] ≈ 10.34` (order statistic + nCr composition). nCr exact: `nCr(54,27) = 1946939425648112` (exact integer double). The combinatorics files use named-arg cross-file calls; hyp_at_least.fw inlines the PMF as nested products (cross-file aggregation body limitation — see Future #103). Iterator names use `v` not `i` in E[X]/variance bodies — the builtin rewrite rule `i^2=-1` fires before unroll on non-linear bodies (see Future #104).

**Collection ontology** (fully implemented through Cycle 3): `[]`=unordered set/range, `{}`=ordered array/sequence — `[lo..hi]` ranges, `{}` collection literals, `map`/`foldl`, named-section reducers, and collections-as-call-arguments all ship; boundary brackets and multi-iterator cartesian forms remain planned.

**Collections & aggregation primitives (gen-6 Collections Cycle 1, 2026-06-24):** `{1,2,3}` ordered collection literal — parses as `FUNC_CALL("seq", {Num(1), Num(2), Num(3)})` via new LBRACE/RBRACE lexer tokens. No new `ExprType`; `sizeof(Expr)==96` unchanged. `seq` is DISTINCT from `vec`: `vec` participates in `try_simplify_vec_mat_binop`/`try_dispatch_vec_mat_builtin` (element-wise ops and matrix dispatch); reusing `vec` for ordered collections would make `{1,2,3}+{4,5,6}` silently element-wise-add and collide with fold semantics. `map(body, i in [dom])` materializes a `seq` at simplify-time when the domain is numeric; symbolic domain stays unevaluated (same contract as `integral`). `foldl(coll, op, init)` is POST-LOAD-ONLY via `resolve_foldl_in_equations` (a 4-line `resolve_at_load` wrapper); `lookup_binary_op_body(op_name)` retrieves a two-parameter section body `[op(acc,elem)->r]` by name and threads `(acc, elem)` through it per element — uniform for all ops, no hardcoded BinOp table. `add`/`mul` provided in `stdlib/collections/operators.fw`. Post-load pass order: **map → foldl → aggregate** (load-bearing: aggregate→map ordering would break `sum(map(score(i), i in [1..3]))`). `unroll_term_with_calls` shared helper factored from `try_unroll_aggregate_with_calls` Shape A — map pass and aggregate pass both use it for per-term formula-call extraction. `collect_vars` binder guard extended: map iterator and foldl op-var are excluded from free variables (same local-scope-exclusion pattern as aggregate iterators). `seq`/`map`/`foldl` added to `is_postload_builtin` so strict-includes mode does not mistake inline uses for missing cross-file calls. **Cross-cycle invariant (AE-3):** a 2-arg op section must be in the SAME FILE as the `foldl` that references it (or pre-registered in `custom_function_defs_`) until multi-section `@include` persistence is fixed (Future #110).

**Collections-as-call-arguments (gen-6 Collections Cycle 3, 2026-06-24):** named-section reducers (`sum_of`, `product_of`, `count_of`, `mean_of` in `stdlib/collections/`) are now callable with `{}` collections — e.g. `sum_of({1,2,3})` = 6, `mean_of({1,2,3,4})` = `5 / 2` (exact). Mechanism: `call_has_seq_arg(const FormulaCall&)` (~7-line predicate, `std::any_of` over bindings) detects a `seq`-shaped argument; the forward-resolve paths in `solve_recursive` (`try_formula` lambda) and `solve_all` (FORMULA_FWD branch) splice BEFORE `prepare_sub_bindings` — when `call_has_seq_arg` fires, `unfold_formula_call_body` inlines the section body (carrying the `seq` as an ExprPtr, sidestepping the numeric-only binding channel), then `resolve_foldl_calls` folds the concrete `foldl(seq(...), op, init)`, then `inline_seq_section_calls` cleans up any surviving nested seq-bearing calls. A non-inlinable seq-arg (section body does not expose an output equation for substitution) throws `FoldOperatorError` — loud, never silent numeric fallback. `unfold_formula_call_body` was extended to re-inline nested formula calls captured at parse time (needed for composed bodies like `mean_of`'s `sum_of(xs)/count_of(xs)` which are stored as `_fc1/_fc0` placeholders). **Cross-cycle invariant:** collection-valued section arguments resolve by parent-side body inlining (`unfold_formula_call_body` on the forward-resolve path, gated by `call_has_seq_arg`); `prepare_sub_bindings` stays numeric-only by contract. The AC8 invariant (`[sum_of(xs)] = foldl(xs,add,0)` ≡ C++ `sum`) is now tested on BOTH the fold-expression path (Cycle 2b) AND the named-section path (Cycle 3).

### Vector and matrix literals (since 2026-05-10)
```
v = [1, 2, 3]                   # row vector (FUNC_CALL "vec" internally)
m = [[1, 0], [0, 1]]            # 2×2 matrix (FUNC_CALL "mat", rows are vec calls)
w = [a, b+1, c^2]              # symbolic elements supported
```
Element-wise add/sub and scalar-mul simplify automatically. Matrix builtins: `matmul(A, B)`, `det(M)` (2×2 and 3×3), `inv(M)` (2×2), `transpose(M)` (general). Shape mismatch → `undefined`. No new `ExprType` — `sizeof(Expr)` unchanged. See Developer.md §"Vectors and matrices" and Known-Issues #12 for scope. **`--derive` on whole vec/mat works today** via `solved_symbolic_` — concrete and symbolic literals, `matmul`/`det`/`inv`/`transpose` outputs, and `matmul(A, inv(A))` cancellation all round-trip cleanly; no `#10a` structural extension needed for the current derive scope. `diff` / `integral` do NOT yet distribute over vec/mat containers (Future #71, deferred to the queued Linear-algebra completeness arc).

### Numeric solving
Enabled by default. Nonlinear equations (quadratics, transcendentals, recursive inverses) solved via adaptive grid scan + Newton/bisection. Exact results use `=`, approximate use `~`.
- `--no-numeric` — algebraic only
- `--precision N` — scan density (default 200)
- Conditions narrow the search range: `x > 0` scans only positive values
- Constants: `NUMERIC_DEFAULT_SAMPLES`, `NUMERIC_TOLERANCE`, `NUMERIC_SEED`

### Curve fitting
`--fit [N]` samples a function and fits closed-form approximations. Templates: polynomial, power law, exponential (including Gaussian), logarithmic, sinusoidal, reciprocal. Recursive composition (depth N, default 5) discovers nested forms like `sin(sin(x))`, `e^(x*log(x))`. Constants recognized in coefficients (pi, e, sqrt(2), etc.).
- `--output FILE` — write best fit as `.fw` file
- `--derive --fit` — derive symbolic first, then fit alternatives
- `fit.h`: `sample_function`, `fit_base`, `fit_all`, template functions, `recognize_constant`
- Constants: `FIT_DEFAULT_SAMPLES`, `FIT_MAX_DEGREE`, `FIT_R2_THRESHOLD`, `FIT_DEFAULT_DEPTH`

### Batch / table mode (since 2026-05-11)
`--table` evaluates a query across one or more range-valued inputs and emits a TSV table. Range grammar `[start..stop]` (integer step 1, endpoints inclusive); `[start..stop @ step]` (custom step, count-based generation); `[range1, range2, ...]` (compound — concatenated values). Bounds can be expressions: `[0..2*pi @ pi/4]`. Descending ranges require explicit negative step: `[10..1 @ -1]`. Header is range vars then query aliases, both in CLI order. Cartesian product by default; `--zip` for element-wise; `--output FILE` for redirection. Unsolvable rows render as `?`. Mutually exclusive with `--derive/--verify/--fit/--explore`. `parse_range` lives in `system.h` next to `parse_cli_query`; the iteration driver sits in `main.cpp` between nested-call injection and the existing dispatch.

## Key conventions

Read `docs/Developer.md` for the full guide. Summary:

- **References for non-null** (`const Expr&`), **pointers for nullable** (`ExprPtr`)
- **`constexpr`** for predicates and constants, **`inline`** for everything else
- **Named constants** (`EPSILON_ZERO`, `EPSILON_REL`, `SIMPLIFY_MAX_ITER`)
- **`static_assert`** for enum counts, table sizes, constant ranges
- **`assert`** in factories and post-conditions
- **Enum `COUNT_` sentinels** — `case COUNT_: assert(false)`, never `default:`
- **Data-driven** — BinOp table, builtin registry, strategy enumeration
- **No empty catch blocks** — return, log, or handle
- **Write failing tests first**, commit tests before refactoring
- `make test && make sanitize && make analyze-fast` must pass before committing. `make analyze-full` (clang-tidy, ~10s post-fix) is a user-triggered batch — see Quality bar below.

## Orchestrated Development Workflow

Activate with `claude --agent fwiz-orchestrator` for multi-phase development:

```
USER BRIEF → RESEARCH → DESIGN → IMPLEMENT → REVIEW → PLAN-NEXT → repeat
```

**Agents** (in `.claude/agents/`): orchestrator, researcher, planner, critic, visionary, implementer, debugger, reviewer, doc-updater, perf-auditor, meta-reviewer. Each has focused context and restricted tools — separation of concerns.

**Slash commands** (in `.claude/commands/`): `/debug <reproducer>` spawns the debugger agent against a specific failing case.

**Artifacts** (in `.fwiz-workflow/`, gitignored): research-brief.md, design-proposal.md, implementation-log.md, review-notes.md, next-priorities.md.

**Quality bar (tiered oracle)**:
- **Per-cycle gate**: `make test && make sanitize && make analyze-fast` (cppcheck — ~1-2 min). Must pass before cycle close.
- **User-triggered batch (static)**: `make analyze-full` (clang-tidy — **~10 s** post-fix). Earlier framing claimed "1-2 h on this header-heavy codebase"; that was a fiction — the tool was hanging indefinitely on `bugprone-exception-escape` for ~9 cycles, never producing output. The 2026-05-07 hang fix excluded `bugprone-exception-escape` and `bugprone-unchecked-optional-access` (a known LLVM regression hang); the bisection lives in `.fwiz-workflow/debug-analyze-full-hang.md`. Orchestrator tracks "N cycles since last clang-tidy" and audits residuals against the cumulative diff since last green baseline. **Escalation rule**: if a user-triggered tool is "pending" for 3+ cycles with zero successful runs, do not keep recommending it — escalate to a debugger-agent diagnostic instead.
- **User-triggered batch (runtime)**: `make valgrind` (memcheck on the full test suite — ~5-8 min with `--track-origins=yes`). Complementary to `make sanitize` — ASan/UBSan are faster (and the per-cycle gate) but valgrind catches certain uninitialized-read and heap-layout patterns ASan misses, and is compiler-portable. **Clean baseline established 2026-05-12** (3330/3330 tests, 0 errors, 0 leaks). Output captured to `/tmp/fwiz-valgrind.log`; printed only on failure. Run after substantial allocator-pattern changes or before release. Soft-skips if valgrind is not on PATH.
- **Periodic**: data locality / disassembly audits on hot paths (perf-auditor agent).

**Core principle**: least code, least features, maximum flexibility, tiny fast core, infinite extendability via .fw rules.

**Simplification over filtration.** When the output of a stage contains tautological or duplicate items, first ask whether a *simplification rule* upstream would make those items collapse into their canonical siblings — making the duplication invisible to downstream stages — before adding a pruning filter to the output stage. Pruning filters are specializations; simplification rules are generalizations (any structurally-matching expression benefits).

### Recovery protocols

- **3-strike implementer rule**: if the implementer reports BLOCKED three times on the same design, the design is wrong. Do not spawn a fourth attempt — revise the design (mini critic+visionary round on the specific failing hypothesis).
- **Diagnostic round**: after two BLOCKED reports, the next spawn is the `debugger` agent (or the `/debug` command). It instruments, measures, and writes findings — does NOT fix. See `.claude/agents/debugger.md`.
- **Ship-with-followup**: if the cycle has shipped SHIP-BLOCKING tests but has SHIP-DESIRABLE outstanding, close the cycle, log Future.md entries with reopen triggers, and spin a micro-cycle for the follow-up.
- **Measure before design** (hang/perf tasks): research phase MUST include an "Empirical bisection" section — run the reproducer with every orthogonal flag, time each, identify which variants fail the same way. Skip only if the user explicitly says "the hang is in [specific layer]" with authority. Triangle-hang wasted two design rounds this way.
- **Meta-review fires automatically** at end of every cycle, not on user request. See `.claude/agents/fwiz-orchestrator.md` Phase 6.

## Future.md tiers

`docs/Future.md` is organised into four visionary tiers. The visionary audit (`/audit-future`, also auto-fired at Phase 5 entry when `Future.md` has been modified) maintains this organisation autonomously — high-confidence moves apply silently; only uncertain calls escalate to the user.

- **In-scope** — aligned with the universal math inference engine vision; tiny fast core; eligible for direct planning.
- **Wrapper-tool** — useful but belongs OUTSIDE the core (plotting, LaTeX, GUIs, integrations, output formatting that isn't load-bearing for solving). Per `visionary.md`: "Everything else (plotting, LaTeX, GUIs, integrations) is built AROUND it, not inside it."
- **Parked** — in-scope but waiting on a concrete reopen trigger. Each parked item carries the trigger condition explicitly, in the spirit of `Future.md` #20's deferral pattern.
- **Killed** — out of vision. Removed from `Future.md` entirely and recorded in `docs/REJECTED.md` (case-law sidecar) with rationale, vision principle violated, date, and optional reopen trigger.

**Lock mechanism.** Any item carrying `**Locked:** YYYY-MM-DD — <reason>` is durable: the visionary skips it on every audit. Locks are the user's veto channel — set one when you disagree with the agent's tier or want an item immune from auto-classification. Removing the lock re-enables future audits.

**Audit firing.** The audit short-circuits silently if `Future.md` mtime is older than `.fwiz-workflow/last-future-audit`. So most cycles close without spawning the visionary; the audit only runs when there's actual change to evaluate. See `.claude/agents/visionary.md` §Audit Mode and `.claude/commands/audit-future.md` for the protocol.

## Blind-spot critic — readability oracle

Auto-fires at Phase 6 prelude (before meta-reviewer). Samples 7 eligible functions from the cycle's diff (2 longest from diff, 2 random from diff, 3 random codebase-wide) and tests them with **Haiku graders** at three context tiers (body-only-comments-stripped → +signatures → +comments). Two Haiku evaluators per tier — one for purpose, one for mechanics. **If a less-capable reader (Haiku) can't accurately explain a piece of code, the code is too confusing.** Opus scores the gaps, files refactor items into `docs/Future.md`, extracts recurring patterns as rules in `docs/Code-Style.md`, and appends a row per (function, tier) to `.fwiz-workflow/blind-spot-scores.md` for trend tracking.

Full-codebase audits via `/blind-spot-sweep` (user-triggered). The blind-spot critic is the **negative-signal complement** to the existing critic/reviewer agents — it catches code that compiles, passes tests, and triggers no warnings, yet is opaque to the next agent that has to read it.

**Three scopes:** function-level (`code-explainer-purpose` + `code-explainer-mechanics`), file-level (`file-explainer`), architecture-level (`architecture-explainer`). Per cycle: 7 functions sampled per the existing rule, 1 file (largest in diff or random fallback), and 1 architecture pass (skip-when-unchanged).

See `.claude/agents/blind-spot-critic.md` and `docs/Code-Style.md`.

## Comprehension-gate principle

Several agents in this workflow use a **weaker grader (Haiku) to test the readability floor** of code, files, and architecture. The principle behind these tests:

> When a weaker grader fails to explain a function / file / architecture, the natural inclination is to explain the failure as the grader being "less capable." Resist this. **Treat the failure as a comprehension gate.** The whole purpose of using a weaker model as the readability oracle is that its failures track the floor — if Haiku can't follow it, neither will the next agent that lacks full context.

Diagnostic order on Haiku failure:

1. **Size** — unit too large for working memory? Split.
2. **Cohesion** — unrelated concerns mashed together? Separate by responsibility.
3. **Structure** — does the organisation carry meaning, or is it a wall of code? Section delimiters, headings, type taxonomies.
4. **Naming** — are identifiers doing the lifting they should? Descriptive names at function, file, and module level.

This principle applies to every agent that uses a weaker grader. It is restated in each Haiku-using agent profile (currently the blind-spot-critic) and at the top of `docs/Code-Style.md`. The rationalization "the grader is just weaker" is the exact failure mode that defeats the test.

## Log-arc reflector & autonomous mode

After the meta-reviewer at Phase 6 close, the **log-arc-reflector** runs and asks "given this cycle ran, where should we go next?" — a strategic positioning question distinct from the meta-reviewer's process audit. It returns one of `continue` / `new-cycle keep-context` / `new-cycle clear-context` / `pause-and-survey` / `new-arc`, with confidence and reasoning grounded in inputs from cycle artifacts, `tools/session-stats.py` (measurable context proxies), and `git log`. Default mode is **interactive** — the reflector recommends, the user confirms destructive choices.

**Autonomous mode** (`/autonomous <goal>` to enter, `/halt-autonomous` or any user input to exit) lets the orchestrator continue cycle-after-cycle without user intervention until the goal is met or a safety brake fires. When active, the reflector's verdicts auto-apply within the goal's `allowed_dispositions`. The mode's state lives in `.fwiz-workflow/autonomous-mode.md`. Goal-met always wins over max-cycles; `pause-and-survey` always exits autonomous and pings the user.

Track record per verdict type lives in `.fwiz-workflow/reflector-track-record.md`; reflections per cycle in `.fwiz-workflow/reflection.md`. See `.claude/agents/log-arc-reflector.md` for the protocol and `.claude/commands/autonomous.md` for the entry recipe.

## Roadmap & campaign planning

`docs/ROADMAP.md` holds the active multi-cycle arc plus queued and completed arcs. Generated by the **plan-ideator + plan-critic** pair (right-brain divergent → left-brain convergent), versioned, with prior generations archived to `.fwiz-workflow/roadmap-archive/<date>-genN.md`.

**An arc** is a multi-cycle campaign aimed at a strategic theme — broader than a Future.md item, narrower than the project vision. Sits between tactics (single-cycle implementation) and vision (the universal math inference engine).

**When the pair fires:**
- Auto — when the log-arc reflector emits `new-arc` at Phase 6.
- Manual — `/plan-campaign [optional seed text]`.

**Selection axes the critic uses:** vision alignment, current state fit, velocity match, risk profile, strategic positioning, counterfactual cost. Confidence is honest — `low` always escalates to user; in autonomous mode, `medium` or `low` exits autonomous and pings.

The ideator and critic are independent halves. They MUST NOT see each other's prior outputs across runs — same anti-collapse principle that keeps generate-then-filter pairs from drifting toward agreement.

See `.claude/agents/plan-ideator.md`, `.claude/agents/plan-critic.md`, `.claude/commands/plan-campaign.md`.
