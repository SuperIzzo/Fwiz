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
- Linear and quadratic algebraic solving (quadratic formula for `ax²+bx+c=0`)
- Multi-equation substitution via shared variables
- Equation chains with recursive resolution
- Like-term combining (`y + 3*y → 4y`) via additive/multiplicative flattening
- Built-in math functions (sqrt, sin, cos, tan, log, abs, asin, acos, atan) — defined in `.fw` with `@extern` + inverse equations
- Custom C++ function registration via `register_function()` API
- Default values
- Step-by-step trace output (`--steps`, `--calc`)
- Cross-file formula calls with explicit or positional argument binding
- Positional args: `sin(3)` maps to `sin(x=3, result=?)` via `[sin(x) -> result]`
- Expression bindings in formula calls (`n=n-1`)
- Conditions on equations (`if x >= 0`, `iff x > 0`, compound `&&`/`||`)
- Global conditions (standalone `x > 0` lines)
- Conditional branching (piecewise functions via conditions + equation ordering)
- Recursion (self-referencing formula calls with conditional base cases)
- Multiple returns (`?` = all solutions, `?!` = strict one)
- ValueSet returns (ranges when exact values unavailable)
- Symbolic derivation (`--derive`) with formula call unfolding
- Verification (`--verify`)
- Explore mode (`--explore`, `--explore-full`)
- CLI expression values (`width=2^3, height=sqrt(9)`)
- Inline comments (`# after equations`), semicolons as line separators
- Data-driven rewrite rules (20 builtin `.fw` patterns for simplification)
- Commutative pattern matching (N-term additive/multiplicative permutation search)
- `undefined` keyword for explicit domain boundaries and exhaustiveness checking
- Context-aware simplification (conditions checked against known numeric bindings)
- Section headers with return-var sugar: `[f(x) -> result] = x^2`
- Arena allocator for expression nodes (100% cache-friendly)
- Numeric solving (adaptive grid scan + Newton/bisection, enabled by default)
- Exact/approximate result classification (`=` vs `~`)
- Curve fitting (`--fit`) with template matching and recursive composition
- Built-in constants (`pi`, `e`, `phi`)
- Irrational number recognition (pi, e, sqrt(2), sqrt(3) in fitted coefficients)
- Structural fractions (`1/3` preserved, not folded to `0.333...`; exact rational arithmetic)
- Constant recognition in derive output (log(2), log(3), sqrt(N), pi, e)
- Output formatting: `--approximate` (collapse to float) / `--exact` (default, human-readable fractions and constants); `fmt_exact_double` shared helper closes solve/derive asymmetry

Planned (see Future.md):
- **Symbolic differentiation** — sensitivity analysis
- **Batch/table mode** — parameter sweeps with range syntax
- **Units** — dimensional analysis and automatic conversion
- **Fraction representation** — exact arithmetic
- **LaTeX export**
- **Standard library** — curated `.fw` files
- **Interactive REPL**

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
│                        │   decompose_linear,         │
│                        │   numeric root-finding,     │
│                        │   builtin constants         │
├────────────────────────┤                             │
│      fit.h             │                             │
│   Curve fitting:       │                             │
│   sampling, templates, │                             │
│   composition,         │                             │
│   constant recognition │                             │
├────────────────────────┼─────────────────────────────┤
│      lexer.h           │         trace.h             │
│   Source text →        │   Trace levels:             │
│   token stream         │   NONE, STEPS, CALC         │
└────────────────────────┴─────────────────────────────┘
```

All headers, no `.cpp` files except `main.cpp` and `tests.cpp`. ~15000 lines total including tests.

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

**ExprPtr** — raw pointer (`Expr*`) to arena-allocated expression tree nodes. Types: `NUM`, `VAR`, `BINOP`, `UNARY_NEG`, `FUNC_CALL`. All nodes allocated from `ExprArena` (contiguous chunks, 100% cache-friendly).

**ValueSet** — unified representation for conditions, ranges, and solutions. Intervals (open/closed, half-infinite) + discrete points. Operations: intersect, union, filter, contains. Returned by `resolve_all()`.

**collect_vars()** — Collects all variable names in a tree into a set. Used by the solver to find what needs resolving.

**contains_var()** — Direct recursive search for a variable. Returns at first hit with no allocation — unlike `collect_vars`, this doesn't build a set.

**expr_equal()** — Structural equality test with pointer shortcut. Used by the simplifier fixpoint loop — zero allocation per iteration, compared to the naive approach of converting both trees to strings and comparing.

**expr_to_string()** — Pretty printer with precedence-aware parenthesization. Only adds parens where needed for correctness.

**evaluate()** — Evaluates a fully numeric expression tree. Returns `Checked<double>`: empty (`!has_value()`) for structural failures (unresolved variable, unknown function, arg-count mismatch, `undefined` sentinel, null pointer). Division by zero also yields empty, via NaN sentinel — not a separate case. Built-in functions are dispatched via a static lookup table. Stays real-valued permanently — do not extend for complex or matrix types.

`Checked<T>` (expr.h:30-89) makes check discipline type-enforced rather than convention-enforced — the complement to the "exceptions for exceptional cases only" principle. `sizeof(Checked<double>) == sizeof(double)`; no hidden bool. Test with `has_value()` / `operator bool`; unwrap with `.value()` (asserts on empty in debug). `.value_or_nan()` is the named boundary escape for handing `double` off to the pure-numeric root-finder layer (`find_numeric_roots`, `adaptive_scan`, `newton_solve`, `bisection_solve`) which has its own `isfinite` discipline — its use should stay rare and grep-worthy. The `Checked(T v)` constructor is deliberately NOT `explicit`: `return some_double;` in `Checked<double>`-returning functions is load-bearing throughout the evaluate paths. Three markers at the declaration document this intent: `// cppcheck-suppress noExplicitConstructor` silences cppcheck, `/*implicit*/` is the human-facing signal, and trailing `// NOLINT(google-explicit-constructor)` silences clang-tidy. All three are load-bearing — remove any one and `make analyze` will fire.

**evaluate_symbolic()** — Exact sibling of `evaluate()`. Returns an `ExprPtr` that preserves non-real structure (currently: integer rationals as `DIV(Num, Num)`). Used by the simplifier's constant-folding paths (`simplify_once_impl` BINOP num/num and FUNC_CALL all-numeric folds). This is the extension point for new number types — add complex or matrix dispatch here, not in `evaluate()`.

**fingerprint_expr(ExprPtr, free_vars, test_points)** — Schwartz–Zippel numeric fingerprint for semantic comparison. Substitutes free variables at test points and collects finite `evaluate()` outputs. Companion to `evaluate` and `evaluate_symbolic` as a tree-querying primitive. Used by `derive_all` dedup.

**canonicity_score(ExprPtr)** — Lex pair `{leaf_count, non_integer_num_count}` measuring expression complexity. Lower is simpler/more canonical. `leaf_count` is the primary key (size first); `non_integer_num_count` is the secondary tiebreaker (penalizes raw decimal literals). Integer `NUM` leaves are not penalized on the secondary key. Used by `derive_all` to sort output ascending — simplest formulas first — and to break ties when two candidates share a fingerprint.

**substitute()** — Replaces a named variable with an expression throughout the tree.

**substitute_builtin_constants()** — Tree walk; replaces Var nodes whose names appear in `builtin_constants()` (`pi`, `e`, `phi`) with their Num values. Used by the `--approximate` derive path before re-simplification. Other Var nodes pass through unchanged.

**simplify()** — Algebraic simplification, run to fixpoint (max 20 iterations, checked via `expr_equal`). Rules:
- Constant folding: `2 + 3 → 5`
- Constant reassociation: `(x + 2) + 3 → x + 5` (handles ADD±ADD, ADD±SUB, SUB±ADD, SUB±SUB, MUL×MUL in one unified block)
- Identity removal: `x + 0 → x`, `x * 1 → x`, `x^1 → x`
- Zero absorption: `x * 0 → 0`, `0/x → 0`
- Negation cancellation: `--x → x`, `x - (-y) → x + y`, `-(a - b) → b - a`
- Negation factoring via `simplify_neg_pair()`: handles `(-a)⊗(-b) → a⊗b`, `(-a)⊗b → -(a⊗b)`, `a⊗(-b) → -(a⊗b)` for both MUL and DIV in a single shared function
- Structural fractions: `Num(a) / Num(b)` preserved as `DIV(Num(a), Num(b))` when result is non-integer; GCD-normalized, sign in numerator. Rational arithmetic via `to_rational()` and `make_rational()` helpers. `flatten_multiplicative()` treats structural fractions as opaque factors.

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

**FormulaSystem** — Holds equations, defaults, formula calls, global conditions, and the solving logic.

`load_file()`:
- Strips UTF-8 BOM from first line
- Handles CRLF, LF, and mixed line endings
- Strips inline `#` comments (respecting parentheses)
- Skips blank lines and full-line `#` comments
- Parses conditions (`: expr op expr`) on equations
- Detects global conditions (standalone `x > 0` lines)
- Extracts formula calls from token stream before expression parsing
- Distinguishes defaults (bare numbers without conditions) from equations

`resolve()` / `resolve_all()` / `resolve_one()`:
- `resolve()` returns first valid result (for internal use)
- `resolve_all()` returns `ValueSet` — all solutions or range constraints
- `resolve_one()` errors on multiple results (`?!` mode)

`enumerate_candidates()` — shared strategy loop for solve/derive/verify:
1. **Direct**: target on LHS → evaluate RHS
2. **Invert**: target in RHS → algebraically isolate
3. **Forward formula call**: target is formula call output_var
4. **Substitute**: two equations share LHS → equate RHS
5. **Reverse formula call**: target maps through a binding
6. **Numeric**: adaptive grid scan + Newton/bisection refinement
7. **Cross-equation elimination**: for target T in equation E1 with unknown U, find E2 expressing U, substitute into E1, solve the reduced expression. Two-level elimination handles 3-variable chains (e.g. `p=xy, q=yz, r=xz`). `expand_for_var()` in `expr.h` distributes MUL over ADD/SUB to enable quadratic decomposition of substituted results.

Conditions are checked before solving (if vars known) and after (to validate). Global conditions checked after every result. Formula call depth tracked via thread-local counter with configurable max (default 1000).

`mutable bool approximate_mode` on `FormulaSystem` (mirrors `--approximate` CLI flag). `format_derived` reads it: exact path uses `fmt_exact_double` (fit.h) on collapsed-numeric branches; approximate path runs `substitute_builtin_constants` (expr.h) — a tree walk replacing `pi`/`e`/`phi` Var nodes with their Num values — then re-simplifies so adjacent numerics fold.

`std::string source_label_` on `FormulaSystem` — set to the file stem on `load_file` and to the explicit label on `load_string`. `build_alias_table()` walks `this->defaults` and each sub-system's `defaults`, groups constants by name, emits them unqualified when all files agree on the value (within `EPSILON_REL`), and emits `stem.name` qualified forms when the same name carries different values across files. Built-in constants (`pi`, `e`, `phi`) are never entered into the user alias table. `fmt_solve_result` (main.cpp) and `format_derived` (system.h) both thread the table into `fmt_exact_double`.

`derive_all` dedup pipeline — after collecting raw candidates, a streaming `std::map<fp_key, {score, ExprPtr}> winners` retains at most one candidate per semantic fingerprint. Two semantic primitives in `expr.h` drive this:

- **`fingerprint_expr(ExprPtr, free_vars, test_points)`** — Schwartz–Zippel numeric fingerprint: substitutes all free variables at each test point, calls `evaluate`, collects finite values; returns an empty vector when all test points lie outside the expression's domain. Test points use per-variable prime cycling `primes[(i+j)%3]` with `primes={2,3,5}`, keeping magnitudes small enough to avoid triangle-inequality violations.
- **`canonicity_score(ExprPtr)`** — lex pair `{leaf_count, non_integer_num_count}`. Integer `NUM` leaves are not penalized on the secondary key, so `2*pi` scores the same as `pi`. Lower score wins; ties are broken in favour of the form already in `winners`.

Candidates with empty fingerprints (all test points domain-excluded) fall back to a format-string sentinel: structurally-different always-NaN expressions stay separate; string-identical ones collapse. Sentinel-bucket candidates sort after real-fingerprint candidates because their discriminator byte is `1` vs `0` for real fingerprints — `std::map` ordering ensures real results appear first without any filtering.

The `derive_all` emit loop sorts all winners ascending by `canonicity_score` before output, so the simplest formula is always first. `--derive N` (N ≥ 1) caps the final result list at N entries after sorting. The `free_vars` list used for fingerprinting is populated from the values (not keys) of `symbolic_bindings`, aligning with the variable names that actually appear in derived expressions after alias substitution.

Results validated — NaN and infinity rejected, causing fallback to next equation.

Error messages are specific: "No equation found for 'x'", "no value for 'y'", "all equations produced invalid results".

### fit.h

Curve fitting: `sample_function`, `fit_base`, `fit_all`, template functions, `recognize_constant`. Also hosts two output helpers shared with the solve/derive pipeline:

**`fmt_exact_double(double v, aliases={})`** — the single formatter for exact numeric output. Wraps `expr_recognize_constants(Expr::Num(v))` and stringifies the result; falls back to `fmt_num` when nothing matches. Accepts an optional `aliases` map (name → value) so callers can inject file-specific constants (e.g. `deg=pi/180`) that render as their names rather than raw decimals. Used from `fmt_solve_result` (main.cpp) and `format_derived` (system.h). `RECOGNIZE_FRACTION_MAX_DEN` (fit.h, currently 360) governs the denominator ceiling for rational recognition.

### trace.h

Three levels:
- `NONE` — no output (default)
- `STEPS` — algebraic reasoning: which equations are tried, inversions, substitutions, results
- `CALC` — steps plus numeric detail: each variable substitution and the expression before evaluation

All trace output goes to stderr. Controlled by `--steps` and `--calc` flags.

---

## Testing

1944+ tests organized into functional tests, edge cases, and robustness groups:

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

All 1944+ tests pass clean under every sanitizer — no leaks, no undefined behavior, no memory errors.

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

**Arena-allocated memory.** All expression nodes are allocated from `ExprArena` — contiguous 1024-node chunks for cache locality. `ExprPtr` is a raw `Expr*`. No `shared_ptr`, no reference counting, no individual deallocation. The arena is owned by `FormulaSystem` and cleared in bulk when destroyed. Thread-local `ExprArena::current()` provides the active arena via RAII scoping.

**No reference cycles.** Expression trees are DAGs (directed acyclic graphs) — children never point back to parents.

**No buffer arithmetic.** The code uses `std::string` and `std::vector` instead of raw char arrays or pointer arithmetic. Bounds are checked by the standard library in debug mode.

**Guarded casts.** The `fmt_num` function casts `double` to `long long` for display, but only when `abs(v) < 1e12` — well within `long long` range. UBSan confirms this never overflows.

**Division by zero handled in two places.** The evaluator returns `NaN` on `x / 0` (IEEE 754 propagation; `NaN` results are filtered as non-finite at result boundaries). The solver checks for zero/near-zero coefficients before dividing. UBSan confirms no unchecked integer division reaches the hardware.

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

### Pointer const deduction

`const auto` on a pointer deduces `T* const` (pointer itself is const, pointee is mutable) — cppcheck's `constVariablePointer` check still fires. The correct idiom is `const auto* sol = fn(...)`, which deduces `const T*` (pointee const). Empirically verified: `const auto` does NOT silence `constVariablePointer`. Use `const auto*` at every local pointer declaration site where the pointee is not mutated.

### constexpr and inline

- **`constexpr`** — type predicates (`is_num`, `is_zero`, etc.), enum queries (`is_additive`), compile-time constants
- **`inline`** — everything else in headers (required for ODR in header-only code)
- Prefer function pointers over `std::function` to avoid heap allocation

### Compile-time safety (static_assert)

Use `static_assert` to catch structural mistakes at compile time:

- **Enum counts** — enums have a `COUNT_` sentinel. `static_assert(static_cast<int>(BinOp::COUNT_) == 5)` catches enum additions that aren't reflected in dependent code.
- **Table sizes** — `static_assert(sizeof(table)/sizeof(table[0]) == static_cast<size_t>(BinOp::COUNT_))` catches table/enum mismatches.
- **Index assumptions** — `static_assert(static_cast<int>(BinOp::ADD) == 0)` documents that enum values are used as array indices.
- **Constant ranges** — `static_assert(EPSILON_ZERO > 0 && EPSILON_ZERO < 1e-6)` prevents accidental misconfiguration.

### Runtime safety (assert)

Use `assert` for invariants that should always hold in correct code:

- **Factory methods** — assert arena is active, operands are non-null (`assert(l && r && "BinOp operands must not be null")`)
- **Post-conditions** — assert results are non-null after operations (`assert(next && "simplify_once must not return null")`)
- **Enum sentinels** — `case COUNT_: assert(false && "invalid BinOp")` in switch statements. Do NOT use `default:` — that suppresses the compiler's `-Wswitch` warning for missing cases. The `case COUNT_:` approach gives both the compile-time warning (for real missing cases) and the runtime trap (for the sentinel).

### Data-driven design

Prefer data tables and registries over switch statements and if-else chains:

- **BinOp metadata** — `binop_info()` returns symbol, precedence, and eval function from a single table. Don't add per-operator switches.
- **Builtin functions** — `builtin_functions()` returns a `std::map`. Add new functions there.
- **Solver strategies** — `enumerate_candidates()` generates candidates for all solver modes. Add new strategies there.

### Error handling

- **Exceptions for exceptional cases only.** Failure during normal flow (numeric probing, candidate enumeration, best-effort parsing) is expected — those paths use `Checked<double>` or `std::optional`, not `try/catch`. Division by zero, unresolved variables, and failed probes are normal flow. True exceptions are unrecoverable conditions (`std::bad_alloc`, programmer errors). `Checked<T>` is the typed complement: non-exceptional failure has a zero-overhead typed representation rather than relying on NaN-discipline by convention.
- Empty `catch` blocks are not allowed (flagged by clang-tidy `bugprone-empty-catch`). Empty catches that must exist for correctness (best-effort sub-system load, `std::stoi` on user input, etc.) MUST be narrowed to a specific type (`std::runtime_error`, `std::invalid_argument`, `std::out_of_range`, `std::filesystem::filesystem_error`) and carry `NOLINTNEXTLINE(bugprone-empty-catch)` with a one-line rationale directly above the `catch` line. Untyped `catch (...)` is reserved for `main.cpp` CLI top-level and test code that deliberately exercises exception paths.
- Use `if (auto v = evaluate(e))` idiom at call sites — do not wrap `evaluate()` calls in try/catch. To hand off to pure-double code, use `.value_or_nan()` explicitly.
- Solver strategy failures are expected — try the next strategy. Only throw when all strategies have been exhausted.
- Validate results: reject NaN, infinity, and near-zero coefficients (`|coeff| < EPSILON_ZERO`).
- Use `std::ptrdiff_t` cast at iterator-arithmetic sites where the RHS is `size_t` (keep indices as `size_t` for natural vector access; cast only at the signed/unsigned boundary).
- `reserve()` vectors when the push_back count is statically known.

### Testing strategy

- **Write failing tests first** — prove the bug before fixing it, define the requirement before implementing it
- **Commit tests separately** before refactoring so you can revert safely
- **Semantic tests for output flexibility** — when simplifier output order may vary, test by evaluating with specific values rather than string comparison
- **Accept either ordering** for commutative operations: `ASSERT(r == "x * y" || r == "y * x", ...)`
- **Run the full pipeline** before committing: `make test && make sanitize && make analyze`
- **cppcheck inline suppressions** require the `--inline-suppr` flag, which is included in the Makefile's `analyze` target. A `// cppcheck-suppress <id>` comment has no effect unless the tool is invoked with `--inline-suppr`.

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
9. Conditions (parsing, solving, errors, global, branching)
10. Multiple returns and ValueSet
11. Recursion (depth guard, factorial)
12. Pre-refactor safety nets (strategy coverage, builtin exhaustive)
13. Simplifier improvements (rule interactions, flattening targets)
