# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

Bidirectional formula solver. Write equations once in `.fw` files, solve for any variable forwards or backwards. Supports conditions, recursion, cross-file formula calls, multiple returns, symbolic derivation, verification, and explore modes. Turing complete via recursive formula calls with conditional base cases.

## Build commands

```bash
make              # build (C++17, GCC 7+ or Clang 5+)
make test         # run all tests (2225+)
make sanitize     # ASan + UBSan
make analyze      # clang-tidy (zero warnings expected)
```

Run: `./bin/fwiz [flags] <file>(<var>=?, <var>=?!, <var>=<value>, <var>=<expr>, ...)`

Flags: `--steps`, `--calc`, `--explore`, `--explore-full`, `--verify all`, `--verify A,B`, `--derive [N]`, `--cse [N]`, `--approximate`, `--exact`, `--fit [N]`, `--output FILE`, `--no-numeric`, `--precision N`

## Architecture

Header-only, no external dependencies. Source in `src/`, examples in `examples/`.

**Pipeline:** source → `lexer.h` → `parser.h` → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI). `fit.h` provides curve fitting (sampling, templates, composition).

**Memory:** Arena allocator (`ExprArena`). `ExprPtr` is raw `Expr*`. No shared_ptr. 100% cache-friendly traversal.

**Solver:** `enumerate_candidates()` generates candidates (7 strategies), shared by solve/derive/verify modes. `resolve()` returns first valid result. `resolve_all()` returns `ValueSet` (all solutions or range). `resolve_one()` errors on multiple results. Algebraic solver includes quadratic formula (`decompose_quadratic` detects `ax²+bx+c` form).

**Numeric solver:** Strategy 6 — adaptive grid scan with Newton/bisection refinement. Enabled by default. `try_resolve_numeric()` handles equation-based root-finding and system-probe fallback (for recursive formulas). Re-entrance guard (thread-local set) prevents stack overflow when numeric solver is called recursively. Memoization via `numeric_memo_`. Results classified as exact (`=`) or approximate (`~`) via forward verification.

**Cross-equation elimination:** Strategy 7 — for target T in equation E1 with unknown U, finds equation E2 that can express U, substitutes into E1, solves the reduced single-variable expression. Two-level elimination handles 3-variable chains. `expand_for_var()` in `expr.h` distributes MUL over ADD/SUB to enable quadratic decomposition of substituted expressions. `flatten_multiplicative()` handles non-numeric denominators (`a / expr`).

**Derive unfolding:** Formula call bodies are inlined into parent expressions when possible, enabling algebraic solving through formula calls. Detects self-referencing calls and falls back to direct sub-system derivation. `FormulaSystem::approximate_mode` (bool, default false) is set by `--approximate`; `format_derived` (system.h) reads it: exact path calls `fmt_exact_double` on collapsed numerics; approximate path runs `substitute_builtin_constants` (expr.h — replaces `pi`/`e`/`phi` Var nodes with their Num values) then re-simplifies so adjacent numerics fold, then stringifies without recognition. `derive_all` dedup: candidates are fingerprinted via `fingerprint_expr` (Schwartz–Zippel numeric evaluation at prime-cycled test points); one canonical form per fingerprint is retained using `canonicity_score` as tiebreaker. Results emitted in ascending `canonicity_score` order — simplest (fewest leaves) first; always-NaN sentinel candidates sort last but are still emitted. `--derive N` (N ≥ 1) caps output at N results after sorting. File-defined constants are injected via `build_alias_table()` so user values like `deg` appear by name in output.

**Derive CSE:** `--cse [N]` (default N=3) extracts AT MOST N helpers from the (canonicalized, capped) winner set, ranked by `value = (occurrences - 1) * (leaves - 1)` — the approximate character savings each helper would yield. Helpers are named `t1, t2, ...` in a `# Helpers` preamble. Two primitives: `cse_extract(exprs, cap, occupied)` (system.h, before the class) counts subtree occurrences, ranks candidates by value, takes the top-N, then re-sorts topologically (smallest first) for nested-helper composition; `cse_replace(e, helpers)` (expr.h) rewrites a tree post-order with a pointer-equality short-circuit so the no-match path costs zero allocations. Pass runs inside `derive_all` BEFORE `format_derived` calls. Pre-canonicalization via `simplify(distribute_over_sum(e))` mirrors the canonicalizer `format_derived` runs internally so structurally-equivalent winners count under the same key — gated to run ONLY when CSE is active (zero-overhead no-CSE path). `output_cap` (= `--derive N`) is applied INSIDE `derive_all` BEFORE the CSE pass so helpers reflect only printed equations. Output round-trips: piping `--cse` output back into fwiz parses and solves correctly (helpers become regular variables in the loaded system). Single-leaf atoms (`t = 2*b`) are never extracted — they have value 0.

**Rewrite rules:** Data-driven simplification via `.fw` patterns. 23 builtin rules (trig symmetry, inverse pairs, abs, log/exp, power rules, division / reciprocal cancellation). Commutative flattened matching handles N-term additive/multiplicative permutations. Rules loaded from `BUILTIN_REWRITE_RULES` string; user `.fw` files can add more.

**Function definitions:** Builtin functions (sin, cos, sqrt, log, abs, etc.) defined as embedded `.fw` sections with `@extern` for C++ evaluation and inverse equations for reverse solving. Custom functions registered via `register_function()` C++ API. Function inversion uses a thread-local callback resolved from `.fw` sub-system definitions.

**Simplifier:** Additive and multiplicative flattening. `rebuild_multiplicative` splits factors by exponent sign: positive exponents → numerator product, negative exponents (sign-flipped) → denominator product, emitting `DIV(num, denom)` when any negative-exp factors exist. Effect: `MUL(a, POW(b, Num(-1)))` renders as `a / b`; `^(-n)` forms never appear in derive output (walker-tested). Structural fractions: `DIV(Num(a), Num(b))` preserved when non-integer, with GCD normalization and exact rational arithmetic (`to_rational()`, `make_rational()`). Most pattern-match rules migrated to `.fw` rewrite rules. Extend flattening logic for structural simplification; add new patterns as `.fw` rules. Structural fractions flow into solve output via `fmt_solve_result` in `main.cpp`. Default (exact) mode: `fmt_exact_double(v)` (fit.h) attempts constant recognition (`pi`, `e`, `phi`, `sqrt(2/3/5)`, `log(2/3/10)`, rational multiples) before falling back to `fmt_num`. `--approximate` mode: always `fmt_num`. The former `is_power_of_10` heuristic has been deleted — `--approximate` is the principled replacement. See Known-Issues #6 for remaining provenance scope (`--steps`/`--calc` traces).

**Two evaluators:** `Checked<double> evaluate(const Expr&)` — numeric projection, collapses tree to a `double`; empty (`!has_value()`) for structural failures (unresolved variable, unknown function, arg-count mismatch, `undefined`, null pointer). Division by zero returns empty via NaN sentinel — not a separate bool. `Checked<T>` (expr.h:30-89) is a NaN-sentinel optional: `sizeof(Checked<double>) == sizeof(double)` (8 bytes vs 16 for `std::optional<double>`); `has_value()` / `operator bool` to test; `.value()` to unwrap (asserts on empty in debug); `.value_or_nan()` is the deliberate boundary escape for handing off to the pure-double numerical root-finder layer — its use is grep-worthy and should stay rare. `ExprPtr evaluate_symbolic(const Expr&)` — exact projection, preserves integer rationals as `DIV(Num, Num)`; used by the simplifier's constant-folding paths. New number types (complex, matrix) extend `evaluate_symbolic`; `evaluate` stays real-valued.

**Pattern matcher:** `match_pattern()` with commutative flattened matching. Variables in patterns are wildcards; builtin constants match literally. Supports N-term additive permutation search and multiplicative coefficient extraction.

**ValueSet:** Unified representation for conditions, ranges, and solutions. Intervals + discrete points + set operations (intersect, union, filter). `covers_reals()` for rewrite rule exhaustiveness checking.

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
`pi`, `e`, `phi` available in any equation. Symbolic in derive, numeric in solve. File defaults override builtins.

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
- `make test && make sanitize && make analyze` must all pass before committing

## Orchestrated Development Workflow

Activate with `claude --agent fwiz-orchestrator` for multi-phase development:

```
USER BRIEF → RESEARCH → DESIGN → IMPLEMENT → REVIEW → PLAN-NEXT → repeat
```

**Agents** (in `.claude/agents/`): orchestrator, researcher, planner, critic, visionary, implementer, debugger, reviewer, doc-updater, perf-auditor, meta-reviewer. Each has focused context and restricted tools — separation of concerns.

**Slash commands** (in `.claude/commands/`): `/debug <reproducer>` spawns the debugger agent against a specific failing case.

**Artifacts** (in `.fwiz-workflow/`, gitignored): research-brief.md, design-proposal.md, implementation-log.md, review-notes.md, next-priorities.md.

**Quality bar**: `make test && make sanitize && make analyze` + periodic data locality / disassembly audits on hot paths.

**Core principle**: least code, least features, maximum flexibility, tiny fast core, infinite extendability via .fw rules.

**Simplification over filtration.** When the output of a stage contains tautological or duplicate items, first ask whether a *simplification rule* upstream would make those items collapse into their canonical siblings — making the duplication invisible to downstream stages — before adding a pruning filter to the output stage. Pruning filters are specializations; simplification rules are generalizations (any structurally-matching expression benefits).

### Recovery protocols

- **3-strike implementer rule**: if the implementer reports BLOCKED three times on the same design, the design is wrong. Do not spawn a fourth attempt — revise the design (mini critic+visionary round on the specific failing hypothesis).
- **Diagnostic round**: after two BLOCKED reports, the next spawn is the `debugger` agent (or the `/debug` command). It instruments, measures, and writes findings — does NOT fix. See `.claude/agents/debugger.md`.
- **Ship-with-followup**: if the cycle has shipped SHIP-BLOCKING tests but has SHIP-DESIRABLE outstanding, close the cycle, log Future.md entries with reopen triggers, and spin a micro-cycle for the follow-up.
- **Measure before design** (hang/perf tasks): research phase MUST include an "Empirical bisection" section — run the reproducer with every orthogonal flag, time each, identify which variants fail the same way. Skip only if the user explicitly says "the hang is in [specific layer]" with authority. Triangle-hang wasted two design rounds this way.
- **Meta-review fires automatically** at end of every cycle, not on user request. See `.claude/agents/fwiz-orchestrator.md` Phase 6.
