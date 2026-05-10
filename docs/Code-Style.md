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

### Rule: load-bearing comments must point at a structural cause, not paper over a naming or control-flow defect

**Convention:** when a function passes the comprehension gate at T3 (with comments) but fails at T1/T2 because a comment explicitly tells the reader "what's really happening" (e.g. "cse_replace does exactly this with structural eq", "fall through to numeric path", "see also …"), the comment is a refactor signal. The *first* response is to remove the structural cause — rename, restructure, or extract — so the comment becomes redundant. Only when the structural cause is genuinely unfixable (e.g. cross-cutting algorithmic context) does the comment stay as load-bearing.

**Anti-pattern:** the comment is the first line of defence. Two examples in Cycle I-M2:
- `try_u_sub_integrate` calling `cse_replace` for non-CSE structural rewriting, with a comment saying so. The rename (or wrapper) is the structural fix; the comment is the symptom.
- `resolve_integral_calls` 4-arg branch's `if (val) { if (finite) return; /* fall through */ } else { return diff; }` asymmetric early-return, where the comment names the asymmetry. Extracting an explicit dispatch helper is the structural fix.

**Reason:** T3-only-passes mean a reader without the comment cannot follow. Comments rot, drift, get stripped by IDE flows, and are skipped by Haiku-grade readers under context pressure. Code that reads correctly without leaning on comments is durable; code that requires a comment to disambiguate the next step is not.

**Origin:** Cycle I-M2 — Haiku-B failure on `try_u_sub_integrate` and `resolve_integral_calls` 4-arg branch (two sites in same cycle, same diagnosis). **Validated** Cycle I-M3 by a third occurrence (`try_ibp_integrate` render-order branch — same shape, same T1+T2-fail T3-pass profile). Three sites total in the integration arc; the rule is now durable.

### Rule: heuristic-priority / rank functions returning small magic ints should use a named-constant `enum class`

**Convention:** when a function's purpose is to assign a priority / rank / score from a small fixed set of categories (LIATE rank, operator precedence, taxonomy depth, etc.), the integer values returned must be defined in a named-constant `enum class` near the function. The function body uses `static_cast<int>(EnumName::Category)` (or the `int` underlying type directly when the enum's underlying type is `int`); the call site continues to compare ints. The enum *is* the table; the comment-table burden disappears.

**Anti-pattern:** the comment block above the function names a category-to-int mapping that the function body then duplicates inline as raw integer returns. A reader without the comment cannot justify why "logarithmic returns 5" — the value is pure magic. Two cycles in a row, the load-bearing-comment-anti-pattern fires on this exact shape.

Concretely (example from Cycle I-M3, src/expr.h `liate_priority`):

```cpp
// BEFORE — magic ints, table in comment
//   Logarithmic    → 5
//   Inverse-trig   → 4
//   ...
[[nodiscard]] inline int liate_priority(const Expr& e, const std::string& var) {
    if (e.name == "log") return 5;
    if (e.name == "asin" || ...) return 4;
    ...
}

// AFTER — named constants, table is the enum
enum class LiateRank : int {
    None = 0, Exponential = 1, Trigonometric = 2,
    Algebraic = 3, InverseTrig = 4, Logarithmic = 5,
};
[[nodiscard]] inline int liate_priority(const Expr& e, const std::string& var) {
    if (e.name == "log") return static_cast<int>(LiateRank::Logarithmic);
    ...
}
```

**Reason:** small-int returns from a heuristic-scoring function are a recognised readability hazard — the int value carries no semantic outside the function, the comparison-at-call-site uses the int but doesn't care which value, and the comment that maps int↔category is the only thing keeping the function legible. Replacing the int with a named enum carries semantic at the return site, eliminates the comment-as-load-bearing-spec, and keeps call-site comparison as int (so no caller change is required when the enum is added).

**Pattern coverage at extraction:** 3 sites — `liate_priority` (this finding), `precedence` in expr.h:1081 (returns 0-5 for operator precedence; constant `5` falls out as the literal "atom" precedence), `canonicity_score` in expr.h:1357 (returns `pair<int, int>` for derive ordering — partial fit, the int values are computed not magic, but the *ordering convention* across return sites is comment-driven and would benefit from named constants for "tie-break direction"). N≥3 met.

**Origin:** Cycle I-M3 — Haiku-B failure on `liate_priority` (T1=wrong-on-detail because rank values 5/4/3/2/1 are inline-magic without the LIATE-table comment).

**Validated:** Cycle Cleanup-Bundle (2026-05-10) — A2 implemented this refactor exactly as Code-Style prescribed. Re-graded `liate_priority` at all three tiers: clean throughout (T1 mechanics moved from wrong-on-detail to match). The named-constant lift produced the predicted readability win **without any caller change** (call site continues to use ints for rank comparison). Strong evidence the rule's prescribed fix is correct. Future heuristic-priority functions should adopt the pattern at first authorship, not retrofit.

### Rule: prefer named-struct `operator()` over `std::function` for recursive tree-walkers (refinement)

**Convention:** when a recursive walker over the AST is needed inside a function body (and an inline lambda capturing `&this`/`&out` would otherwise be assigned to `std::function<...>` to enable self-reference), declare a local named struct with `operator()` and reference members for the captured state. Self-reference uses `(*this)(child)`.

**Anti-pattern:** `std::function<void(const Expr*)> walk = [&](const Expr* n) { ... walk(child); ... };` — heap allocation via type erasure, opaque to a comprehension-gate reader (the recursion's name disappears into the lambda body).

**Good:**
```cpp
struct FactorWalker {
    const Expr* factor;
    bool factor_remains = false;
    void operator()(const Expr* n) {
        if (factor_remains || !n) return;
        if (expr_equal(*n, *factor)) { factor_remains = true; return; }
        switch (n->type) {
            case ExprType::BINOP: (*this)(n->left); (*this)(n->right); break;
            // ...
        }
    }
};
FactorWalker walker{factor};
walker(quotient);
```

**Reason:** removes type-erasure overhead AND lifts readability. A Haiku-grade reader sees a named struct with named members; the recursion is `(*this)(child)`, not a captured-by-ref lambda variable. **Empirical: Cycle Cleanup-Bundle measured T1 mechanics improvement on `try_cancel`** from Cycle I-M2's vague-but-correct (with `std::function walk`) to match (with `FactorWalker`) on the same Haiku-grade comprehension test. The same A1 rewrite applied to `try_u_sub_integrate`'s `gather` made `Gatherer`'s recursion legible at T1; while R1's deeper `cse_replace` naming issue is independent, the gather-loop's clarity improved measurably.

**Refines:** the existing pre-existing rule "prefer template callables and named structs over `std::function`" (in `## Pre-existing conventions`) — that rule covered API-level / storage-level `std::function` use. This refinement extends the principle to *recursive tree-walkers within a function body*, where the historical justification for `std::function` (need for self-reference in C++17 lambdas) had become a default. The two-keystroke "lambda + std::function" idiom is more verbose at the textual level but loses to a named struct on every readability axis.

**Pattern coverage at extraction:** 4 sites in expr.h + system.h — `FactorWalker` (`try_cancel`, this cycle's A1), `Gatherer` (`try_u_sub_integrate`, this cycle's A1), `Walker` (`cse_extract`, pre-existing — predates the rule but follows it), `TreeCounter` (`cse_extract`, pre-existing — same). N≥3 met. The pre-existing sites confirm the pattern was already idiomatic in places; the cycle's A1 made it the *default* for new walker code.

**Origin:** Cycle Cleanup-Bundle — measured T1-mechanics improvement on `try_cancel` after A1's `std::function`→struct rewrite (Cycle I-M2's vague-but-correct → Cycle Cleanup-Bundle's match on the same function under the same Haiku-grade test).

## File-organisation rules

File-scope rules. Appended by the blind-spot critic when file-scope tests reveal a recurring file-level comprehension failure (file-explainer can't reliably name purpose / components / relationships / pattern). Each entry follows the same rule format as above; **Origin** line names the cycle and file.

Examples of rules that would live here once empirically derived:

- "Files > 1500 lines must declare a `// SECTION:` header table at the top, mirroring logical structure."
- "A header file mixing AST type definitions with solver-strategy implementations should be split."
- "Files with unrelated top-level concerns (parser + solver + formatter in one file) violate file-cohesion."

### Rule (provisional): non-contiguous milestone surfaces in a >2000-line file require cross-reference comments at each end

**Convention:** when a single feature-milestone (Future.md item, design-proposal milestone, or analogous unit of work) ships symbolic and numeric — or otherwise paired — halves in two physically separated regions of the same file, each region's section header must name the other region by line-anchor and one-line role description.

Concretely (example from Cycle I-M2, src/expr.h §"Symbolic integration"):
- Symbolic block header: `// Numeric counterpart: adaptive_simpson (line ~3329) — definite-integral fallback when symbolic_integrate returns nullptr.`
- Numeric block header: `// Paired with symbolic_integrate (line ~2690 in §Symbolic integration); dispatch is resolve_integral_calls in system.h.`

**Anti-pattern:** symbolic and numeric halves of the same milestone shipped in non-adjacent regions of a 3500-LOC header with no cross-reference. A `file-explainer` reading either half does not realise the other exists; the design pattern (symbolic-then-numeric fallback) is invisible at the file level.

**Reason:** files in this codebase grow monotonically (header-only design + interleaved feature areas). Once a file passes ~2000 LOC, milestone surfaces start interleaving with other concerns. Cross-references restore the structural pairing without requiring a file split. Code-as-prose for navigation when code-as-structure isn't enough.

**Status:** **adopted** — Cycle I-M3 validated the rule under live use: M3's §"Symbolic integration" header comment (expr.h lines 2716-2762) embeds M1+M2+M3 milestone notes in a single comment block, and the M3 cycle ran without re-flagging the section. The cross-reference back from `adaptive_simpson` (line ~3329) to §Symbolic integration remains R4 (Future.md, open) — the rule's mechanism (cross-reference at each end) is partially in place but not symmetrically. The rule itself is durable; promotion from provisional to adopted reflects the absence of regression under one additional cycle of churn.

**Origin:** Cycle I-M2 — file-explainer scored vague-but-correct on Components/Relationships/Pattern for src/expr.h §Symbolic integration extension (lines 2616–2878 + 3329–3391, separated by ~430 LOC). Cycle I-M3 — validated; no regression, M3 surface follows the rule.

### Rule (provisional): intra-class section dividers in classes exceeding ~1500 LOC

**Convention:** when a single class body exceeds ~1500 LOC and contains conceptually separable sub-areas (e.g. parsing, loading, solving, deriving, CLI orchestration), each sub-area's first member is preceded by a nested section divider in the visual style:

```cpp
    // ────────────── Subsection: Loading and parsing ──────────────
```

The box-drawing-character style (`──────`) visually subordinates these dividers below the file-level top dividers (which use `============`). No code moves; the goal is structural legibility for a single-pass file reader.

**Anti-pattern:** a 3000+ LOC class body with public/private blocks but no internal grouping marker. Methods from different sub-areas (loading vs solving vs deriving) interleave by authorship order and a `file-explainer` cannot identify sub-area boundaries without reading bodies.

**Reason:** the existing file-organisation rule above ("non-contiguous milestone surfaces in a >2000-line file require cross-reference comments at each end") covers cross-region references in a file. This rule covers *intra-class* sub-area boundaries in a class that has itself exceeded the 1500-LOC threshold. The same comprehension-gate principle (Haiku-grade reader cannot navigate without structural delimiters) applies. Empirical: Cycle Cleanup-Bundle file-explainer scored Components and Relationships at vague-but-correct on src/system.h's `class FormulaSystem` (~3400 LOC) — the file's *top-level* layout is clean (5 sections delimited) but the class body itself has no internal markers.

**Status:** **provisional** — single instance in the codebase (`class FormulaSystem` is the only class >1500 LOC). The rule will be retired or promoted by the next cycle's evidence: if R8 (Future.md) lands and the next cycle's file-explainer scores src/system.h's Components / Relationships at *match*, the rule is validated and adopted; if a second >1500-LOC class emerges in the codebase and exhibits the same pattern, also promote.

**Origin:** Cycle Cleanup-Bundle — file-explainer scored vague-but-correct on Components and Relationships for src/system.h (4037 LOC; central `class FormulaSystem` at lines 293–3700 has 5 conceptually separable sub-areas with no internal section delimiters).

## Architecture rules

Codebase-wide structural rules. Appended by the blind-spot critic when architecture-scope tests reveal a comprehension failure (architecture-explainer can't reliably identify codebase purpose / module roles / dependency graph / pattern). Architecture rules with deeper "this is how the codebase works" content also land in `docs/Developer.md`.

Examples of rules that would live here once empirically derived:

- "Module dependencies flow in one direction along the lexer → parser → expr → system → main pipeline; reverse dependencies are forbidden."
- "File names must be domain-meaningful, not generic (`Manager`, `Data`, `State` are warning signs)."
- "Top-level public types in a file should cluster around a single role; if the file's public symbols span multiple roles, the file is a split candidate."

### Rule (provisional): symbol-only manifests should make codebase purpose, module roles, and dependency direction inferable without prose

**Origin:** Cycle 2026-05-10 — architecture-scope blind-spot ANALYZE (first architecture pass after staged 20-function sweep). Both floor graders (Haiku + Gemma), reading only `<file> (lines): types: ... functions: ...` manifests, identified **codebase purpose, module roles, dependency graph, and architectural pattern correctly**. Module names (`lexer.h`, `parser.h`, `expr.h`, `system.h`, `fit.h`, `trace.h`, `main.cpp`) and top-level type names (`Expr`, `Token`, `FormulaSystem`, `ValueSet`, `Condition`, `RewriteRule`) carry the architectural narrative without prose support.

**Rule (positive form):** the codebase's architecture should be legible from a symbol-only manifest:

- **File names are the layer names.** `lexer` / `parser` / `expr` / `system` / `fit` / `trace` / `main` each name a clear pipeline stage or domain. Generic names (`Manager`, `Data`, `State`, `util`) are forbidden; if such a name is proposed, the file's role hasn't been identified yet.
- **Top-level types name domains, not abstractions.** `Expr`, `ExprArena`, `Token`, `FormulaSystem`, `ValueSet`, `Condition`, `RewriteRule` — each surfaces what the type IS in domain terms. `Handler`, `Helper`, `Wrapper`, `Adapter` are warning signs.
- **Function names follow per-file patterns.** Parsers use `parse_*`; simplifiers use `simplify*`/`apply_rewrite_rules`; solvers use `resolve*`/`solve*`/`newton_solve`/`bisection_solve`; tree walkers use `tree_map*`. New code in a file should follow that file's verb conventions.
- **Dependency direction is unidirectional.** `lexer → parser → expr → system → main`; `fit.h` parallel to the main pipeline; `trace.h` as utility. Reverse imports are forbidden — if `expr.h` would need to know about `system.h`, the dependency is mis-modelled.

**How to verify:** run the architecture-scope blind-spot test (`/blind-spot-sweep` or auto-fire when `src/*.h`/`src/*.cpp` change). If both floor graders correctly identify the four axes (purpose, module roles, dependency graph, pattern) from the symbol-only manifest, the rule is satisfied. Failure on any axis = rename / re-layer / re-organise the affected file before proceeding.

### Rule (provisional): a codebase whose two largest files together exceed ~80% of source LOC is a structural concern, not a domain-density signal

**Origin:** Cycle 2026-05-10 — architecture-scope blind-spot ANALYZE. `expr.h` (3861 LOC) + `system.h` (4037 LOC) = 7898 LOC, ~88% of the manifest's source weight. Gemma flagged this as "Monolithic Core (file size)" *unprompted* — a `nuanced-refactor-candidate` per the floor-vs-supplementary verdict matrix. Haiku did not flag size; the disagreement is itself the signal that the weaker grader's working memory is taxed by the file weight.

**Rule:** when a codebase's two (or fewer) largest files account for >80% of source LOC, file size is a structural variable to track, not a domain-density invariant. Specifically:

- **A file approaching ~3000 LOC** is a candidate for split-by-responsibility, even if internally cohesive. If multiple sub-areas exist within the file (e.g. `numeric.h` extractable from `system.h`'s solver substrate), the split should be designed under a Future.md item with a concrete reopen trigger.
- **A class within such a file approaching ~1500 LOC** is a candidate for intra-class section dividers (see file-organisation rule above) as a cheap interim step before split.
- **The split must preserve dependency direction.** Extracting a sub-file from a large file should not reverse the pipeline (extracting `numeric.h` from `system.h` is fine because `system.h` calls `numeric.h`-content already; pulling `expr.h` symbols UP into `system.h` would reverse the layering).

This rule is satisfied today by the open work in `Future.md`:
- **#R8** (FormulaSystem intra-class section dividers) addresses intra-class structure cheaply.
- **T4.1 trigger** (`numeric.h` extraction from `system.h`, then `query.h`) addresses the file-split path.
- **#R12** (`nuanced-refactor-candidate` — engine/query API split) is the design-track sibling of T4.1.

**Reopen trigger** (rule retirement): when `expr.h` and `system.h` together drop below ~70% of source LOC (via T4.1's `numeric.h` extraction or analogous splits), re-evaluate whether this rule still applies or has been outgrown.

### Rule (positive datum): unidirectional pipelines with a central domain module are the validated architectural shape

**Origin:** Cycle 2026-05-10 — architecture-scope blind-spot ANALYZE. Both floor graders independently identified the architecture as a "linear pipeline with a central domain module" (Haiku) / "Layered Architecture with a strong emphasis on a Core Engine pattern" (Gemma). Both read the dependency direction correctly. The pattern is recognized without prose.

**Rule:** future architectural additions should preserve this shape:

- **The pipeline is `lexer → parser → expr → system → main`.** New modules slot into this chain or stand parallel to it (the `fit.h` precedent), never reverse it.
- **Parallel modules attach at the orchestration layer.** `fit.h` is consumed by `system.h` and `main.cpp`; it does not import `system.h`. Future parallel modules (e.g. a hypothetical `units.h` for dimensional analysis, `latex.h` for export) should follow the same pattern: `expr.h`-imports-only, consumed at the system/main layer.
- **`expr.h` is the central domain.** It owns expression types, simplification, evaluation, and primitive solvers. New domain primitives (a new tree walker, a new value type, a new pattern-matching strategy) belong here. Algorithmic compositions / orchestrations belong in `system.h`.
- **Utility modules (`trace.h`, future `numeric.h`) are leaves.** They are imported by the pipeline; they import nothing from it.

**How to verify:** any new `.h` introduced to `src/` should pass the architecture-scope blind-spot test (run `/blind-spot-sweep`). If the new file's role isn't legible from its name + symbols alone, redesign before merging.
