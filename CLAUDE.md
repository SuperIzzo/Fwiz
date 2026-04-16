# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

Bidirectional formula solver. Write equations once in `.fw` files, solve for any variable forwards or backwards. Supports conditions, recursion, cross-file formula calls, multiple returns, symbolic derivation, verification, and explore modes. Turing complete via recursive formula calls with conditional base cases.

## Build commands

```bash
make              # build (C++17, GCC 7+ or Clang 5+)
make test         # run all tests (1700+)
make sanitize     # ASan + UBSan
make analyze      # clang-tidy (zero warnings expected)
```

Run: `./bin/fwiz [flags] <file>(<var>=?, <var>=?!, <var>=<value>, <var>=<expr>, ...)`

Flags: `--steps`, `--calc`, `--explore`, `--explore-full`, `--verify all`, `--verify A,B`, `--derive`, `--fit [N]`, `--output FILE`, `--no-numeric`, `--precision N`

## Architecture

Header-only, no external dependencies. Source in `src/`, examples in `examples/`.

**Pipeline:** source → `lexer.h` → `parser.h` → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI). `fit.h` provides curve fitting (sampling, templates, composition).

**Memory:** Arena allocator (`ExprArena`). `ExprPtr` is raw `Expr*`. No shared_ptr. 100% cache-friendly traversal.

**Solver:** `enumerate_candidates()` generates candidates (7 strategies), shared by solve/derive/verify modes. `resolve()` returns first valid result. `resolve_all()` returns `ValueSet` (all solutions or range). `resolve_one()` errors on multiple results. Algebraic solver includes quadratic formula (`decompose_quadratic` detects `ax²+bx+c` form).

**Numeric solver:** Strategy 6 — adaptive grid scan with Newton/bisection refinement. Enabled by default. `try_resolve_numeric()` handles equation-based root-finding and system-probe fallback (for recursive formulas). Re-entrance guard (thread-local set) prevents stack overflow when numeric solver is called recursively. Memoization via `numeric_memo_`. Results classified as exact (`=`) or approximate (`~`) via forward verification.

**Cross-equation elimination:** Strategy 7 — for target T in equation E1 with unknown U, finds equation E2 that can express U, substitutes into E1, solves the reduced single-variable expression. Two-level elimination handles 3-variable chains. `expand_for_var()` in `expr.h` distributes MUL over ADD/SUB to enable quadratic decomposition of substituted expressions. `flatten_multiplicative()` handles non-numeric denominators (`a / expr`).

**Derive unfolding:** Formula call bodies are inlined into parent expressions when possible, enabling algebraic solving through formula calls. Detects self-referencing calls and falls back to direct sub-system derivation.

**Rewrite rules:** Data-driven simplification via `.fw` patterns. 20 builtin rules (trig symmetry, inverse pairs, abs, log/exp, power rules, division). Commutative flattened matching handles N-term additive/multiplicative permutations. Rules loaded from `BUILTIN_REWRITE_RULES` string; user `.fw` files can add more.

**Function definitions:** Builtin functions (sin, cos, sqrt, log, abs, etc.) defined as embedded `.fw` sections with `@extern` for C++ evaluation and inverse equations for reverse solving. Custom functions registered via `register_function()` C++ API. Function inversion uses a thread-local callback resolved from `.fw` sub-system definitions.

**Simplifier:** Additive and multiplicative flattening. Structural fractions: `DIV(Num(a), Num(b))` preserved when non-integer, with GCD normalization and exact rational arithmetic (`to_rational()`, `make_rational()`). Most pattern-match rules migrated to `.fw` rewrite rules. Extend flattening logic for structural simplification; add new patterns as `.fw` rules.

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
- `iff` — bidirectional: condition can be inverted (used in stdlib.fw piecewise functions)
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

Read `DEVELOPER.md` for the full guide. Summary:

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

**Agents** (in `.claude/agents/`): orchestrator, researcher, planner, critic, visionary, implementer, reviewer, doc-updater, perf-auditor. Each has focused context and restricted tools — separation of concerns.

**Artifacts** (in `.fwiz-workflow/`, gitignored): research-brief.md, design-proposal.md, implementation-log.md, review-notes.md, next-priorities.md.

**Quality bar**: `make test && make sanitize && make analyze` + periodic data locality / disassembly audits on hot paths.

**Core principle**: least code, least features, maximum flexibility, tiny fast core, infinite extendability via .fw rules.
