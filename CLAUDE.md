# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

Bidirectional formula solver. Write equations once in `.fw` files, solve for any variable forwards or backwards. Supports cross-file formula calls, symbolic derivation, verification, and explore modes.

## Build commands

```bash
make              # build (C++17, GCC 7+ or Clang 5+)
make test         # run all tests (1129+)
make sanitize     # ASan + UBSan
make analyze      # clang-tidy (zero warnings expected)
```

Run: `./bin/fwiz [--steps|--calc|--explore|--explore-full|--verify all|--derive] <file>(<var>=?, <var>=<value>, ...)`

## Architecture

Header-only, no external dependencies. Source in `src/`, examples in `examples/`.

**Pipeline:** source → `lexer.h` → `parser.h` → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI)

**Memory:** Arena allocator (`ExprArena`). `ExprPtr` is raw `Expr*`. No shared_ptr. 100% cache-friendly traversal.

**Solver:** `enumerate_candidates()` generates candidates, shared by solve/derive/verify modes. Strategies: direct LHS, algebraic inversion, forward formula call, shared-LHS equating, reverse formula call.

**Simplifier:** Additive and multiplicative flattening (not case-by-case rules). Extend flattening logic, don't add new pattern-match rules.

## Key conventions

Read `DEVELOPER.md` for the full conventions guide. Summary:

- **References for non-null** (`const Expr&`), **pointers for nullable** (`ExprPtr`)
- **`constexpr`** for predicates and constants, **`inline`** for everything else in headers
- **Named constants** (`EPSILON_ZERO`, `EPSILON_REL`, `SIMPLIFY_MAX_ITER`) — no magic numbers
- **Data-driven** — BinOp metadata table, builtin function registry, strategy enumeration
- **Function pointers** over `std::function`
- **Enums** use `uint8_t` base
- **No empty catch blocks** — return, log, or handle
- **Write failing tests first**, commit tests before refactoring
- `make test && make sanitize && make analyze` must all pass before committing
