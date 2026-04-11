# Developer Guide

## Vision

fwiz is a formula language built on one idea: **you shouldn't have to tell a computer which direction to solve an equation**.

Traditional programming languages are imperative — you write `y = x + 5` and that's an assignment. To go the other way, you write a different function. fwiz treats equations as mathematical truths: write `y = x + 5` once, and the system derives `x = y - 5` automatically.

The long-term vision is a language where you declare mathematical relationships and the system handles all the algebraic manipulation, substitution, and resolution. Think of it as functional programming taken to its logical extreme — instead of defining functions that transform inputs to outputs, you define equations that relate variables, and query whichever one you need.

### Design principles

- **Equations, not assignments.** Every line declares a relationship, not a computation direction.
- **Bidirectional by default.** Any variable in any equation can be the solve target.
- **Human-readable files.** `.fw` files should look like maths on a whiteboard.
- **Fail clearly.** When the solver can't find an answer, say why — not just "error".
- **Robustness over features.** A wrong answer is worse than no answer. Reject NaN, infinity, and near-zero floating point artifacts.

### Roadmap

Current capabilities:
- Linear algebraic solving (variable in additions, subtractions, multiplied/divided by constants)
- Multi-equation substitution via shared variables
- Equation chains with recursive resolution
- Like-term combining (`y + 3*y → 4y`)
- Built-in math functions (sqrt, sin, cos, tan, log, abs)
- Default values
- Step-by-step trace output

Planned:
- **Cross-file imports** — call formulas from other `.fw` files
- **Quadratic solving** — equations where the variable appears squared
- **Inverse functions** — solving `y = sin(x)` for `x` via `asin`
- **Multi-solution results** — returning both roots of a quadratic
- **Units** — dimensional analysis and automatic conversion
- **Interactive REPL** — define equations and query interactively

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                    main.cpp                          │
│            CLI parsing, flag handling                │
├──────────────────────────────────────────────────────┤
│                   system.h                           │
│     FormulaSystem: file loading, equation            │
│     storage, recursive solving, trace output         │
├────────────────────────┬─────────────────────────────┤
│      parser.h          │         expr.h              │
│   Token stream →       │   Expression tree:          │
│   expression tree      │   simplify, evaluate,       │
│                        │   substitute, solve_for,    │
│                        │   decompose_linear          │
├────────────────────────┼─────────────────────────────┤
│      lexer.h           │         trace.h             │
│   Source text →        │   Trace levels:             │
│   token stream         │   NONE, STEPS, CALC         │
└────────────────────────┴─────────────────────────────┘
```

All headers, no `.cpp` files except `main.cpp` and `tests.cpp`. The codebase is intentionally compact — under 1000 lines of implementation.

### lexer.h

Converts source text into tokens: `NUMBER`, `IDENT`, `PLUS`, `MINUS`, `STAR`, `SLASH`, `CARET`, `LPAREN`, `RPAREN`, `EQUALS`, `QUESTION`, `COMMA`, `END`.

- Handles: integers, floats (including leading dot `.5`), identifiers with underscores/digits
- Rejects: all non-mathematical characters with `"Unexpected character: X"` errors
- Does NOT handle: scientific notation in formula files (that's a lexer limitation), newlines (file parser splits lines first)

### parser.h

Recursive descent parser. Converts token stream to expression tree.

Precedence (highest to lowest):
1. Atoms: numbers, variables, function calls, parenthesized expressions
2. Unary minus: `-x`
3. Power: `x^2`
4. Multiplicative: `x * y`, `x / y`
5. Additive: `x + y`, `x - y`

Power is currently NOT right-associative — `x^2^3` parses as `x^2` with trailing tokens. This is a known limitation.

### expr.h

The core of the system. Contains:

**ExprPtr** — shared pointer to expression tree nodes. Types: `NUM`, `VAR`, `BINOP`, `UNARY_NEG`, `FUNC_CALL`.

**collect_vars()** — Collects all variable names in a tree into a set. Used by the solver to find what needs resolving.

**contains_var()** — Direct recursive search for a variable. Returns at first hit with no allocation — unlike `collect_vars`, this doesn't build a set.

**expr_equal()** — Structural equality test with pointer shortcut. Used by the simplifier fixpoint loop — zero allocation per iteration, compared to the naive approach of converting both trees to strings and comparing.

**expr_to_string()** — Pretty printer with precedence-aware parenthesization. Only adds parens where needed for correctness.

**evaluate()** — Evaluates a fully numeric expression tree. Throws on unresolved variables or division by zero. Built-in functions are dispatched via a static lookup table.

**substitute()** — Replaces a named variable with an expression throughout the tree.

**simplify()** — Algebraic simplification, run to fixpoint (max 20 iterations, checked via `expr_equal`). Rules:
- Constant folding: `2 + 3 → 5`
- Constant reassociation: `(x + 2) + 3 → x + 5` (handles ADD±ADD, ADD±SUB, SUB±ADD, SUB±SUB, MUL×MUL in one unified block)
- Identity removal: `x + 0 → x`, `x * 1 → x`, `x^1 → x`
- Zero absorption: `x * 0 → 0`, `0/x → 0`
- Negation cancellation: `--x → x`, `x - (-y) → x + y`, `-(a - b) → b - a`
- Negation factoring via `simplify_neg_pair()`: handles `(-a)⊗(-b) → a⊗b`, `(-a)⊗b → -(a⊗b)`, `a⊗(-b) → -(a⊗b)` for both MUL and DIV in a single shared function

**decompose_linear()** — The key insight for solving. Decomposes an expression into `coeff * target + rest` where `coeff` and `rest` are free of the target variable. This works by walking the expression tree:
- `VAR(target)` → coeff=1, rest=0
- `VAR(other)` → coeff=0, rest=other
- `NUM(n)` → coeff=0, rest=n
- `ADD(l, r)` → coeff=l.coeff+r.coeff, rest=l.rest+r.rest
- `MUL(l, r)` where only one side has target → coeff=other*inner.coeff, rest=other*inner.rest
- Returns `ok=false` for nonlinear cases (target in both sides of multiply, target in denominator, target inside function calls, target in exponent)

**solve_for()** — Solves `lhs = rhs` for a target variable:
1. Compute `combined = lhs - rhs`
2. Decompose into `coeff * target + rest = 0`
3. Return `target = -rest / coeff`
4. Returns `nullptr` if nonlinear or coefficient is zero/near-zero

Near-zero coefficient guard: if `|coeff| < 1e-12`, returns nullptr. This prevents floating point artifacts like `0.1 + 0.2 - 0.3 ≈ 5.5e-17` from producing wildly wrong answers.

### system.h

**FormulaSystem** — Holds equations, defaults, and the solving logic.

`load_file()`:
- Strips UTF-8 BOM from first line
- Handles CRLF, LF, and mixed line endings
- Skips blank lines and `#` comments
- Wraps each line parse in try/catch — bad lines are skipped, not fatal
- Distinguishes defaults (bare numbers) from equations (expressions)

`resolve()`:
- Takes the target variable name and a bindings map (by value — caller is never mutated)
- Applies defaults for non-target variables
- Calls `solve_recursive()`

`solve_recursive()` — The resolver. Uses a `try_expr` lambda to unify the three strategies:
1. **Direct**: target is on the LHS of an equation → evaluate the RHS
2. **Invert**: target appears in an equation's RHS → algebraically isolate it using `solve_for()`
3. **Substitute**: two equations share a LHS variable → equate their RHS sides and solve both directions

For each strategy, if sub-variables are unknown, it recurses to solve them first. The `visited` set prevents infinite recursion on circular dependencies.

Results are validated — NaN and infinity are rejected, causing the solver to try alternative equations. This means if one equation path produces `sqrt(-1)` but another gives a valid answer, the valid one is used.

Error messages are specific: "No equation found for 'x'", "no value for 'y'", "all equations produced invalid results".

### trace.h

Three levels:
- `NONE` — no output (default)
- `STEPS` — algebraic reasoning: which equations are tried, inversions, substitutions, results
- `CALC` — steps plus numeric detail: each variable substitution and the expression before evaluation

All trace output goes to stderr. Controlled by `--steps` and `--calc` flags.

---

## Testing

678 tests organized into functional tests, edge cases, and robustness groups:

```bash
make test
```

### Test structure

All tests are in `src/tests.cpp` with a minimal assertion framework (no external dependencies). Tests are organized into sections:

**Functional tests** — core behavior of each component:
- Lexer, Parser, Evaluate, Simplifier, Substitute, Variable helpers
- Linear decomposition, Algebraic solver
- Full system (forward, inverse, multi-equation, defaults, chains)
- CLI parser, File parsing

**Edge cases** — boundary behavior:
- Lexer: special chars, long inputs, leading dots
- Parser: deep nesting, unary minus positions, mismatched parens
- Evaluate: NaN, inf, division by zero, negative exponents
- Simplifier: negation chains, constant reassociation, zero absorption
- Decomposition: zero coefficients, symbolic coefficients, nonlinear rejection
- Solver: zero coefficient, identity equations, fractional coefficients

**Robustness groups** (7 groups):
1. **Numeric extremes** — inf/NaN propagation, near-zero float coefficients, output formatting for extreme values
2. **Expression depth & scale** — trees up to depth 10000, wide expressions with 1000 variables, 500-equation chain resolution, parsing deep strings
3. **Contradictions & overdetermined** — equation ordering, circular dependencies, NaN fallthrough to alternatives, defaults vs equations
4. **Statefulness** — load_file accumulation, resolve isolation, caller bindings not mutated, system reuse patterns
5. **File format portability** — CRLF, mixed endings, UTF-8 BOM, trailing whitespace, no trailing newline, indentation
6. **CLI value parsing** — scientific notation, negative values, multiple query targets, inf/nan rejection, long queries, spacing variants
7. **Error message quality** — specific messages for missing variables, NaN/inf, circular deps, file errors, CLI errors

### Bugs found through testing

Testing uncovered and fixed 7 bugs:
1. `solve_for` crashed on zero coefficient (division by zero in simplifier)
2. File parser crashed on malformed lines instead of skipping them
3. Opening a directory as a `.fw` file silently succeeded
4. UTF-8 BOM at file start silently ate the first line
5. NaN/infinity results accepted as valid solver output
6. Near-zero floating point coefficients (`0.1 + 0.2 - 0.3`) produced wildly wrong answers
7. All solver errors produced the same generic "Cannot solve" message

---

## Memory Safety

fwiz uses three compiler sanitizers to verify memory safety. No external tools (like Valgrind) are needed — everything is built into GCC and Clang.

### Running sanitizer checks

```bash
# Run all sanitizers
make sanitize

# Or individually:
make asan     # AddressSanitizer + LeakSanitizer
make ubsan    # UndefinedBehaviorSanitizer
```

All 678 tests pass clean under every sanitizer — no leaks, no undefined behavior, no memory errors.

### What each sanitizer catches

**AddressSanitizer (ASan)** — compiled with `-fsanitize=address`:
- Heap buffer overflow (reading/writing past allocation bounds)
- Stack buffer overflow
- Use-after-free (accessing memory after `delete`/`free`)
- Use-after-return (accessing stack memory after function returns)
- Double-free
- Memory leaks (via the bundled LeakSanitizer, enabled with `ASAN_OPTIONS=detect_leaks=1`)

**UndefinedBehaviorSanitizer (UBSan)** — compiled with `-fsanitize=undefined`:
- Signed integer overflow (e.g. `INT_MAX + 1`)
- Null pointer dereference
- Division by zero (integer)
- Misaligned pointer access
- Shift overflow (shifting by more than the type width)
- Invalid enum/bool values

### Why fwiz passes clean

The architecture makes several classes of bugs structurally impossible:

**No manual memory management.** All expression trees use `shared_ptr<Expr>` (`ExprPtr`). There is no `new`/`delete`, no raw pointer ownership, and no manual `free()`. Reference counting handles cleanup automatically, so use-after-free and double-free are impossible by construction.

**No reference cycles.** Expression trees are DAGs (directed acyclic graphs) — children never point back to parents. This means `shared_ptr` reference counts always reach zero, and LeakSanitizer confirms nothing leaks.

**No buffer arithmetic.** The code uses `std::string` and `std::vector` instead of raw char arrays or pointer arithmetic. Bounds are checked by the standard library in debug mode.

**Guarded casts.** The `fmt_num` function casts `double` to `long long` for display, but only when `abs(v) < 1e12` — well within `long long` range. UBSan confirms this never overflows.

**Division by zero handled in two places.** The evaluator throws on `x / 0`. The solver checks for zero/near-zero coefficients before dividing. UBSan confirms no unchecked division reaches the hardware.

### Sanitizer-aware test depths

ASan adds ~200 bytes of red zones per stack frame, which reduces the effective stack depth. The tests auto-detect sanitizers at compile time:

```cpp
#if defined(__SANITIZE_ADDRESS__)       // GCC
    constexpr int DEPTH_HIGH = 500;
    constexpr int DEPTH_MED = 200;
#else
    constexpr int DEPTH_HIGH = 10000;
    constexpr int DEPTH_MED = 5000;
#endif
```

This means depth-stress tests use 500/200 under sanitizers instead of 10000/5000 normally. All assertions use these constants, so the tests remain correct at both scales. The reduced depths still exercise the same code paths — they just don't push to the stack limit.

### Adding sanitizer checks to CI

For continuous integration, run `make sanitize` as part of the build pipeline. It compiles and runs the full test suite twice (once with ASan, once with UBSan). If either detects a problem, it prints a diagnostic with a stack trace and exits non-zero.

```yaml
# Example CI step
- name: Sanitizer checks
  run: make sanitize
```

### When to run sanitizers

- **Always** after adding new code that allocates memory or does arithmetic
- **Always** after modifying the expression tree, simplifier, or solver
- **Before any release** — `make sanitize` should be part of the release checklist
- **When debugging crashes** — ASan gives precise stack traces for memory errors

Sanitizer builds are ~3-5x slower than optimized builds due to instrumentation, but for fwiz's test suite this means seconds rather than milliseconds — not a practical concern.

---

## Known Limitations

### Stack depth
Recursive tree operations (simplify, evaluate, substitute) will stack overflow on extremely deep expression trees. Approximate limits with default 8MB stack:
- `simplify`: ~25,000 depth (amplified by 20x fixpoint iterations)
- `evaluate`/`substitute`: ~100,000 depth

This is not a practical concern for human-written formulas.

### Floating point
All values are IEEE 754 doubles. Precision is approximately 15-16 significant digits. The near-zero coefficient guard (`|coeff| < 1e-12`) prevents the worst float artifacts but means very small genuine coefficients (below 1e-12) will be treated as zero.

### Equation ordering
When multiple equations can solve for the same variable, the first one in file order wins. Contradictory equations are resolved silently — no warning is issued.

### Bare carriage returns
Classic Mac line endings (bare `\r` without `\n`) are not supported as line separators. This is an extremely rare format.

### Power associativity
`x^2^3` is parsed as `(x^2)` with `^3` left unparsed, rather than the mathematical convention of right-associative power.

---

## Contributing

### Adding a new built-in function

1. Add to the `builtin_functions()` registry in `expr.h` — it's a `std::map<std::string, double(*)(double)>`
2. Consider whether it needs an inverse for `decompose_linear` (most functions make expressions nonlinear)
3. Add tests in `tests.cpp`

### Adding a solver strategy

1. Add the strategy to `enumerate_candidates()` in `system.h`
2. Handle the new `CandidateType` in the callbacks of `solve_recursive`, `derive_recursive`, and `verify_variable`
3. Add strategy-specific tests

### Extending the simplifier

The simplifier uses **flattening** rather than case-by-case pattern matching:
- **Additive**: `flatten_additive()` decomposes ADD/SUB chains into `(coefficient, base)` term lists. `group_additive()` combines like terms. `rebuild_additive()` reconstructs.
- **Multiplicative**: `flatten_multiplicative()` decomposes MUL chains into `(base, exponent)` factor lists. `group_multiplicative()` combines matching bases. `rebuild_multiplicative()` reconstructs.
- **Division**: handled with targeted rules (not flattened) to preserve readable forms like `x / 2`. Cross-term cancellation via flatten-both-sides.

To add a new simplification: check if it can be handled by extending the flattening logic before adding a new case-by-case rule.

---

## Conventions

### Code style

- Header-only implementation (except `main.cpp` and `tests.cpp`)
- No external dependencies — stdlib only
- All enums use `uint8_t` base type to minimize struct sizes
- No magic numbers — use named constants (`EPSILON_ZERO`, `EPSILON_REL`, `SIMPLIFY_MAX_ITER`)

### Memory model

Expression nodes are allocated from an **arena allocator** (`ExprArena`), not individually heap-allocated. `ExprPtr` is a raw `Expr*` — no `shared_ptr`, no reference counting.

- The arena is owned by `FormulaSystem` and scoped via `ExprArena::Scope` during operations
- Nodes are never individually freed — the arena is cleared in bulk when destroyed
- Thread-local `ExprArena::current()` provides access to the active arena
- This gives 100% cache-friendly traversal and eliminates shared_ptr overhead

### Parameter passing

- **`const Expr&`** — for functions that always expect a valid expression (tree queries, evaluate, predicates). No null check needed inside the function.
- **`ExprPtr` (`Expr*`)** — for return types, struct fields, and functions that may return or accept nullptr (substitute, simplify, solve_for).
- **Pointer overloads** — thin null-checking wrappers that dereference and delegate to the reference version.

### constexpr and inline

- **`constexpr`** — type predicates (`is_num`, `is_zero`, etc.), enum queries (`is_additive`), compile-time constants
- **`inline`** — everything else in headers (required for ODR in header-only code)
- Prefer function pointers over `std::function` to avoid heap allocation

### Data-driven design

Prefer data tables and registries over switch statements and if-else chains:

- **BinOp metadata** — `binop_info()` returns symbol, precedence, and eval function from a single table. Don't add per-operator switches.
- **Builtin functions** — `builtin_functions()` returns a `std::map`. Add new functions there.
- **Solver strategies** — `enumerate_candidates()` generates candidates for all solver modes. Add new strategies there.

### Error handling

- Empty `catch` blocks are not allowed (flagged by clang-tidy). Use `return false;`, `return;`, or add trace logging.
- Solver strategy failures are expected — catch and try the next strategy. Only throw when all strategies have been exhausted.
- Validate results: reject NaN, infinity, and near-zero coefficients (`|coeff| < EPSILON_ZERO`).

### Testing strategy

- **Write failing tests first** — prove the bug before fixing it, define the requirement before implementing it
- **Commit tests separately** before refactoring so you can revert safely
- **Semantic tests for output flexibility** — when simplifier output order may vary, test by evaluating with specific values rather than string comparison
- **Accept either ordering** for commutative operations: `ASSERT(r == "x * y" || r == "y * x", ...)`
- **Run the full pipeline** before committing: `make test && make sanitize && make analyze`

### Test organization

Tests are grouped by concern, not by code:
1. Functional tests (core behavior)
2. Edge cases (boundary conditions)
3. Robustness (garbage input, numeric extremes, scale)
4. Statefulness (isolation, mutation, reuse)
5. File format portability (line endings, BOM, whitespace)
6. CLI parsing and integration
7. Error message quality
8. Feature-specific (formula calls, verify, explore, derive)
9. Pre-refactor safety nets (strategy coverage, builtin exhaustive)
10. Simplifier improvements (rule interactions, flattening targets)
