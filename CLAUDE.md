# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

Bidirectional formula solver. Write equations once in `.fw` files, solve for any variable forwards or backwards. Supports conditions, recursion, cross-file formula calls, multiple returns, symbolic derivation, verification, and explore modes. Turing complete via recursive formula calls with conditional base cases.

## Build commands

```bash
make              # build (C++17, GCC 7+ or Clang 5+)
make test         # run all tests (3249+)
make sanitize     # ASan + UBSan
make test-clang   # optional: rebuild + run tests under clang++ (soft-skip if not on PATH)
make analyze-fast # cppcheck only (~1-2 min, per-cycle gate)
make analyze-full # clang-tidy (~10s after 2026-05-07 hang fix; was hung indefinitely before)
make analyze      # both tiers (analyze-fast + analyze-full)
make valgrind     # memcheck on full test suite (~5-8 min; user-triggered batch oracle)
make fuzz         # libFuzzer harness for parser; Clang-only; pre-release / post-parser-change check
```

Run: `./bin/fwiz [flags] <file>(<var>=?, <var>=?!, <var>=<value>, <var>=<expr>, ...)`

Flags: `--steps`, `--calc`, `--explore`, `--explore-full`, `--verify all`, `--verify A,B`, `--derive [N]`, `--cse [N]`, `--approximate`, `--exact`, `--fit [N]`, `--output FILE`, `--no-numeric`, `--precision N`

## Architecture

Header-only, no external dependencies. Source in `src/`, examples in `examples/`.

**Pipeline:** source → `lexer.h` → `parser.h` → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI). `fit.h` provides curve fitting (sampling, templates, composition). `lexer.h` `read_number` handles scientific notation (`[eE][+-]?[0-9]+` tail, since 2026-05-13); bare `e`/`E` not followed by digits becomes a separate IDENT token.

**Memory:** Arena allocator (`ExprArena`). `ExprPtr` is raw `Expr*`. No shared_ptr. 100% cache-friendly traversal.

**Solver:** `enumerate_candidates()` generates candidates (7 strategies), shared by solve/derive/verify modes. `resolve()` returns first valid result. `resolve_all()` returns `ValueSet` (all solutions or range). `resolve_one()` errors on multiple results. Algebraic solver includes quadratic formula (`decompose_quadratic` detects `ax²+bx+c` form).

**Numeric solver:** Strategy 6 — adaptive grid scan with Newton/bisection refinement. Enabled by default. `try_resolve_numeric()` handles equation-based root-finding and system-probe fallback (for recursive formulas). Re-entrance guard (thread-local set) prevents stack overflow when numeric solver is called recursively. Memoization via `numeric_memo_`. Results classified as exact (`=`) or approximate (`~`) via forward verification. Newton uses symbolic derivatives automatically when `symbolic_diff_simplified` succeeds (quadratic convergence, 2 evals/iteration); falls back to central finite-differences when it returns `nullptr`.

**Cross-equation elimination:** Strategy 7 — for target T in equation E1 with unknown U, finds equation E2 that can express U, substitutes into E1, solves the reduced single-variable expression. Two-level elimination handles 3-variable chains. `expand_for_var()` in `expr.h` distributes MUL over ADD/SUB to enable quadratic decomposition of substituted expressions. `flatten_multiplicative()` handles non-numeric denominators (`a / expr`).

**Derive unfolding:** Formula call bodies are inlined into parent expressions when possible, enabling algebraic solving through formula calls. Detects self-referencing calls and falls back to direct sub-system derivation. `FormulaSystem::approximate_mode` (bool, default false) is set by `--approximate`; `format_derived` (system.h) reads it: exact path calls `fmt_exact_double` on collapsed numerics; approximate path runs `substitute_builtin_constants` (expr.h — replaces `pi`/`e`/`phi` Var nodes with their Num values) then re-simplifies so adjacent numerics fold, then stringifies without recognition. `derive_all` dedup: candidates are fingerprinted via `fingerprint_expr` (Schwartz–Zippel numeric evaluation at prime-cycled test points); one canonical form per fingerprint is retained using `canonicity_score` as tiebreaker. Results emitted in ascending `canonicity_score` order — simplest (fewest leaves) first; always-NaN sentinel candidates sort last but are still emitted. `--derive N` (N ≥ 1) caps output at N results after sorting. File-defined constants are injected via `build_alias_table()` (pure query — returns a copy of `aliases_`) or `populate_aliases_()` (side-effect mutator — writes `aliases_` in place, no return value) so user values like `deg` appear by name in output. Side-effect-only call sites use `populate_aliases_()` directly; query-only callers (`format_derived`, `derive_all`) use `build_alias_table()`.

**Derive CSE:** `--cse [N]` (default N=3) extracts AT MOST N helpers from the (canonicalized, capped) winner set, ranked by `value = (occurrences - 1) * (leaves - 1)` — the approximate character savings each helper would yield. Helpers are named `t1, t2, ...` in a `# Helpers` preamble. Two primitives: `cse_extract(exprs, cap, occupied)` (system.h, before the class) counts subtree occurrences, ranks candidates by value, takes the top-N, then re-sorts topologically (smallest first) for nested-helper composition; `cse_replace(e, helpers)` (expr.h) rewrites a tree post-order with a pointer-equality short-circuit so the no-match path costs zero allocations. Pass runs inside `derive_all` BEFORE `format_derived` calls. Pre-canonicalization via `simplify(distribute_over_sum(e))` mirrors the canonicalizer `format_derived` runs internally so structurally-equivalent winners count under the same key — gated to run ONLY when CSE is active (zero-overhead no-CSE path). `output_cap` (= `--derive N`) is applied INSIDE `derive_all` BEFORE the CSE pass so helpers reflect only printed equations. Output round-trips: piping `--cse` output back into fwiz parses and solves correctly (helpers become regular variables in the loaded system). Single-leaf atoms (`t = 2*b`) are never extracted — they have value 0.

**Rewrite rules:** Data-driven simplification via `.fw` patterns. 26 builtin rules (trig symmetry, inverse pairs, abs, log/exp, power rules, division / reciprocal cancellation, complex identity `i^2=-1`, negative-exponent normalization `x^n = 1/x^(-n) iff is_neg_num(n)`). Commutative flattened matching handles N-term additive/multiplicative permutations. Rules loaded from `BUILTIN_REWRITE_RULES` string; user `.fw` files can add more. `RewriteRule::condition` is `std::optional<Condition>`, parsed once at rule-load time by `parse_condition` (system.h) and evaluated per match by `check_condition` (expr.h free function). `||` in rule conditions works correctly. `Condition`/`CondClause`/`CondOp`/`CondLogic` structs and `check_condition` live in `expr.h` (after `ValueSet`, before `RewriteRule`). **Typed-binding predicates** (since 2026-05-10): rule conditions accept `is_neg_num(n)` to test that a wildcard binding is a negative numeric literal — fail-safe semantics (unknown binding → false, not permissive-true). Encoded as `CondClause{lhs=FUNC_CALL("is_neg_num", {Var("n")}), rhs=nullptr, op=CondOp::EQ}` — FUNC_CALL-in-lhs with sentinel null rhs, NOT a new clause type. `check_condition` gains an optional third param `const map<string, ExprPtr>* expr_bindings = nullptr`; the 14 system.h comparison-clause call sites are unchanged. Predicate set extends per-consumer schedule (Future #53).

**Function definitions:** Builtin functions (sin, cos, sqrt, log, abs, etc.) defined as embedded `.fw` sections with `@extern` for C++ evaluation and inverse equations for reverse solving. Custom functions registered via `register_function()` C++ API. Function inversion uses a thread-local callback resolved from `.fw` sub-system definitions. **Cross-file cycle detection** (since 2026-05-13): `load_sub_system` guards against recursive self-loading via a thread-local `currently_loading` set + RAII `LoadGuard`; re-entrant loads throw `CrossFileResolutionCycleError` (sibling exception — not `std::runtime_error`). Prevents SIGSEGV when a `.fw` file's body calls a function whose name matches its own filename (Future #69).

**Simplifier:** Additive and multiplicative flattening. `rebuild_multiplicative` splits factors by exponent sign: positive exponents → numerator product, negative exponents (sign-flipped) → denominator product, emitting `DIV(num, denom)` when any negative-exp factors exist. Effect: `MUL(a, POW(b, Num(-1)))` renders as `a / b`; `^(-n)` forms never appear in derive output (walker-tested). Structural fractions: `DIV(Num(a), Num(b))` preserved when non-integer, with GCD normalization and exact rational arithmetic (`to_rational()`, `make_rational()`). Most pattern-match rules migrated to `.fw` rewrite rules. Extend flattening logic for structural simplification; add new patterns as `.fw` rules. Structural fractions flow into solve output via `fmt_solve_result` in `main.cpp`. Default (exact) mode: `fmt_exact_double(v)` (fit.h) attempts constant recognition (`pi`, `e`, `phi`, `sqrt(2/3/5)`, `log(2/3/10)`, rational multiples) before falling back to `fmt_num`. `--approximate` mode: always `fmt_num`. The former `is_power_of_10` heuristic has been deleted — `--approximate` is the principled replacement. See Known-Issues #6 for remaining provenance scope (`--steps`/`--calc` traces).

**Symbolic provenance carrier:** `solved_symbolic_` (`mutable map<string, ExprPtr>`) and `aliases_` (`mutable map<string, double>`) on `FormulaSystem` carry symbolic forms and the alias-resolution table alongside the numeric `bindings` map, so `--steps`/`--calc` trace sites render from the same ExprPtr as final output — trace and final cannot disagree. `fmt_trace(double, ExprPtr=nullptr, key="")` is the single unified render helper.

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
`pi`, `e`, `phi`, `i` available in any equation. `pi`/`e`/`phi` are symbolic in derive, numeric in solve; `i` (imaginary unit, since 2026-05-09) has a NaN binding — `evaluate()` on `i`-containing expressions returns empty, and `i^2` simplifies to `-1` via rewrite rule. File defaults override builtins.

### Unit-suffix desugar (since 2026-05-13)
```
mass = 100kg                    # parses as MUL(Num(100), Var("kg")) via parser primary() desugar
v = 1.5e3                       # 1500 — scientific notation in lexer (`read_number`)
f = 100sin(x)                   # MUL(Num(100), FUNC_CALL("sin", [Var("x")])) — function-call branch
```
Parser-only desugar (no new `ExprType`, no new `TokenType`). The identifier is an ordinary `Var`; unit semantics live in stdlib `.fw` bindings — `stdlib/units/si-minimal.fw` provides the 7 SI base units as scalar 1. Lexer's `read_number` consumes scientific notation (`[eE][+-]?[0-9]+`) so `100e3` stays numeric instead of becoming `MUL(100, Var("e3"))`. `100m^2` parses as `(100 * m)^2` (precedence quirk — Future #74) and emits a parse-time warning; `100sin(x)^2` does NOT warn (function-call branch is mathematically correct). CLI-arg `var=100kg` does NOT yet resolve `kg` (deferred CLI-value evaluation — Future #73); in-file `mass = 100kg` works today.

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
