# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is fwiz

fwiz is a bidirectional formula solver. You write equations once in `.fw` files, and fwiz algebraically solves for any variable — forwards or backwards. It handles linear equations, multi-equation substitution, equation chains, and like-term combining.

## Build and test commands

```bash
make              # build fwiz binary (requires C++17, GCC 7+ or Clang 5+)
make test         # build + run all functional tests (678 tests)
make sanitize     # run ASan + UBSan sanitizer checks
make asan         # AddressSanitizer only
make ubsan        # UndefinedBehaviorSanitizer only
make clean        # remove build artifacts
```

Run the binary: `./bin/fwiz <file>(<var>=?, <var>=<value>, ...)`

## Architecture

Header-only implementation with no external dependencies (stdlib only). All expression trees use `shared_ptr<Expr>` (`ExprPtr`) — no manual memory management. Source files live in `src/`, example `.fw` files in `examples/`.

**Pipeline:** source text → `lexer.h` (tokens) → `parser.h` (expression tree) → `expr.h` (simplify/evaluate/solve) → `system.h` (multi-equation resolution) → `main.cpp` (CLI)

Key modules:
- **lexer.h** — Tokenizer. Does not handle scientific notation or newlines (file parser splits lines first).
- **parser.h** — Recursive descent parser. Known limitation: `^` is not right-associative.
- **expr.h** — Core algebra engine. `decompose_linear()` is the key function: decomposes expressions into `coeff * target + rest`. `solve_for()` uses this to isolate variables. `simplify()` runs to fixpoint (max 20 iterations).
- **system.h** — `FormulaSystem` class. Loads `.fw` files, stores equations/defaults, runs recursive solving with three strategies: direct evaluation, algebraic inversion, and substitution via shared variables. Results validated against NaN/infinity.
- **trace.h** — Three trace levels (NONE, STEPS, CALC). Output goes to stderr.
- **main.cpp** — CLI parsing and flag handling.
- **tests.cpp** — All tests with a minimal built-in assertion framework (no test library).

## Design principles

- Equations are bidirectional relationships, not assignments
- Fail clearly with specific error messages — never silently produce wrong answers
- Reject NaN, infinity, and near-zero float artifacts (coefficient guard: `|coeff| < 1e-12`)
- First valid equation in file order wins; invalid results (NaN/inf) trigger fallback to next equation

## Adding a built-in function

1. Add evaluation dispatch in `evaluate()` in `expr.h`
2. Consider nonlinearity — most functions make `decompose_linear()` return `ok=false`
3. Add tests in `tests.cpp`

## Extending the solver

Add cases to `decompose_linear()` in `expr.h` for new linear forms. For nonlinear solving (e.g., quadratics), add a new strategy to `solve_recursive()` in `system.h`.

## Test structure

Tests in `tests.cpp` are grouped by failure mode: functional → edge cases → robustness (numeric extremes, expression depth, contradictions, statefulness, file format portability, CLI parsing, error messages). Sanitizer-aware depth constants reduce recursion limits under ASan. Always run `make sanitize` after modifying expression tree, simplifier, or solver code.
