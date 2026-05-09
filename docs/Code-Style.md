# Code Style

Style rules for fwiz. Multiple sections:

1. **Pre-existing conventions** — hand-authored, transcribed from `docs/Developer.md` and `CLAUDE.md`. These are 100%-certain conventions, mostly tooling-enforced (cppcheck, clang-tidy, grep locks).
2. **Empirically-derived rules** — appended by the blind-spot critic when function-scope Haiku-grader tests reveal a recurring readability failure.
3. **File-organisation rules** — appended when file-scope Haiku-grader tests reveal a recurring file-level comprehension failure.
4. **Architecture rules** — appended when architecture-scope Haiku-grader tests reveal a structural comprehension failure. Architecture rules with deeper "this is how the codebase works" content also land in `docs/Developer.md`.

## Comprehension-gate principle (load-bearing meta-rule)

Style rules in this document are validated against a **comprehension gate**: a
weaker grader (Haiku) is asked to explain code, files, or architecture given
only the artefact itself. If the weaker grader fails, the artefact has failed
the gate — and a rule is extracted to keep similar artefacts from failing in
the future.

The natural inclination when a weaker grader fails is to dismiss the failure
as "Haiku is just less capable." **Resist this.** The whole purpose of using
a weaker model is that its failures track the readability floor — if Haiku
can't follow it, neither will the next agent that lacks full context.

When a Haiku-grader fails, the diagnostic order is:

1. **Size** — too large for working memory? Split / extract.
2. **Cohesion** — unrelated concerns mashed together? Separate by responsibility.
3. **Structure** — does organisation carry meaning, or is it a wall of code? Section delimiters, type taxonomies, named constants.
4. **Naming** — are identifiers doing the lifting they should? Descriptive names; module-level naming for files; domain-meaningful taxonomies.

Rules in the empirically-derived sections of this document are extracted along
these axes. The grader's failure is the signal. The rationalization "Haiku is
less capable" is the failure mode that defeats the test.

## Rule format

    ### Rule: <short imperative name>
    **Convention:** what to do.
    **Anti-pattern:** what to avoid.
    **Reason:** why — citation if tool-enforced.
    **Origin:** hand-authored | Cycle N — Haiku failure on `<function>`

## Pre-existing conventions

### Naming and structure

#### Rule: no magic numbers
**Convention:** use named constants (`EPSILON_ZERO`, `EPSILON_REL`, `SIMPLIFY_MAX_ITER`, `NUMERIC_DEFAULT_SAMPLES`, etc.).
**Anti-pattern:** literal numbers in non-trivial contexts.
**Reason:** intent encoded in the name; tunability without a hunt.
**Origin:** hand-authored.

#### Rule: enum base type is `uint8_t`
**Convention:** every enum declares `: uint8_t`.
**Reason:** struct-size minimization across the codebase.
**Origin:** hand-authored.

#### Rule: `enum class` for boolean discriminators
**Convention:** any `bool` parameter or field encoding a two-valued semantic distinction must be replaced with a named `enum class`.
**Anti-pattern:** `bool inherent` on `SimplifyAssumption`.
**Good:** `AssumptionSource : uint8_t { Derived, Inherent }`.
**Reason:** call sites read by intent (`Inherent`) not by truth value (`true`).
**Origin:** hand-authored.

#### Rule: enum `COUNT_` sentinel, never `default:`
**Convention:** every enum has a `COUNT_` sentinel; switches handle it explicitly with `case COUNT_: assert(false && "...")`.
**Anti-pattern:** `default:` clauses.
**Reason:** `default:` suppresses `-Wswitch` for genuinely missing cases. The sentinel pattern preserves the warning AND traps misuse at runtime.
**Origin:** hand-authored.

### Pointer and reference idioms

#### Rule: references for non-null, pointers for nullable
**Convention:** `const Expr&` when the callee always expects a valid expression (predicates, evaluators, tree queries). `ExprPtr` (`Expr*`) for return types, struct fields, and parameters that may be null (`substitute`, `simplify`, `solve_for`).
**Reason:** signal nullability through the type. No null check needed inside reference-taking functions.
**Origin:** hand-authored.

#### Rule: `const auto*` at const-pointer local declarations
**Convention:** `const auto* sol = fn(...);` — deduces `const T*` (pointee const).
**Anti-pattern:** `const auto sol = fn(...);` — deduces `T* const` (pointer const, pointee mutable).
**Reason:** cppcheck `constVariablePointer` empirically fires on the anti-pattern; the correct idiom silences it cleanly.
**Origin:** hand-authored.

#### Rule: range-for over `vector<ExprPtr>` uses `const auto*`
**Convention:** `for (const auto* x : c)` when iterating containers of `ExprPtr`.
**Anti-pattern:** `for (const auto& x : c)` — deduces `Expr* const&` and triggers cppcheck `constVariableReference`.
**Origin:** hand-authored.

#### Rule: lambda over `vector<ExprPtr>` takes `ExprPtr` by value
**Convention:** `[](ExprPtr x) { ... }` — by-value house style.
**Acceptable:** `[](const Expr* x)` when the callee accepts `const Expr&` via dereference.
**Anti-pattern:** `[](const ExprPtr& x)` — compiles, semantically equivalent, but violates the "ExprPtr = raw pointer, not a reference type" convention. Reviewer finding.
**Origin:** hand-authored.

### Const correctness

#### Rule: locals never mutated must be `const`
**Convention:** local variables that are not reassigned after construction must be declared `const`.
**Reason:** clang-tidy `misc-const-correctness` (baseline zero as of Cycle 7.5; 245 findings fixed in that cycle).
**Origin:** hand-authored.

#### Rule: west-const placement
**Convention:** `const T name`, `const auto& x`. Codebase is 100% west-const.
**Anti-pattern:** east-const (`T const name`).
**Reason:** consistency. When `clang-tidy --fix` writes east-const (its default), normalize before committing.
**Origin:** hand-authored.

### Compile-time safety

#### Rule: `constexpr` for predicates and constants; `inline` for everything else
**Convention:** `constexpr` on type predicates (`is_num`, `is_zero`), enum queries, compile-time constants. `inline` on every other function in headers (ODR requirement for header-only code).
**Origin:** hand-authored.

#### Rule: `static constexpr` or annotated `static const`
**Convention:** all compile-time-constant statics are `static constexpr`. Runtime `static const` (lambdas, `std::map`, `std::sqrt` initializers — none constexpr-able in C++17) carry a `// static const: <reason>` comment on the line above.
**Lock:** `grep -nE 'static const ' src/*.h | grep -v '// static const:'` must return 0 lines.
**Origin:** hand-authored.

#### Rule: `static_assert` at structural boundaries
**Convention:** use `static_assert` for enum counts (`sizeof(table)/sizeof(table[0]) == BinOp::COUNT_`), index assumptions (`BinOp::ADD == 0`), constant ranges (`EPSILON_ZERO > 0 && EPSILON_ZERO < 1e-6`).
**Reason:** catches structural mistakes at compile time before they ship.
**Origin:** hand-authored.

### Runtime safety

#### Rule: `assert` at factories and post-conditions
**Convention:** factories assert operand non-null (`assert(l && r && "BinOp operands must not be null")`); post-conditions assert results non-null (`assert(next && "simplify_once must not return null")`).
**Origin:** hand-authored.

### `[[nodiscard]]` discipline

#### Rule: `[[nodiscard]]` on pure factories, predicates, queries, transformers, evaluators, and `Checked<T>` accessors
**Convention:** discarding the return is almost always a logic error. `-Wunused-result` (under `-Wextra`) makes the discard a compile-time error. Side-effect-only call sites discard explicitly with `(void)`.
**Placement:** before `inline`/`constexpr`/`static`/template headers.
**Reason:** clang-tidy `modernize-use-nodiscard` flags new pure functions without it.
**Origin:** hand-authored.

### STL algorithms

#### Rule: prefer `<algorithm>` over hand-rolled loops where the algorithm form is at least as clear
**Convention:** `std::any_of`, `std::all_of`, `std::find_if`, `std::count_if`, `std::transform`, `std::accumulate`, `std::copy_if` over manual loops.
**Reason:** cppcheck `useStlAlgorithm` (un-suppressed since Cycle 4) catches new raw loops automatically.
**Acceptable to keep manual:** multiple side effects in body, lambda gymnastics required (`std::move_iterator + back_inserter`), numerical inner loops in `fit.h` where arithmetic structure matters. Annotate inline: `// not std::algorithm: <reason>` plus `// cppcheck-suppress useStlAlgorithm`.
**Origin:** hand-authored.

### `std::function` policy

#### Rule: prefer template callables and named structs over `std::function`
**Convention:** template `F&&` parameters when the callable isn't stored beyond the call. Named structs with `operator()` for recursive lambdas C++17 can't express otherwise.
**Anti-pattern:** unjustified `std::function` (heap allocation + virtual dispatch overhead).
**Justified keeps (must annotate `// std::function: <reason>`):** boundary erasure across thread-local / ABI, optional callbacks stored as nullable `const std::function*`, heterogeneous lambda storage in `std::vector<T>`.
**Lock:** `grep -nE 'std::function' src/expr.h src/system.h src/fit.h | grep -v '// std::function:'` must return 0 lines.
**Origin:** hand-authored.

### Error handling

#### Rule: exceptions only for unrecoverable conditions
**Convention:** normal-flow failures (numeric probing, candidate enumeration, best-effort parsing) use `Checked<double>` or `std::optional`. True exceptions are reserved for `std::bad_alloc`, programmer errors, and other unrecoverable conditions.
**Anti-pattern:** `try/catch` around `evaluate()` calls. Use `if (auto v = evaluate(e)) { ... }` instead.
**Origin:** hand-authored.

#### Rule: no empty `catch` blocks
**Convention:** flagged by clang-tidy `bugprone-empty-catch`. Empty catches that must exist for correctness narrow to a specific type (`std::runtime_error`, `std::invalid_argument`, `std::out_of_range`, `std::filesystem::filesystem_error`) and carry `NOLINTNEXTLINE(bugprone-empty-catch)` with a one-line rationale on the line above.
**Reason:** untyped `catch (...)` is reserved for `main.cpp` CLI top-level and test code that deliberately exercises exception paths.
**Origin:** hand-authored.

### Data-driven design

#### Rule: prefer tables and registries over switches and if-else chains
**Convention:** `binop_info()` returns a single tabulated row; `builtin_functions()` returns a `std::map`; `enumerate_candidates()` generates candidates for all solver modes from one place.
**Reason:** add-a-row beats add-a-case-everywhere. The data is the contract; the code reads it.
**Origin:** hand-authored.

### Testing discipline

#### Rule: failing test first
**Convention:** prove the bug before fixing it. Define the requirement before implementing it.
**Origin:** hand-authored.

#### Rule: commit tests separately before refactoring
**Convention:** test commits precede refactor commits, so a refactor regression can be reverted cleanly.
**Origin:** hand-authored.

#### Rule: semantic tests, not string-equal, when output order may vary
**Convention:** for commutative operations, accept either ordering: `ASSERT(r == "x * y" || r == "y * x", ...)`. For numeric output where formatting may vary, evaluate the result and compare numerically.
**Origin:** hand-authored.

## Empirically-derived rules

Function-scope rules. Appended by the blind-spot critic when function-scope Haiku-grader tests reveal a recurring readability failure. Each entry includes its origin cycle and the function whose failure prompted the rule.

_(none yet — populated by blind-spot-critic on first qualifying failure.)_

## File-organisation rules

File-scope rules. Appended by the blind-spot critic when file-scope tests reveal a recurring file-level comprehension failure (file-explainer can't reliably name purpose / components / relationships / pattern). Each entry follows the same rule format as above; **Origin** line names the cycle and file.

Examples of rules that would live here once empirically derived:

- "Files > 1500 lines must declare a `// SECTION:` header table at the top, mirroring logical structure."
- "A header file mixing AST type definitions with solver-strategy implementations should be split."
- "Files with unrelated top-level concerns (parser + solver + formatter in one file) violate file-cohesion."

_(none yet — populated by blind-spot-critic file-scope tests.)_

## Architecture rules

Codebase-wide structural rules. Appended by the blind-spot critic when architecture-scope tests reveal a comprehension failure (architecture-explainer can't reliably identify codebase purpose / module roles / dependency graph / pattern). Architecture rules with deeper "this is how the codebase works" content also land in `docs/Developer.md`.

Examples of rules that would live here once empirically derived:

- "Module dependencies flow in one direction along the lexer → parser → expr → system → main pipeline; reverse dependencies are forbidden."
- "File names must be domain-meaningful, not generic (`Manager`, `Data`, `State` are warning signs)."
- "Top-level public types in a file should cluster around a single role; if the file's public symbols span multiple roles, the file is a split candidate."

_(none yet — populated by blind-spot-critic architecture-scope tests.)_
