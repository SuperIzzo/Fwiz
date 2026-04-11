# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

Bidirectional formula solver. Write equations once in `.fw` files, solve for any variable forwards or backwards. Supports conditions, recursion, cross-file formula calls, multiple returns, symbolic derivation, verification, and explore modes. Turing complete via recursive formula calls with conditional base cases.

## Build commands

```bash
make              # build (C++17, GCC 7+ or Clang 5+)
make test         # run all tests (1300+)
make sanitize     # ASan + UBSan
make analyze      # clang-tidy (zero warnings expected)
```

Run: `./bin/fwiz [flags] <file>(<var>=?, <var>=?!, <var>=<value>, <var>=<expr>, ...)`

Flags: `--steps`, `--calc`, `--explore`, `--explore-full`, `--verify all`, `--verify A,B`, `--derive`, `--no-numeric`, `--precision N`

## Architecture

Header-only, no external dependencies. Source in `src/`, examples in `examples/`.

**Pipeline:** source → `lexer.h` → `parser.h` → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI)

**Memory:** Arena allocator (`ExprArena`). `ExprPtr` is raw `Expr*`. No shared_ptr. 100% cache-friendly traversal.

**Solver:** `enumerate_candidates()` generates candidates (6 strategies), shared by solve/derive/verify modes. `resolve()` returns first valid result. `resolve_all()` returns `ValueSet` (all solutions or range). `resolve_one()` errors on multiple results.

**Numeric solver:** Strategy 6 — adaptive grid scan with Newton/bisection refinement. Enabled by default. `try_resolve_numeric()` handles equation-based root-finding and system-probe fallback (for recursive formulas). Memoization via `numeric_memo_`. Results classified as exact (`=`) or approximate (`~`) via forward verification.

**Derive unfolding:** Formula call bodies are inlined into parent expressions when possible, enabling algebraic solving through formula calls. Detects self-referencing calls and falls back to direct sub-system derivation.

**Simplifier:** Additive and multiplicative flattening. Extend flattening logic, don't add pattern-match rules.

**ValueSet:** Unified representation for conditions, ranges, and solutions. Intervals + discrete points + set operations (intersect, union, filter).

## Language features

### Conditions
```
y = sqrt(x) : x >= 0           # equation with condition
y = 0 : x < 0                  # piecewise branching
tax = income * 0.1 : income > 0 && income <= 50000  # compound
```
Operators: `>`, `>=`, `<`, `<=`, `=`, `==`, `!=`. Compound: `&&`, `||`.

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

### Multiple returns
- `x=?` — all solutions (returns ValueSet)
- `x=?!` — exactly one solution (errors on multiple)
- `x=?alias` / `x=?!alias` — with alias
- CLI values can be expressions: `width=2^3, height=sqrt(9)`

### Recursion
```
result = 1 : n <= 0
result = n * factorial(result=?prev, n=n-1) : n > 0
```
Depth guard: `max_formula_depth` (default 1000).

### Numeric solving
Enabled by default. Nonlinear equations (quadratics, transcendentals, recursive inverses) solved via adaptive grid scan + Newton/bisection. Exact results use `=`, approximate use `~`.
- `--no-numeric` — algebraic only
- `--precision N` — scan density (default 200)
- Conditions narrow the search range: `x > 0` scans only positive values
- Constants: `NUMERIC_DEFAULT_SAMPLES`, `NUMERIC_TOLERANCE`, `NUMERIC_SEED`

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
