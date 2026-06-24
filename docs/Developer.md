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
- Symbolic differentiation (`diff(f, x)` builtin + `diff(...)=?` CLI query)
- Symbolic integration — Tier 1 indefinite + M2 u-substitution + M2 definite + M3 IBP/LIATE (`integral(f, x)` and `integral(f, x, a, b)` builtins; `integral(...)=?` CLI query); adaptive Simpson numeric fallback; `BuiltinMeta` registry — Future #16 M1+M2+M3
- Verification (`--verify`)
- Explore mode (`--explore`, `--explore-full`)
- CLI expression values (`width=2^3, height=sqrt(9)`)
- Inline comments (`# after equations`), semicolons as line separators
- Dotted variable names (`car.velocity.x`) as flat identifiers — no struct machinery needed
- Data-driven rewrite rules (23 builtin `.fw` patterns for simplification)
- Commutative pattern matching (N-term additive/multiplicative permutation search)
- `undefined` keyword for explicit domain boundaries and exhaustiveness checking
- Context-aware simplification (conditions checked against known numeric bindings)
- Section headers with return-var sugar: `[f(x) -> result] = x^2`
- Arena allocator for expression nodes (100% cache-friendly)
- Numeric solving (adaptive grid scan + Newton/bisection, enabled by default)
- Exact/approximate result classification (`=` vs `~`)
- Curve fitting (`--fit`) with template matching and recursive composition
- Built-in constants (`pi`, `e`, `phi`, `i` — imaginary unit with NaN binding since 2026-05-09)
- Vector/matrix literals (`[1,2,3]`, `[[1,0],[0,1]]`) with element-wise ops and `matmul`/`det`/`inv`/`transpose` builtins (since 2026-05-10)
- Batch/table mode (`--table`) — evaluate formula across range inputs; output as TSV; `[start..stop @ step]` range syntax; compound ranges; cartesian and `--zip` element-wise modes (since 2026-05-11)
- Bounded aggregation (`sum`/`product`/`count`/`max`/`min`/`mean` over discrete ranges) — `sum(i^2, i in [1..n])` with unroll-at-simplify model; formula-call bodies; broadcast sugar; reverse-solve composes for free (gen-6 cycle 1, 2026-06-22); symbolic-bound aggregation resolves via the binding path (`collect_vars` binder-awareness, gen-6 cycle 2a)
- Ordered collections (`{1,2,3}` → `seq(...)` literal), `map(body, i in [dom])` (materializes seq), `foldl(coll, op, init)` (left-fold via named binary section); `stdlib/collections/operators.fw` defines `add`/`mul`; post-load order map→foldl→aggregate (gen-6 Collections Cycle 1, 2026-06-24)
- Combinatorics stdlib (`stdlib/combinatorics/`: factorial, nPr, nCr, falling, rising factorial, hypergeometric PMF + upper-tail) and probability stdlib (`stdlib/probability/`: E[X], variance, hyp_mean, hyp_variance, order_statistic) — built on bounded aggregation (gen-6 cycles 2 + 3)
- Irrational number recognition (pi, e, sqrt(2), sqrt(3) in fitted coefficients)
- Structural fractions (`1/3` preserved, not folded to `0.333...`; exact rational arithmetic)
- Constant recognition in derive output (log(2), log(3), sqrt(N), pi, e)
- Output formatting: `--approximate` (collapse to float) / `--exact` (default, human-readable fractions and constants); `fmt_exact_double` shared helper closes solve/derive asymmetry

Planned (see Future.md):
- **Units / dimensional analysis** — type-axis unified (gen-5 cycles 3a+3b, 2026-05-15/16): `type_map_` (replaces `dim_map_`), `SetDef` registry with 4 built-in named sets (int/real/rational/imaginary — `complex` renamed cycle 3b), `is_in` canonical predicate + parse-time rewrite of `is_int`/`is_in_dimension` aliases, `SimplifyContext` transport, cross-file propagation bug fixed. User-defined predicate sets (`[name(param)] iff ...` sections, `USER_PREDICATE` kind) shipped in cycle 3b. Compound-expression dimensional propagation shipped cycle 3c (2026-06-06): `DimMap` exponent-map (`BindingType::dim` promoted from `std::string`), `compute_dim` recursive fold, `BuiltinMeta.dim_propagate` callbacks. See Future #7b FULL DONE, #84 trigger FIRED, #100 NEW PARKED.
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

### Validated architectural shape

Cycle 2026-05-10 architecture-scope blind-spot ANALYZE confirmed (via two model-family-different floor graders reading only a symbol manifest, no prose) that the architecture is legible from symbols alone:

- **Linear pipeline with a central domain module.** `lexer → parser → expr → system → main`. Both graders independently identified the layered structure and the dependency direction. New code should preserve this shape — see `docs/Code-Style.md` §"Architecture rules" for the rule statement.
- **`expr.h` is the central domain.** Expression types, simplification, evaluation, primitive solvers, tree walkers, pattern matching, `ValueSet`, builtin metadata registry. New domain primitives belong here.
- **`system.h` is the orchestrator.** Multi-equation resolution, `enumerate_candidates` (7 strategies), cross-equation elimination, derive mode (canonicalization + CSE), file loading, post-load passes, CLI query types. Algorithmic compositions belong here.
- **`fit.h` is parallel to the main pipeline.** Imported by `system.h` and `main.cpp`; does not import `system.h`. Future parallel modules (e.g. a hypothetical `units.h` for dimensional analysis, `latex.h` for export) should follow the same shape.
- **`trace.h` is a leaf utility.** Imported by the pipeline; imports nothing from it.

**Outstanding architectural concerns** (tracked in `docs/Future.md`):

- `expr.h` (3861 LOC) + `system.h` (4037 LOC) together account for ~88% of source LOC. Above the ~80% threshold codified in the Code-Style rule. The path forward is **T4.1** (extract `numeric.h` from `system.h` first, then `query.h`) plus **#R8** (intra-class section dividers as a cheap interim step).
- The `FormulaSystem` class conflates engine concerns (rewrite rules, simplify, evaluate, candidate enumeration) and query/CLI concerns (CLI query types, derive output formatting). **#R12** in `docs/Future.md` is the `nuanced-refactor-candidate` capturing this; the design-track work would split a thin `Engine` / `Solver` interface from the orchestrator. Multi-cycle arc, not single-cycle.

### lexer.h

Converts source text into tokens: `NUMBER`, `IDENT`, `PLUS`, `MINUS`, `STAR`, `SLASH`, `CARET`, `LPAREN`, `RPAREN`, `EQUALS`, `QUESTION`, `COMMA`, `END`.

- Handles: integers, floats (including leading dot `.5`), identifiers with underscores/digits, scientific notation (`1.5e3`, `100e-3`, `1E5` — `[eE][+-]?[0-9]+` tail consumed by `read_number` since 2026-05-13)
- Rejects: all non-mathematical characters with `"Unexpected character: X"` errors
- Bare `e`/`E` not followed by digits becomes a separate `IDENT` token (e.g. `1e` → `NUMBER(1)` + `IDENT("e")`, desugared to `1 * e` = Euler's constant by the parser)
- Does NOT handle: newlines (file parser splits lines first)
- **`DOTDOT` and `AT` tokens** (since gen-6 cycle 1, 2026-06-22): `..` two-dot sequence is checked BEFORE the number-start dispatch so a lone leading `.` is never misread as a float start; `'@'` maps to `AT`. `read_number`'s inner loop stops at the first `.` of a `..` sequence (`src_[pos_+1] != '.'` guard) so `1.5..3` lexes as `Num(1.5)`, `DOTDOT`, `Num(3)` rather than attempting `1.5.` as a float. `COUNT_` sentinel advanced 17 → 19; `static_assert` updated.
- No `switch` over `TokenType` exists in parser.h / system.h (all if/else chains), so new tokens require no switch updates in those files.

### parser.h

Recursive descent parser. Converts token stream to expression tree.

Precedence (highest to lowest):
1. Atoms: numbers, variables, function calls, parenthesized expressions
2. Unary minus: `-x`
3. Power: `x^2`
4. Multiplicative: `x * y`, `x / y`
5. Additive: `x + y`, `x - y`

**NUMBER-IDENT desugar (since 2026-05-13):** `primary()` detects a `NUMBER` token immediately followed by an `IDENT` token and desugars the pair into a `MUL` node without emitting any new token type. Three cases:
- `100kg` → `MUL(Num(100), Var("kg"))` — unit suffix; `kg` is an ordinary variable.
- `100sin(x)` → `MUL(Num(100), FUNC_CALL("sin", [Var("x")]))` — function-call branch; no warning.
- `100m^2` → `MUL(Num(100), Var("m"))^2` i.e. `(100*m)^2` — precedence quirk; emits a stderr warning pointing the user at `100 * m^2`. Fix is Future #74.

Power is currently NOT right-associative — `x^2^3` parses as `x^2` with trailing tokens. This is a known limitation.

**Range literal (since gen-6 cycle 1, 2026-06-22):** inside the `LBRACKET` branch of `primary()`, after parsing the first element, a `DOTDOT` token triggers `parse_range_literal(lo)` — consuming hi via `parse_expr()`, optional `@ step`, and `RBRACKET`. Emits `FUNC_CALL("range", {lo, hi})` (arity 2, step=1 implied) or `FUNC_CALL("range", {lo, hi, step})` (arity 3). The arity is intentionally distinct: downstream consumers can tell whether a step was explicitly supplied without normalizing. Absence of `DOTDOT` falls through to the existing vec/mat COMMA-gathered path — zero ambiguity.

**Aggregate iterator clause (since gen-6 cycle 1):** `parse_expr_or_iter_clause(args&)` — on `IDENT in` lookahead, pushes `Var(iter_name)` onto `args` then returns `parse_expr()` as the domain (reuses the LBRACKET branch, no duplicated range-parse). Called for each argument of a FUNC_CALL whose name is NOT an aggregate reducer guard; reducers' `in`-shaped arg becomes `{Var(iter), range}` without extra parser machinery. Single iterator only; multi-iterator cartesian forms are deferred (master plan cycle 3+).

### expr.h

The core of the system. Contains:

**ExprPtr** — raw pointer (`Expr*`) to arena-allocated expression tree nodes. Types: `NUM`, `VAR`, `BINOP`, `UNARY_NEG`, `FUNC_CALL`. All nodes allocated from `ExprArena` (contiguous chunks, 100% cache-friendly).

**ValueSet** — unified representation for conditions, ranges, and solutions. Intervals (open/closed, half-infinite) + discrete points + periodic families (since 2026-05-08, Future.md #12). Operations: intersect, union, filter, contains. Returned by `resolve_all()`.

**`Condition` / `CondClause` / `CondOp` / `CondLogic`** — Condition AST structs. Defined in `expr.h` after `ValueSet` and before `RewriteRule`. Moved from `system.h` in the T1 cycle, mirroring the existing `ValueSet` split: data and evaluation live in `expr.h`; parsing (`parse_condition`) stays in `system.h` because it uses `Lexer`/`Parser`. `check_condition(const Condition&, numeric_bindings)` is a free function in `expr.h` that evaluates a condition AST against numeric bindings; clauses with unbound wildcards return `nullopt` (treated as satisfied, same permissive default as before). `condition_to_string(const Condition&, expr_bindings)` serializes a condition AST with wildcard bindings substituted inline (using `expr_to_string`); used by `apply_rewrite_rules` to record assumption strings for `--steps`/`--calc` output. `RewriteRule::condition` is `std::optional<Condition>`, parsed once at rule-load time — not re-parsed per match attempt.

**`BindingType` / `SetDef` / `SimplifyContext`** — Type-system structs for the named-set mechanism (since gen-5 cycle 3a, 2026-05-15; `SetDef` extended in cycle 3b). Live in `expr.h` after `Condition` structs and before `RewriteRule` — placement is architecture-emergent: `check_condition`'s `is_in` dispatch arm needs full `SetDef` definitions (not just forward declarations), and `check_condition` lives in `expr.h`. `system.h` notes the move with a comment. `BindingType { std::string dim; std::set<std::string> sets; }` — per-variable type record stored in `type_map_`. `SetDef { Kind kind; std::function<bool(double)> membership; std::string parameter; std::optional<Condition> predicate; }` with `Kind = { BUILTIN_PREDICATE, USER_PREDICATE, DIM_SECTION, COUNT_ }` (3-valued since cycle 3b, `COUNT_==3` static_assert) — named-set registry entries. `parameter` and `predicate` fields are populated only for `USER_PREDICATE` entries; `membership` is populated only for `BUILTIN_PREDICATE`. `SimplifyContext { const map<string,BindingType>* type_map; const map<string,SetDef>* set_definitions; }` — aggregates both maps for transport into `check_condition` via `RewriteRulesGuard` thread-local; replaces the raw `dim_map*` pointer from cycle 2.

**collect_vars()** — Collects all variable names in a tree into a set. Used by the solver to find what needs resolving. **Aggregation-iterator binder awareness (since gen-6 cycle 2a, 2026-06-22):** when `collect_vars` hits a reducer FUNC_CALL (`is_aggregate_reducer(name)`), the iterator arg is a binder local to that subtree. The function collects free vars of all reducer args into a LOCAL set, erases the iterator name from that local set only, then merges the local set into the caller's `out`. Local-scope exclusion (not global erase) means a sibling occurrence of the same iterator name outside the reducer still appears in the caller's set (A2 shadowing correctness). Iterator position: `iter_pos = (name=="count") ? 0 : 1` (mirrors the arity layout in `try_unroll_aggregate`). Without this, `collect_vars` on `product((n-k+i)/i, i, range(1,k))` returns `{n,k,i}` and the solver aborts trying to resolve `i` as an unknown, breaking the combinatorics stdlib. **`map`/`foldl` binder guards (since gen-6 Collections Cycle 1, 2026-06-24):** the same local-exclusion pattern extends to `map(body, Var(iter), range)` (iter is bound) and `foldl(coll, Var(op_name), init)` (op_name is a section reference, not a numeric variable). Both guards precede the generic FUNC_CALL fallthrough; `is_aggregate_reducer` remains separate.

**contains_var()** — Direct recursive search for a variable. Returns at first hit with no allocation — unlike `collect_vars`, this doesn't build a set.

**expr_equal()** — Structural equality test with pointer shortcut. Used by the simplifier fixpoint loop — zero allocation per iteration, compared to the naive approach of converting both trees to strings and comparing.

**expr_to_string()** — Pretty printer with precedence-aware parenthesization. Only adds parens where needed for correctness.

**evaluate()** — Evaluates a fully numeric expression tree. Returns `Checked<double>`: empty (`!has_value()`) for structural failures (unresolved variable, unknown function, arg-count mismatch, `undefined` sentinel, null pointer). Division by zero also yields empty, via NaN sentinel — not a separate case. Built-in functions are dispatched via a static lookup table. Stays real-valued permanently — do not extend for complex or matrix types.

`Checked<T>` (expr.h:30-89) makes check discipline type-enforced rather than convention-enforced — the complement to the "exceptions for exceptional cases only" principle. `sizeof(Checked<double>) == sizeof(double)`; no hidden bool. Test with `has_value()` / `operator bool`; unwrap with `.value()` (asserts on empty in debug). `.value_or_nan()` is the named boundary escape for handing `double` off to the pure-numeric root-finder layer (`find_numeric_roots`, `adaptive_scan`, `newton_solve`, `bisection_solve`) which has its own `isfinite` discipline — its use should stay rare and grep-worthy. The `Checked(T v)` constructor is deliberately NOT `explicit`: `return some_double;` in `Checked<double>`-returning functions is load-bearing throughout the evaluate paths. Three markers at the declaration document this intent: `// cppcheck-suppress noExplicitConstructor` silences cppcheck, `/*implicit*/` is the human-facing signal, and trailing `// NOLINT(google-explicit-constructor)` silences clang-tidy. All three are load-bearing — remove any one and `make analyze-full` (clang-tidy) or `make analyze-fast` (cppcheck) will fire.

**evaluate_symbolic()** — Exact sibling of `evaluate()`. Returns an `ExprPtr` that preserves non-real structure (currently: integer rationals as `DIV(Num, Num)`). Used by the simplifier's constant-folding paths (`simplify_once_impl` BINOP num/num and FUNC_CALL all-numeric folds). This is the extension point for new number types — add complex or matrix dispatch here, not in `evaluate()`.

**fingerprint_expr(ExprPtr, free_vars, test_points)** — Schwartz–Zippel numeric fingerprint for semantic comparison. Substitutes free variables at test points and collects finite `evaluate()` outputs. Companion to `evaluate` and `evaluate_symbolic` as a tree-querying primitive. Used by `derive_all` dedup.

**canonicity_score(ExprPtr)** — Lex pair `{leaf_count, non_integer_num_count}` measuring expression complexity. Lower is simpler/more canonical. `leaf_count` is the primary key (size first); `non_integer_num_count` is the secondary tiebreaker (penalizes raw decimal literals). Integer `NUM` leaves are not penalized on the secondary key. Used by `derive_all` to sort output ascending — simplest formulas first — and to break ties when two candidates share a fingerprint.

**substitute()** — Replaces a named variable with an expression throughout the tree. Implemented via `tree_map_leaf`.

**substitute_builtin_constants()** — Tree walk; replaces Var nodes whose names appear in `builtin_constants()` (`pi`, `e`, `phi`) with their Num values. Used by the `--approximate` derive path before re-simplification. Other Var nodes pass through unchanged. Implemented via `tree_map_leaf`.

**`tree_map<Fn>` / `tree_map_leaf<Fn>`** — Two post-order tree-rewrite templates in `expr.h` (before the `substitute` section). Both use a pointer-equality short-circuit: if no child node changed, the original parent pointer is returned without reconstructing the node — zero allocations on the no-match path.

- `tree_map<Fn>(ExprPtr, Fn)` — calls `fn` on every node *after* its children have been rewritten. Use when the transform may match any node shape (interior or leaf). Current consumers: `cse_replace` (matches subtrees by `expr_equal`), `resolve_diff_calls` in `system.h` (matches `FUNC_CALL` nodes named `"diff"`).
- `tree_map_leaf<Fn>(ExprPtr, Fn)` — calls `fn` only on `NUM`/`VAR` terminals; passes interior nodes through structurally without invoking `fn`. Use when the transform targets only leaves and the per-node guard (`if (!is_var(node)) return node`) would otherwise be repeated at every call site. Current consumers: `substitute` (replaces `VAR` by name), `substitute_builtin_constants` (replaces `VAR` by builtin lookup), `expr_recognize_constants` in `fit.h` (signature: `const Expr*` first parameter — the call site in `system.h` passes a `const Expr*` directly; the single `const_cast` is inside `fit.h` at the `tree_map_leaf` boundary, documented with a comment explaining that no path inside mutates through the pointer).

`expand_for_var` is the explicit non-consumer: its MUL-distribution logic inspects the *reconstructed* `l`/`r` child shapes after recursion to decide whether to distribute — a post-recurse sibling dependency that cannot be expressed as a pure per-node lambda. It stays as a hand-written recursive function.

When adding a new tree pass, prefer a `.fw` rewrite rule first (per CLAUDE.md "simplification over filtration"). Reach for `tree_map`/`tree_map_leaf` only when the transform cannot be expressed as a pattern match on a static LHS. Baseline after T1: 5 consumers total; >7 without rule-equivalence justification triggers re-review (see Future.md T1 reopen triggers).

**simplify()** — Algebraic simplification, run to fixpoint (max 20 iterations, checked via `expr_equal`). Rules:
- Constant folding: `2 + 3 → 5`
- Constant reassociation: `(x + 2) + 3 → x + 5` (handles ADD±ADD, ADD±SUB, SUB±ADD, SUB±SUB, MUL×MUL in one unified block)
- Identity removal: `x + 0 → x`, `x * 1 → x`, `x^1 → x`
- Zero absorption: `x * 0 → 0`, `0/x → 0`
- Negation cancellation: `--x → x`, `x - (-y) → x + y`, `-(a - b) → b - a`
- Negation factoring via `simplify_neg_pair()`: handles `(-a)⊗(-b) → a⊗b`, `(-a)⊗b → -(a⊗b)`, `a⊗(-b) → -(a⊗b)` for both MUL and DIV in a single shared function
- Structural fractions: `Num(a) / Num(b)` preserved as `DIV(Num(a), Num(b))` when result is non-integer; GCD-normalized, sign in numerator. Rational arithmetic via `to_rational()` and `make_rational()` helpers. `flatten_multiplicative()` treats structural fractions as opaque factors.
- Negative-exponent normalization: `rebuild_multiplicative` splits the factor list by exponent sign — positive exponents go into a numerator product, negative exponents (sign-flipped) go into a denominator product — and emits `DIV(num, denom)` when any negative-exp factors are present. `MUL(a, POW(b, Num(-1)))` → `a / b`; `MUL(a, POW(b, Num(-2)))` → `a / b^2`. This is a rebuilder invariant, not a rewrite rule; it fires wherever `flatten_multiplicative` rebuilds a MUL chain.
- Data-driven rewrite rules (`BUILTIN_REWRITE_RULES` string, 26 rules): applied after the structural rules above via `apply_rewrite_rules`. The two Tier 1 additions (G1: `k * x / (k * y) = x / y iff k != 0` and G3: `x / (1 / y) = x * y iff y != 0`) close numeric-common-factor cancellation and unit-fraction-denominator rewriting respectively. Rules with `iff cond` evaluate `check_condition` against numeric bindings built from the current pattern match; comparison clauses with unbound wildcards return `nullopt` (treated as satisfied — permissive for symbolic unknowns). This is intentional: `sqrt(x)^2 = x iff x >= 0` simplifies `sqrt(a)^2` to `a` even when `a` has no known sign, because the alternative — leaving the `sqrt^2` wrapper intact — adds no information and blocks downstream fingerprint dedup. The semantic cost is accepted: the condition annotation documents the domain constraint without enforcing it for symbolic unknowns. Rules that must NOT fire for unknown-sign variables (e.g. `abs(x) = x iff x >= 0`) require a principled domain-propagation mechanism before they can be added; see Future #31.

  **Typed-binding predicates** (since 2026-05-10, Future #53; unified gen-5 cycle 3a 2026-05-15; user-defined sets gen-5 cycle 3b 2026-05-16): rule conditions additionally support typed predicates that test the runtime binding of a wildcard. Two canonical predicates: `is_neg_num(n)` (structural — negative numeric literal) and `is_in(v, set_name)` (membership — canonical for all type/set/dimension predicates since cycle 3a). `is_int(v)` and `is_in_dimension(v, dim)` are sugar: `parse_condition` (system.h) rewrites them to `is_in` form before `is_predicate_clause` runs. All are fail-safe: unknown or non-matching binding → false. `is_in` dispatches via `SetDef::Kind` switch in `check_condition` — 3 cases since cycle 3b: `BUILTIN_PREDICATE` → calls `sdef.membership(val.value_or_nan())` (`.value_or_nan()` is the deliberate NaN-sentinel boundary escape — NaN is meaningful for `imaginary`; comment enumerates 7 cooperating locations since cycle 3b); `DIM_SECTION` → calls `compute_dim(*bound_expr, set_ctx)` and compares the resulting `DimMap` to `{{set_name, 1}}` (map-equality; works for bare Vars and compound expressions since cycle 3c; returns false on nullopt mismatch sentinel); `USER_PREDICATE` → evaluates `sdef.predicate` with `sdef.parameter` bound to the queried ExprPtr, with a thread-local recursion guard keyed on set name. User-defined sets are declared via predicate sections (`[whole_number(n)] iff n >= 0 && is_in(n, int)`); `is_predicate_section` (system.h ~947) identifies the section shape; `register_predicate_section` (system.h) parses inline and multi-line bodies into a stored `Condition`. Encoding: `CondClause{lhs=FUNC_CALL(name, args), rhs=nullptr, op=CondOp::EQ}` — FUNC_CALL-in-lhs with null rhs sentinel. `is_predicate_clause(c)` checks for `is_neg_num` and `is_in` only. `check_condition` 4th param is `const SimplifyContext* set_ctx = nullptr` (was `const map<string,string>* dim_map`; `SimplifyContext` carries both `type_map_` and `set_definitions_` together). `RewriteRulesGuard` 5th-arg type changed accordingly; three construction sites in system.h build a stack-local `SimplifyContext` and pass its address. `BindingType`, `SetDef`, and `SimplifyContext` live in `expr.h` (moved from system.h in cycle 3a — full definitions required at `check_condition` call sites; forward declarations were insufficient). The predicate name set is `predicate_names()` in `expr.h`; extend it when adding a new predicate with a named consumer.

**Bounded aggregation — simplifier layer (since gen-6 cycle 1, 2026-06-22):**

Three primitives in `expr.h` implement the static-domain unroll:

- `gen_range_values(lo, hi, step) → vector<double>` — count-based value generation (avoids IEEE 754 drift from repeated addition); zero-step/empty-range guards return empty vector on degenerate input. Placed in expr.h upstream of all consumers; `parse_range` in system.h (the CLI batch-table range parser) was updated to call it, eliminating the prior divergence.
- `extract_range_values(rng_node, out&) → bool` — validates a `range(lo,hi)` or `range(lo,hi,step)` FUNC_CALL (arity 2 or 3), checks that all bounds evaluate numerically, fills `out` via `gen_range_values`. Returns `false` for symbolic bounds (caller leaves unevaluated). Empty domain is a *successful* extraction with empty `out` — the caller decides identity semantics.
- `is_aggregate_reducer(name) → bool` — 6-name predicate (`sum`/`product`/`count`/`max`/`min`/`mean`). Single source of truth shared by the simplifier, `extract_formula_calls`, and `extract_positional_calls`.
- `fold_aggregate(name, values, make_term) → ExprPtr` — shared reducer policy table covering all 6 reducers plus their empty-domain identities. `make_term` is a `std::function<ExprPtr(double)>` — the simplify path passes `simplify∘substitute`; the post-load path passes a FormulaCall cloner. Both paths use the same fold.
- `try_unroll_aggregate(name, sa) → ExprPtr` — called from `simplify_once_impl` FUNC_CALL branch BEFORE all-numeric function dispatch. Recognizes the 3-arg `{body, Var(iter), range}` shape (bodied) and the 2-arg `{Var(iter), range}` shape (count body-free). Delegates to `extract_range_values` + `fold_aggregate`. Returns `nullptr` on non-aggregate name, wrong shape, non-Var iterator, or symbolic bound.

`mean` uses `simplify(DIV(acc, Num(count)))` — the structural-fraction path; `mean(i, i in [1..4])` produces `5 / 2` (exact rational), not `2.5`. `max`/`min` track the winning ExprPtr via `evaluate()` numeric comparison; any non-numeric term returns `nullptr` (unevaluated). Empty domain: `count→0`, `sum→0`, `product→1`, `max/min/mean→nullptr` (unevaluated, not a crash).

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

**symbolic_diff(const Expr&, const std::string& var) → ExprPtr** — Free function in `expr.h`. Differentiates an expression tree with respect to `var`. Two-level dispatch:

- Per-AST-class switch: `NUM` → 0; `VAR` → 1 if name matches, 0 otherwise (builtin constants are treated as 0); `UNARY_NEG` → chain rule negation; `BINOP` inner switch covers ADD/SUB (sum rule), MUL (product rule), DIV (quotient rule), POW (general `f^g` formula collapsing via `simplify` to the `x^n` / `c^x` / `f^g` sub-cases).
- FUNC_CALL: registry lookup in `builtin_meta()` (Future #49, M3-extracted, see below). Per-builtin derivative `f'(u)` returned by the registry callback; chain rule (`* du/dvar`) applied at the call site. Nine builtins covered: sin → `cos(u) * u'`, cos → `-sin(u) * u'`, tan → `(1 + tan(u)^2) * u'`, asin → `u' / sqrt(1-u^2)`, acos → `-u' / sqrt(1-u^2)`, atan → `u' / (u^2+1)`, log → `u' / u`, sqrt → `u' / (2 * sqrt(u))`, abs → `abs(u)/u * u'` (requiring `sign` builtin + two rewrite rules).
- Returns `nullptr` for unknown or multi-arg FUNC_CALL nodes — the post-load pass treats `nullptr` as a "leave-symbolic" signal.

**symbolic_diff_simplified(const Expr&, const std::string& var) → ExprPtr** — Thin wrapper that calls `symbolic_diff` then `simplify()`. Use this at call sites that want a canonical result; use `symbolic_diff` directly when the caller will do its own simplification pass. Called once per query by `try_resolve_numeric` (`system.h`) before `find_numeric_roots`: if non-null, the result is wrapped in a `std::function` lambda and passed as the optional `fp` parameter to Newton's method, giving quadratic convergence (2 evaluations per iteration instead of 3). If `symbolic_diff_simplified` returns `nullptr` (unrecognized function), `fp` stays null and Newton falls back to central finite-differences transparently.

**sign(x)** — New builtin registered in `builtin_functions()` (expr.h). `sign_eval` numeric evaluator returns −1, 0, or +1 per IEEE-754 sign comparison; NaN propagates. Symbolic-only intent: appears in derivative of `abs(x)` as `abs(x)/x = sign(x) iff x != 0`.

**resolve_diff_in_equations** (system.h) — Post-load pass hooked into `load_with_sections()` after `compute_rewrite_groups()`. Recursively rewrites `diff(target, var)` FUNC_CALL nodes in every equation's RHS. Three target shapes handled:
1. `diff(named_var, x)` — substitutes the named variable's equation RHS and differentiates.
2. `diff(formula_call_placeholder, x)` — `unfold_formula_call_body()` inlines the FormulaCall body, then differentiates the inlined tree.
3. Literal expression — differentiates directly.

Throws `std::runtime_error` on `diff(<non-var-non-formula-call>, ...)` per design. Running after rewrite rules load ensures `simplify()` inside `symbolic_diff_simplified` can apply all rules (including the three new `x^a/x^b`, `abs(x)/x`, and `sign` rules) to the differentiated result.

**Symbolic integration (`integral(f, x)`) — Tier 1 indefinite + M2 u-sub & definite + M3 IBP/LIATE, Future #16 M1+M2+M3 (2026-05-10)** (sibling of Symbolic differentiation above; same structural pattern throughout)

`symbolic_integrate(const Expr&, const std::string& var) → ExprPtr` — Free function in `expr.h`, sibling of `symbolic_diff`, mirroring its return-on-`nullptr`-on-miss contract. **`e^x` antiderivative convention:** result is `BinOpExpr(POW, Var("e"), Var(var))` — the same `e^x` FUNC_CALL sugar is not used; consumers that pattern-match on `e^x` must match this POW form. Per-AST-class switch + `BuiltinMeta` registry lookup for FUNC_CALL (M3-extracted, Future #49). Tier 1 covers ~25 atomic patterns:

- `NUM` (constant `c`) → `c*x`. `VAR` → `x^2/2` if name matches `var`, else `name*x`.
- `BINOP::ADD/SUB` — linearity (`∫(l ± r) = ∫l ± ∫r`).
- `BINOP::MUL` — `c*f` (one operand constant w.r.t. `var`); both-contain-var triggers **M2 derivative-divides u-substitution** (see below) followed by **M3 IBP** if u-sub fails.
- `BINOP::DIV` — `f/c`, `c/x`, `1/x → log(x)`. **M3 additions:** `c / (k * Var(var))` → `(c/k) * log(Var(var))`; both-contain-var dispatches to `try_u_sub_integrate` (handles `x/(x^2+1) → log(x^2+1)/2`, the recursive form needed for `atan(x)` IBP).
- `BINOP::POW` — `Var(var)^n` (`n` numeric, `n ≠ -1`) → `x^(n+1)/(n+1)`; `Var(var)^(-1) → log(x)`; `e^Var(var) → e^Var(var)`.
- `UNARY_NEG` — integrate child, negate.
- `FUNC_CALL` (single-arg only, arg must equal `Var(var)` — chain rule via u-sub at MUL): registry lookup in `builtin_meta()`. Three table entries today (sin → -cos, cos → sin, tan → -log(cos(x))). When the registry has no entry AND the function sits at L or I in LIATE (rank ≥ 4 — `log`, `asin`, `acos`, `atan`), the FUNC_CALL is synthesised as `f(x) * 1` and dispatched to IBP — handles `∫atan(x) dx → x*atan(x) - log(x^2+1)/2`, `∫log(x) dx → x*log(x) - x`, etc.

Anything outside this list returns `nullptr`. The wrapper `symbolic_integrate_simplified` calls `simplify()` after; null propagates.

**Unevaluated-fallback contract:** when `symbolic_integrate` returns `nullptr`, the post-load pass `resolve_integral_in_equations` keeps the original `integral(target, var)` FUNC_CALL in place — same convention as diff. The result is observable to downstream stages (e.g., `--steps` traces, output round-trip) as a literal `integral(...)` form, signalling "no rule matched."

**Out of scope** (deferred or future cycles): cyclic IBP detection (`e^x*sin(x)` family — depth limit catches it; reopen trigger: user reports family unevaluated in real reproducer AND cleanly-layered detection mechanism identified), `+ C` constant of integration (never — would not round-trip), domain-aware antiderivative `log(abs(x))` (gated on Future #31 condition propagation), Risch / improper / multi-variable / trig substitution / partial fractions / special functions (per cross-arc reopen triggers in `.fwiz-workflow/master-plan.md`).

**M2 — Derivative-divides u-substitution** (`try_u_sub_integrate` in expr.h, called from MUL when both factors mention `var`): enumerates candidate sub-expressions `g(x)` of the integrand to depth `U_SUB_DEPTH = 2`, sorted ascending by leaf count (simplest first — avoids `log(e)` artifacts when a chain-rule POW derivative would be picked). For each `g`, computes `g_prime = symbolic_diff_simplified(g, var)`; calls `try_cancel(integrand, g_prime)` to symbolically divide; substitutes `g → Var("_u_sub_")` in the residual via `cse_replace`; if no `var` remains, integrates w.r.t. `_u_sub_` and back-substitutes. Returns the simplified result on first match, `nullptr` if no candidate cancels cleanly. The root expression itself is excluded from candidates (cancelling against your own derivative is degenerate). `try_cancel(expr, factor)` is the matching primitive: returns `simplify(DIV(expr, factor))` if no subtree of the result equals `factor` structurally; else `nullptr` (heuristic — perfect cancellation is hard).

**M3 — Integration by parts via LIATE** (`try_ibp_integrate` in expr.h, called from `symbolic_integrate`'s MUL branch after u-sub returns null, AND from FUNC_CALL when no antiderivative-table entry exists for an L/I-rank function). LIATE priority (the `liate_priority(expr, var)` helper): Logarithmic (`log`) → 5; Inverse-trig (`asin`/`acos`/`atan`) → 4; Algebraic (`Var(var)`, `Var(var)^n`, `c*Var(var)`, var-free) → 3; Trigonometric (`sin`/`cos`/`tan`) → 2; Exponential (`e^Var(var)` POW form) → 1; anything else → 0. For a MUL integrand, the operand at HIGHER LIATE rank becomes `u`, the other becomes `dv`; at rank-tie or both-zero, IBP declines (no preference). Algorithm: `V = symbolic_integrate(dv, var)`; `du = symbolic_diff_simplified(u, var)`; recursively `int_V_du = symbolic_integrate(V * du, var)`; result = `simplify(u*V - int_V_du)`. **Depth limit ≤ 3** enforced via thread-local `ibp_depth_` counter — bounds recursion without explicit cyclic detection (the `e^x*sin(x)` family blows the depth limit on the recursive call and bails out, returning `nullptr`; the unevaluated `integral(...)` FUNC_CALL is preserved by the post-load pass). Render-order tuning: when `V` is a structural fraction (DIV node), the result builds `V_num * u / V_denom` to preserve the rational form (avoids the simplifier flattening `(x^3/3) * log(x)` to `0.333 * log(x) * x^3`); when `V == Var(var)` and `u` is a FUNC_CALL, swap to `V * u` (algebraic-before-function — `x * atan(x)` not `atan(x) * x`); else plain `MUL(u, V)`. The `mul_through_div(a, b)` helper centralises the structural-fraction-preserving multiply.

**M3 — `BuiltinMeta` registry** (`builtin_meta()` in expr.h, Future #49 — DONE 2026-05-10). Per-builtin metadata table consolidating `symbolic_diff` and `symbolic_integrate`'s formerly-duplicated FUNC_CALL if-chains. Schema:
```cpp
struct BuiltinMeta {
    using DiffFn = ExprPtr (*)(ExprPtr u);              // returns f'(u); chain rule applied at caller
    using IntegrateFn = ExprPtr (*)(const std::string& var);  // returns ∫f(var) dvar; arg must equal Var(var)
    DiffFn diff;
    IntegrateFn integrate;  // nullptr signals "no table entry — try IBP or fall through"
};
```
Nine current entries (sin/cos/tan/asin/acos/atan/log/sqrt/abs); three carry antiderivative entries (sin/cos/tan), the rest leave `integrate == nullptr` so the IBP layer or unevaluated-fallback handles them. Free `*_diff` and `*_integrate` helper functions live immediately above the registry; the registry returns a `const map<string, BuiltinMeta>&` to a function-local static. Future consumers (Future #7 units, #9 LaTeX) plug in by extending `BuiltinMeta` with new fields. **Why C++ today, not `.fw` rules**: typed-binding predicates (Future #53) are required to express the antiderivative table's pattern guards (`Var(var)^n iff is_num(n)`). `BuiltinMeta` is the **4th consumer** of #53 (after T3.5 `simplify_div`, T3.6 `x^(-n)`, integration Tier 1). Migration to `.fw` rules waits on #53 shipping.

**M2 — Definite integrals (4-arg form)**. CLI parsing in `parse_cli_query` (system.h) accepts both `integral(f, x)` and `integral(f, x, a, b)` — the inner-comma split tracks paren AND bracket depth (mirror of `parse_call_args` post-Cycle-B), takes either 2 or 4 pieces, anything else is a parse error. Bounds are encoded as text in the synthesised equation string (e.g. `alias = integral(f, x, 0, 3)`) so the post-load pass receives the full 4-arg form. `resolve_integral_calls` (system.h) dispatches the 4-arg form: try the symbolic path (compute antiderivative `F`, return `simplify(F(b) - F(a))`); if `evaluate` collapses the difference to a finite number, return `Num(value)`; if it stays symbolic (free vars in bounds), return the symbolic difference. Numeric fallback: `adaptive_simpson` (expr.h, sibling of `newton_solve`) — recursive Simpson's rule with bisection error estimate `|S(a,b) - S(a,m) - S(m,b)| / 15`, tolerance defaults to `NUMERIC_TOLERANCE`, depth bounded by `ADAPTIVE_SIMPSON_MAX_DEPTH = 30`. NaN at any sample short-circuits to NaN → caller preserves the unevaluated FUNC_CALL.

**`resolve_at_load(rewriter, up_to)` primitive** (Future #48, system.h) — Generic post-load tree-rewriting loop extracted at M1. Both `resolve_diff_in_equations` and `resolve_integral_in_equations` are 4-line wrappers around it. Subsequent post-load passes (e.g., units #7, LaTeX hints #9, typed-binding rewrites once #53 ships) plug in here. The `up_to` dirty-flag pattern (`diff_resolved_up_to_`, `integral_resolved_up_to_`) ensures a second `load_string` (e.g., the CLI's synthesized `<alias> = integral(...)` injection) only walks the new tail.

**CLI surface:** `integral(target, var)=?[alias]` and `diff(target, var)=?[alias]` are synthesised by `parse_cli_query` as `<alias> = integral/diff(...)` equations in `CLIQuery::synthetic_equations`, loaded via a single `sys.load_string` call after the file/inline source (Future #67). The standard post-load passes and query loop handle alias resolution — no parallel dispatch layer. **No `--integrate` flag** — the in-file form is the single surface (see Future #64 for the deferred-flag rationale).

**Dependency on Future #53:** `Var(var)^n` with `n` constant is the pattern that motivates `is_num(...)` typed-binding predicates. Tier 1 antiderivative table was the 3rd consumer; the M3 `BuiltinMeta` registry (shipped 2026-05-10) is the **4th consumer** — pulling the diff/integrate metadata toward `.fw`-rule definability is gated on #53. Until #53 ships, `BuiltinMeta` carries C++ function pointers; migration to `.fw` rules is a future cycle.

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
- Rewrite rules with malformed `iff` conditions are **dropped at load time** with a stderr warning (`"warning: dropping rewrite rule '…' — malformed condition: …"`). A parse-failed rule is never pushed into the rule set, so it cannot accidentally be treated as unconditional. Sub-systems loaded via `load_sub_system` inherit the parent's `approximate_mode` at construction time (alongside `numeric_mode` and `custom_functions_`).
- **Cross-file cycle detection** (since 2026-05-13): `load_sub_system` maintains a thread-local `currently_loading` set keyed on the cache key (file path + section name). Re-entrant loads throw `CrossFileResolutionCycleError` — a sibling exception (not `std::runtime_error`) — with message `"Cross-file resolution cycle: <name> recursively loads itself"`. A RAII `LoadGuard` erases the entry on both success and exception paths. This prevents SIGSEGV when a `.fw` file's body calls a function that shadows its own filename (e.g. `matmul.fw` containing `matmul(A, B)`).
- **`@include` + strict-includes** (M1–M3, 2026-06-23; Future #80 DONE): `process_includes()` is a pre-pass in `load_with_sections()` (runs before `split_sections`) that resolves each `@include "path.fw"`, recursively `load_file`s it, records the canonical path in `included_files_`, and blanks the line. `resolve_file_path()` implements the search order (file-relative `base_dir` → `-I` `include_dirs` → `FWIZ_PATH`); its `exclude_base_dir` param is set true only by the strict `load_sub_system` branch. `strict_includes_` (bool on `FormulaSystem`) now **defaults to `true`** — a cross-file call resolves only via the `@def:` cache, the `@include` allow-list (`resolve_from_included()` — stem-scan of `included_files_`), or the search path; the implicit base_dir co-location probe is skipped, and a callable file must declare an explicit `[name(args)->ret]` section. A miss throws `StrictIncludeError` (sibling exception, not `std::runtime_error`) so the "add `@include`" hint survives the solver's silent `catch (const std::runtime_error&)` sites. `--legacy-implicit` sets `strict_includes_ = false` (one-release opt-out). The `extract_positional_calls` catch site uses the `is_postload_builtin(name)` discriminator (in `expr.h`, next to `is_aggregate_reducer`): on `StrictIncludeError` it returns the node for the post-load pass / simplifier when the name is a post-load/simplifier builtin (`diff`/`integral`/`range`/`vec`/`mat`/`matmul`/`det`/`inv`/`transpose`), and rethrows otherwise so genuine un-`@include`'d cross-file calls still surface the hint. All include-related fields (`include_dirs`, `included_files_`, `strict_includes_`) propagate to sub-systems via `copy_metadata_to_sub`. **Maintenance invariant:** any new simplifier or post-load pass that handles a `FUNC_CALL` name not in `builtin_functions()` and not in `is_aggregate_reducer()` MUST be added to `is_postload_builtin()` (`expr.h`), or strict mode will mistake inline uses of it for missing cross-file calls and throw `StrictIncludeError`.

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

`std::string source_label_` on `FormulaSystem` — set to the file stem on `load_file` and to the explicit label on `load_string`. Two entry points for alias resolution: `populate_aliases_()` (side-effect mutator, `void` return — called at `resolve()` / `resolve_all()` entry and from `main.cpp` to prime the table before output) writes the computed map into `aliases_`; `build_alias_table()` (pure query, `[[nodiscard]] std::map<string,double>` return — called by `format_derived` / `derive_all` when the table is needed as a value) calls `populate_aliases_()` then returns a copy. Both walk `this->defaults` and each sub-system's `defaults`, group constants by name, emit them unqualified when all files agree on the value (within `EPSILON_REL`), and emit `stem.name` qualified forms when the same name carries different values across files. Built-in constants (`pi`, `e`, `phi`) are never entered into the user alias table. `fmt_solve_result` (main.cpp) and `format_derived` (system.h) both thread the table into `fmt_exact_double`. Side-effect-only call sites use `populate_aliases_()` directly rather than `(void)build_alias_table()` — the trailing underscore convention marks it as an internal mutator.

`derive_all` dedup pipeline — after collecting raw candidates, a streaming `std::map<fp_key, {score, ExprPtr}> winners` retains at most one candidate per semantic fingerprint. Two semantic primitives in `expr.h` drive this:

- **`fingerprint_expr(ExprPtr, free_vars, test_points)`** — Schwartz–Zippel numeric fingerprint: substitutes all free variables at each test point, calls `evaluate`, collects finite values; returns an empty vector when all test points lie outside the expression's domain. Test points use per-variable prime cycling `primes[(i+j)%3]` with `primes={2,3,5}`, keeping magnitudes small enough to avoid triangle-inequality violations.
- **`canonicity_score(ExprPtr)`** — lex pair `{leaf_count, non_integer_num_count}`. Integer `NUM` leaves are not penalized on the secondary key, so `2*pi` scores the same as `pi`. Lower score wins; ties are broken in favour of the form already in `winners`.

Candidates with empty fingerprints (all test points domain-excluded) fall back to a format-string sentinel: structurally-different always-NaN expressions stay separate; string-identical ones collapse. Sentinel-bucket candidates sort after real-fingerprint candidates because their discriminator byte is `1` vs `0` for real fingerprints — `std::map` ordering ensures real results appear first without any filtering.

The `derive_all` emit loop sorts all winners ascending by `canonicity_score` before output, so the simplest formula is always first. `--derive N` (N ≥ 1) caps the final result list at N entries after sorting. The `free_vars` list used for fingerprinting is populated from the values (not keys) of `symbolic_bindings`, aligning with the variable names that actually appear in derived expressions after alias substitution.

**CSE pass (`--cse [N]`)** — opt-in extension to `derive_all`. Default N=3 (when `--cse` is bare). Semantics: extract at most N helpers, ranked by value. Two free primitives:
- `cse_extract(exprs, cap, occupied)` (system.h, before the class) walks each expression, counts non-atomic subtrees by stringification, filters to those with ≥ 2 occurrences AND at least one free Var, computes `value = (count-1)*(leaves-1)` per candidate (where `leaves` counts Var/Num atoms plus FUNC_CALL function names — the printed-token count), sorts by value descending, takes the top `cap`, then re-sorts the survivors topologically (node-count ascending) so dependencies are emitted before parents. Single-leaf atoms have value 0 and are never extracted. Names `t1, t2, ...` skip any `occupied` name. `node_count` and `leaf_count` are computed by a single shared `tree_counts` walker that returns both metrics together, backed by one `std::map<ExprPtr, TreeCounts>` cache — one map lookup per node, both metrics returned. Comparators do O(log N) lookups, not O(depth) recursion.
- `cse_replace(e, helpers)` (expr.h) walks `e` post-order, replacing structural-equal subtrees with `Var(helper_name)`. Pointer-equality short-circuit on the no-match path returns the original `e` when no child changed and no helper matched, avoiding the O(|tree|) rebuild that would otherwise come from fwiz's factory pattern.

Inside `derive_all`: the cap is applied BEFORE the CSE pass (so helpers reflect printed equations only); when CSE is active each winner is pre-canonicalized via `simplify(distribute_over_sum(e))` (mirroring what `format_derived` does internally) BEFORE counting — gated so the no-CSE path stays zero-overhead; the occupied set unions `all_variables()`, every section's positional args + return_var, the target, the symbolic_bindings keys + values, the numeric_bindings keys, and `pi/e/phi`. Helpers themselves are formatted with each helper's RHS `cse_replace`'d against earlier helpers, producing nested forms like `t2 = sin(t1)` (D8 invariant).

Results validated — NaN and infinity rejected, causing fallback to next equation.

Error messages are specific: "No equation found for 'x'", "no value for 'y'", "all equations produced invalid results".

**Bounded aggregation post-load pass (since gen-6 cycle 1, 2026-06-22):** `resolve_aggregate_in_equations()` — a 4-line `resolve_at_load` wrapper (same pattern as `resolve_diff_in_equations` and `resolve_integral_in_equations`), called from `load_with_sections` between the integral pass and `trace_loaded`. Drives `resolve_aggregate_calls(e)` — a `tree_map` post-order walk that dispatches on `is_aggregate_reducer`. The core is `try_unroll_aggregate_with_calls(node)`:

- **Shape A (explicit iterator, positional body):** body after substitute is a FUNC_CALL → `extract_positional_calls` per term. Handles `sum(score(roll), roll in [1..6])`.
- **Shape A-named (explicit iterator, named-binding body):** body after substitute is a bare `Var("_fcN")` referencing a pre-extracted FormulaCall whose bindings contain the iterator → `clone_call_with_subst` per domain value, erasing the template call from `formula_calls` (orphan-erase, required for correct reverse-solve). Handles `sum(dmg(atk=f, def=k), f in [1..6])`.
- **Shape B (broadcast):** body contains a FormulaCall with exactly 1 range-literal binding → clone N times with `Num(v)` substituted for the range; lockstep (second arg equals the range param Var) substitutes together. Handles `sum(combat(atk=[1..6], def=atk, dmg=?))`. 2+ ranges → leave unevaluated + stderr warning.

`agg_resolved_up_to_` dirty-flag member ensures a second `load_string` only walks the new tail (same `resolve_at_load` contract). `extract_formula_calls` and `extract_positional_calls` both have `!is_aggregate_reducer` guards so reducer nodes are not mistaken for cross-file function calls.

**Collections primitives — `seq` / `map` / `foldl` (gen-6 Collections Cycle 1, 2026-06-24):**

- `seq(e0, e1, ...)` — ordered-collection FUNC_CALL node produced by `{1,2,3}` parser sugar (LBRACE/RBRACE tokens, `static_assert` advanced). Distinct from `vec`: `vec` participates in `try_simplify_vec_mat_binop`/`try_dispatch_vec_mat_builtin` (element-wise and matrix dispatch); `seq` does not — conflating them would make `{1,2,3}+{4,5,6}` silently element-wise-add. `seq` is guarded by a passthrough in `simplify_once_impl` before `lookup_function`. `extract_range_values` extended to recognize all-numeric `seq(...)` nodes as iterator domains.
- `map(body, Var(iter), range)` — materialized at simplify-time to `seq(...)` for numeric domains; symbolic domain stays unevaluated. Post-load pass `resolve_map_in_equations` handles formula-call bodies. `unroll_term_with_calls` is a shared helper (factored from `try_unroll_aggregate_with_calls` Shape A) used by both the map pass and the aggregate pass.
- `foldl(coll, Var(op_name), init)` — POST-LOAD-ONLY via `resolve_foldl_in_equations` (4-line `resolve_at_load` wrapper). `lookup_binary_op_body(op_name)` looks up a two-parameter section `[op(acc,elem)->r]` by name and threads `(acc, elem)` through its body per element. Uniform for all ops: `add`, `mul`, and user-defined `max2`/`min2` all use the same path. `stdlib/collections/operators.fw` defines `add` and `mul` sections. No `named_op_to_binop` BinOp table.
- **Post-load pass order:** `resolve_map_in_equations` → `resolve_foldl_in_equations` → `resolve_aggregate_in_equations`. This order is load-bearing: aggregate must run last so `sum(map(score(i), i in [1..3]))` sees a materialized `seq` from the map pass before folding.
- `seq`, `map`, `foldl` added to `is_postload_builtin` so strict-includes mode does not mistake inline uses for missing cross-file calls.
- **Cross-cycle invariant (AE-3):** a 2-arg op section must be in the SAME FILE as the `foldl` that references it (or pre-registered in `custom_function_defs_`) until multi-section `@include` persistence is fixed (Future #110). Workaround: one-section-per-file where the file stem matches the section name (M3-aligned pattern).

**Strategy 6 emission widening:** `formula_call_bindings_contain(expr, var) const` (new private const member, system.h) returns true if any FormulaCall whose output var appears in `expr` has a binding expression that contains `var`. Added to Strategy 6's emission predicate (`const bool target_in_fc_bindings`) as the last `&&` short-circuit. This is necessary because after post-load unroll, the solve target (e.g. `k`) lives inside FormulaCall bindings (`def=k`), not in the equation RHS expression text (`_fc0+...+_fc5`). The system-probe fallback in `try_resolve_numeric` already handles this naturally; the widening just ensures the candidate is emitted. `resolve_all` is the sound path for piecewise-body reverse solves (Future #102).

**Table mode (`--table`, `--zip`)** — batch evaluation across range inputs; output as TSV. `parse_range(const std::string&) → vector<double>` (system.h, free function before `parse_cli_query`) parses `[start..stop]`, `[start..stop @ step]`, and compound `[r1, r2, ...]` forms. Bounds accept literal numbers or arbitrary expressions via the existing `Parser + evaluate` idiom. Count-based value generation (`start + i*step`) avoids IEEE 754 drift from repeated addition. `CLIQuery::range_bindings` (`vector<pair<string, vector<double>>>`, CLI-order-preserving) carries the expanded sequences. The iteration driver lives in `main.cpp`: cartesian product (odometer, rightmost-fastest) or `--zip` (element-wise to min length). Mutual exclusion: `--table` cannot combine with `--derive`, `--verify`, `--fit`, `--explore`, or `--explore-full`. `--steps`/`--calc` trace is suppressed in table mode to keep the TSV stream clean. The comma-splitter in `parse_cli_query` was extended to track `[]` depth (alongside `()`) so compound range args such as `a=[1..5, 6..10]` are not split at the inner comma.

### fit.h

Curve fitting: `sample_function`, `fit_base`, `fit_all`, template functions, `recognize_constant`. Also hosts two output helpers shared with the solve/derive pipeline:

**`fmt_exact_double(double v, aliases={})`** — the single formatter for exact numeric output. Wraps `expr_recognize_constants(Expr::Num(v))` and stringifies the result; falls back to `fmt_num` when nothing matches. Accepts an optional `aliases` map (name → value) so callers can inject file-specific constants (e.g. `deg=pi/180`) that render as their names rather than raw decimals. Used from `fmt_solve_result` (main.cpp) and `format_derived` (system.h). `RECOGNIZE_FRACTION_MAX_DEN` (fit.h, currently 360) governs the denominator ceiling for rational recognition.

`recognize_constant` uses two file-scope statics — `sqrt_log_constants()` (greppable `std::array` of `{value, name}` pairs for sqrt/log constants) and `base_recognition_constants()` (a merged `std::map` combining builtins and sqrt/log constants, built once at static initialization). Per-call iteration performs a zero-allocation 2-way merge between `base_recognition_constants()` and the caller's `extra_constants` (both already sorted), preserving the original alphabetical iteration order that downstream fingerprint dedup depends on. No heap allocation per call.

### Symbolic provenance carrier (`solved_symbolic_`, `aliases_`)

`FormulaSystem` carries two parallel maps alongside the main numeric `bindings` (`map<string, double>`) to support exact-form trace output:

- **`mutable std::map<std::string, ExprPtr> solved_symbolic_`** — stores the recognized symbolic ExprPtr for each bound variable. Written at T10 (`try_resolve`, `src/system.h`) when a result is committed: `expr_recognize_constants(simplified, aliases_)` is applied once to the solver's `simplified` ExprPtr and the result is stored here. Also populated from the sub-system bridge at T7 (cross-formula results). Cleared at the top of `resolve()` and `resolve_all()` to reset per-query, matching the per-query lifecycle of the numeric `bindings` parameter.

- **`mutable std::map<std::string, double> aliases_`** — the universal alias-resolution table, populated as a side effect of `populate_aliases_()` (and therefore also by `build_alias_table()`, which calls it). Read by `fmt_trace` and `fmt_exact_double` to resolve user-defined constants (e.g. `deg`) by name. Named `aliases_` rather than `display_aliases_` to signal its role as a general primitive, not a display-only concern.

**Invariant:** for any successfully solved variable `k`, `bindings[k]` (the numeric result) and `solved_symbolic_[k]` (the symbolic form) are written together at T10. Both are cleared together at the start of each `resolve()` call.

**Single render helper:**

```cpp
// system.h, private on FormulaSystem
std::string fmt_trace(double v, const Expr* sym = nullptr,
                      const std::string& key = "") const;
```

- `--approximate` → `fmt_num(v)` always
- sym provided → `expr_to_string(sym)` (already-recognized form stored at write)
- key provided → `solved_symbolic_.find(key)`, then `expr_to_string` on hit
- fallback → `fmt_exact_double(v, aliases_)`

Every `--steps`/`--calc` trace site calls `fmt_trace`, so trace and final output share the same symbolic form by construction. The recognizer (`expr_recognize_constants`) runs once per binding at write time, not once per trace line — cost is O(1) per query, not O(trace_lines).

**Sub-system bridge (T7):** after `sub_sys.resolve()`, the parent looks up `sub_sys.solved_symbolic_[resolve_var]` and adopts the ExprPtr into its own `solved_symbolic_[target]`. Sub-system arenas are held via `shared_ptr` for the parent's lifetime, so the pointer remains valid. When Future #20 (typed FORMULA_CALL nodes) ships, this bridge can be deleted and replaced by typed-node evaluation.

### Periodic families (`PeriodicFamily`, `ValueSet::periodic_`)

`struct PeriodicFamily { double base; ExprPtr period; }` (expr.h) represents one branch of a trig solution family parameterized by integer `k` — e.g. `pi/6 + k * 2*pi`. `ValueSet` carries a `std::vector<PeriodicFamily> periodic_` field alongside its existing `intervals_` and `discrete_` members; `has_periodic()` / `periodic()` expose it. `ValueSet::to_string()` renders each surviving family as `<base> + k * <period>, k in Z` (ASCII), deduplicating at render time by checking whether `(b1 - b2) mod period ≈ 0` for each new family against already-kept ones. `trig_period(fn_name)` (src/system.h) is a symbolic-table lookup returning `2*pi` for sin/cos or `pi` for tan. `detect_trig_origin(target, equations)` (src/system.h) scans the equation set to determine whether the target variable comes exclusively from a single named-trig builtin, enabling the `resolve_all` hook that promotes discrete results to a `PeriodicFamily` when the source matches. `FuncInverter` was widened from `std::function<ExprPtr(...)>` to `std::function<std::vector<ExprPtr>(...)>` to support multiple inverse branches per builtin (sin/cos now each have two); `solve_by_inversion` iterates all returned branches and concatenates solutions. Deferred concerns (set algebra on `periodic_`, gap-based detection, round-trip parsing, `--derive` format) are tracked in Future.md #12a-g.

### Complex numbers (`i`)

The imaginary unit `i` ships as a Fwiz-wide builtin constant with a quiet-NaN binding, registered alongside `pi`, `e`, `phi` in `builtin_constants()` (`src/expr.h`). Two integration points:

- **`evaluate()`**: the NaN-as-empty contract on `Checked<double>` (`src/expr.h:30-89`) collapses NaN to empty automatically, so `evaluate()` on any `i`-containing expression returns empty `Checked<double>{}`. No new code path; `i` reuses the existing domain-failure surface.
- **Pattern matcher**: the literal-match guard at `src/expr.h:838` already consults `builtin_constants().count(name)`, so `i` cannot bind as a wildcard in any rewrite-rule LHS. A pattern naming `i` matches only the literal symbol `i`.

Symbolic identity ships as a rewrite rule in `BUILTIN_REWRITE_RULES` (`src/system.h`):

```
i * i = -1
i ^ 2 = -1
```

Both forms are present because the simplifier's multiplicative flattening canonicalizes `i * i` → `i^2` (POW form) before any rewrite rule sees it; `i ^ 2 = -1` is the form that actually fires after canonicalization, while `i * i = -1` is defensive for paths that may bypass flattening.

**Branch-cut convention** (principal value): when complex identities ship, `sqrt(-1) = i`, `log(-1) = i * pi`, `arg(z) ∈ (-π, π]`. Currently only the `i^2 = -1` identity ships (two rules — `i * i = -1` and `i ^ 2 = -1` — cover both pre- and post-flatten forms). Other identities (Euler, conjugate product, complex `sqrt`/`log`) are aspirational and depend on future rule additions.

**LLM-ergonomics**: complex-containing expressions return empty from `evaluate()` — the same surface as any other domain failure (unresolved variable, unknown function). To distinguish "complex result" from "evaluation failed", check the simplified expression's string form for the literal token `i`. The `--approximate` mode does **not** substitute `i` to a numeric form (it would just produce NaN).

**Out of scope**: `ExprType::COMPLEX` leaf (deferred — reopen when profiling shows complex arithmetic >5% of `simplify` time); numeric complex root-finding (Strategy 6 stays real-only); the `Var("i")` shows up in `collect_vars` as a free variable, which is harmless because no equation defines `i` unless a user authors one (and that's a user error).

### Dotted variable names (`car.velocity.x`)

Dotted identifiers like `car.velocity.x` are tokenized as a **single** `IDENT` token by the lexer (`src/lexer.h:82-89`): after the initial alphanumeric run, additional `.IDENT` segments are appended to the same token whenever a dot is followed by an alpha character. They are pure **syntactic sugar** — internally `car.velocity.x` is just `Var("car.velocity.x")`, with the dots being legal name characters that flow through the entire pipeline (parser, simplifier, `FormulaSystem.parse_line`, `parse_condition`, `expr_to_string`) without any special handling.

There is **no struct/record machinery**: no namespace resolution, no field-access infrastructure, no hierarchical lookup table. The dotted name acts as one flat key in the `bindings` / `defaults` / equation maps. This means user-facing equations like `speed = sqrt(car.velocity.x^2 + car.velocity.y^2)` work end-to-end, and dotted names compose under arithmetic, function calls, and global conditions (`car.speed > 0`) the same way single-segment names do.

**Out of scope**: a real structural `ExprType::STRUCT` / `DOT_ACCESS` node type (Future.md #15) is deferred. Reopen trigger: a concrete user need for cross-field invariants, type-checking on dotted shapes, or namespace-scoped resolution that flat naming cannot express.

### Vectors and matrices (`vec`/`mat` sugar)

Vectors and matrices ship as **`FUNC_CALL` sugar** — no new `ExprType`. Internally:

- `[1, 2, 3]` parses to `FUNC_CALL("vec", {Num(1), Num(2), Num(3)})`.
- `[[1, 2], [3, 4]]` parses to `FUNC_CALL("mat", {vec(1,2), vec(3,4)})` — each row is itself a `vec` call.
- Empty `[]` parses to `vec()` (zero-element vector). The promotion to `mat` only fires when EVERY element is itself a `vec` AND the list is non-empty.
- `expr_to_string` special-cases `name == "vec" || name == "mat"` to render with brackets; the recursive call on each `args[i]` produces the nested `[[...], [...]]` shape naturally for `mat`.

**Element-wise add/sub/scalar-mul**: a single hook `try_simplify_vec_mat_binop` (in `expr.h`) runs from `simplify_once_impl`'s BINOP branch BEFORE the standard scalar dispatch. It handles three cases: (1) `BINOP(ADD/SUB, vec, vec)` and `BINOP(ADD/SUB, mat, mat)` with matching arity → element-wise; mismatched arity → `Var("undefined")`; (2) `BINOP(MUL, Num, vec/mat)` and the commuted form → element-wise scaled. `MUL(vec, vec)` and `MUL(mat, mat)` fall through (no element-wise multiplication of vectors — use `matmul` explicitly).

**Ragged-literal parse-time validation** (since 2026-05-13): `parser.h` validates that all rows in a matrix literal have the same column count immediately after the `all_vec` test. On mismatch the parser throws `RaggedMatrixError` naming the first divergent row: `"Ragged matrix literal: row 0 has 2 columns, row 1 has 1 column"`. `RaggedMatrixError` is a sibling exception (derives from `std::exception`, not `std::runtime_error`) so per-line `load_lines` warning catches do not swallow it — see Error handling section. Uniform shapes and symbolic elements (all rows same length) pass through unchanged.

**Multi-arg builtins** (`matmul`, `det`, `inv`, `transpose`): dispatched in the FUNC_CALL branch of `simplify_once_impl` via `try_dispatch_vec_mat_builtin` after children are simplified. Scope per design §M3:

- `det`: 2x2 closed form (`a*d - b*c`) and 3x3 cofactor expansion. Larger → `undefined`.
- `inv`: 2x2 only. Other shapes or singular determinant → `undefined`.
- `matmul`: arbitrary R×K × K×C → R×C. Inner-dim mismatch → `undefined`.
- `transpose`: arbitrary rectangular matrix or row-vec.

All handlers preserve **symbolic args** — `det([[a,b],[c,d]])` returns the symbolic tree `a*d - b*c`, not a numeric fold.

**Shape mismatch → `undefined`** (deliberate divergence from CAS prior art). This is the fwiz domain-boundary idiom that already covers `x/x = undefined iff x = 0`. Symbolic-first reasoning lets shape resolution be deferred to substitution time. Users can check `is_undefined(result)` at the solve boundary.

**`evaluate()` rejects matrices**: `evaluate(parse("[1,2,3]"))` returns empty `Checked<double>`. Vector/matrix has no real-valued projection; the existing `args.size() != 1 || !lookup_function(name)` short-circuit in `evaluate()` already covers this — no new failure modes added.

**`--derive` works today** (verified 2026-05-13, matrix-arc cycle 3 regression pins): concrete and symbolic vec/mat literals, `matmul`/`det`/`inv`/`transpose` outputs, and `matmul(A, inv(A))` cancellation all round-trip cleanly through `solved_symbolic_` → `format_derived`. No `#10a` structural extension required for the current derive scope. The known gap is `diff(M, t)` and `integral(M, t)` returning scalar-treatment instead of element-wise distribution — filed as Future.md #71 for the queued Linear-algebra completeness arc.

**Out of scope** (Future.md / reopen triggers in `master-plan.md`): Gaussian elimination for `inv` of N≥4 (open `.fw`-rule alternative first); eigenvalues / LU / SVD; complex-element matrices (orthogonal vector — Future.md #13a); promotion to `ExprType::MATRIX` leaf (reopen when `vec`/`mat` dispatch shows >5% of `simplify` time on matrix-heavy reproducers).

### trace.h

Three levels:
- `NONE` — no output (default)
- `STEPS` — algebraic reasoning: which equations are tried, inversions, substitutions, results
- `CALC` — steps plus numeric detail: each variable substitution and the expression before evaluation

All trace output goes to stderr. Controlled by `--steps` and `--calc` flags.

---

## Testing

2307+ tests organized into functional tests, edge cases, and robustness groups:

```bash
make test
```

### Fuzzing

`src/fuzz_parser.cpp` contains a libFuzzer harness that feeds arbitrary byte sequences through the lexer, parser, and simplifier via `FormulaSystem::load_string()`. The harness swallows `std::runtime_error` (expected on malformed input) but lets any other exception or memory error propagate — a sanitizer hit or crash-file drop is a real bug.

**When to run:** pre-release, and after any change to `lexer.h`, `parser.h`, or `expr.h::simplify`. Not required per-cycle (Clang-only; not part of the standard gate).

**Standard invocation:**
```bash
make fuzz
mkdir -p /tmp/fuzz_run
./bin/fwiz_fuzz /tmp/fuzz_run fuzz_corpus/ -max_total_time=60
```

The first positional argument is a writable scratch directory for the runtime queue; the second is the committed seed corpus. Passing only `fuzz_corpus/` as the sole argument causes libFuzzer to write ~1000+ mutated files into the seed directory, polluting the committed corpus.

**Crash protocol:** any `crash-*` file dropped by libFuzzer is a real bug. Capture it, file a Known-Issues entry, and fix before the next release.

**Scope:** lexer + parser + simplifier only. The resolve path is not fuzzed — it requires well-formed bindings and is a separate harness concern.

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

All 2307+ tests pass clean under every sanitizer — no leaks, no undefined behavior, no memory errors.

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
- Warning flags: `-Wall -Wextra -Wpedantic -Wshadow -Wuninitialized -Wnull-dereference -Wimplicit-fallthrough -Wdouble-promotion`. All flags are locked in `Makefile`; `make` must produce zero warnings. `-Wconversion` is deferred (untriaged).
- **Boolean discriminators → `enum class`**: a `bool` parameter or field that encodes a two-valued semantic distinction (e.g., "is this assumption inherent or derived?") must be replaced with a named `enum class`. Canonical example: `AssumptionSource : uint8_t { Derived, Inherent }` replacing `bool inherent` on `SimplifyAssumption` (`expr.h`).

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

### Const correctness

Local variables that are never mutated after construction must be declared `const`. This is enforced by the `misc-const-correctness` clang-tidy check (baseline zero as of Cycle 7.5, 2026-05-07; 245 findings were fixed in that cycle). New code that introduces a non-const local the checker would flag will surface at the next `make analyze-full` run.

**Const placement: prefer west-const** (`const T name`, `const auto& x`) for consistency with codebase convention. The codebase is 100% west-const; avoid east-const (`T const name`). When `clang-tidy --fix` is applied (it defaults to east-const), normalize the output to west-const before committing.

### Pointer const deduction

`const auto` on a pointer deduces `T* const` (pointer itself is const, pointee is mutable) — cppcheck's `constVariablePointer` check still fires. The correct idiom is `const auto* sol = fn(...)`, which deduces `const T*` (pointee const). Empirically verified: `const auto` does NOT silence `constVariablePointer`. Use `const auto*` at every local pointer declaration site where the pointee is not mutated.

### constexpr and inline

- **`constexpr`** — type predicates (`is_num`, `is_zero`, etc.), enum queries (`is_additive`), compile-time constants
- **`inline`** — everything else in headers (required for ODR in header-only code)
- **`static constexpr`** — all compile-time-constant statics. Runtime `static const` (lambdas, `std::map`, `std::sqrt` initializers — none constexpr-able in C++17) carry a one-line `// static const: <reason>` comment on the line immediately above the declaration. Lock: `grep -nE 'static const ' src/*.h | grep -v '// static const:'` must return 0 lines.
- Prefer function pointers over `std::function` to avoid heap allocation

### STL algorithms

Prefer `<algorithm>` over hand-rolled loops where the algorithm form is at least as clear: `std::any_of`, `std::all_of`, `std::none_of`, `std::find_if`, `std::count_if`, `std::transform`, `std::accumulate`, `std::copy_if`, `std::replace_if`. The cppcheck `useStlAlgorithm` check (un-suppressed in `make analyze-fast` since Cycle 4) enforces this automatically — any new raw loop that a standard algorithm can express will be caught at the per-cycle gate.

Keep manual loops when: the body has multiple side effects (assigns two captured outputs and breaks, emits a trace step, or early-returns from the enclosing function); when a readable algorithm form requires obscure lambda gymnastics (e.g. `std::move_iterator + back_inserter` for move-append into a different container); or when the loop is a numerical inner loop in `fit.h` where the arithmetic structure matters for readability and performance. When keeping a manual loop in a header file that cppcheck would otherwise flag, annotate inline with `// not std::algorithm: <reason>` immediately above the loop body's first statement, followed by `// cppcheck-suppress useStlAlgorithm` on the same position (cppcheck reports the warning at the body line, not the `for` line).

For separator-join formatting, prefer the `join_with_sep(range, sep, fn)` helper in `expr.h` over ad-hoc `bool first` flag patterns. `bool first` flags that guard a one-shot action (e.g., BOM strip on the first line) are NOT separator-join shapes and should stay as-is with a `// bool first: not separator-join shape — <reason>` annotation.

### `std::function` policy

Prefer template callable parameters and named structs over `std::function` (Turner ch 40). `std::function` carries type-erasure overhead (virtual dispatch, possible heap allocation for large captures) that is avoidable in most fwiz call sites.

- **Template `F&&` parameters** — for functions that accept a callable but do not store it beyond the call. Applied in Cycle 5 to `newton_solve`, `bisection_solve`, `adaptive_scan`, `find_numeric_roots` (expr.h) and `sample_function`, `compute_fit_stats` (fit.h).
- **Named structs with `operator()`** — for recursive lambdas that C++17 cannot express without `std::function`. The struct holds shared state by reference; recursive calls become `(*this)(child)`. Applied in Cycle 5 to `Walker`, `TreeCounter` (system.h, `cse_extract`) and `PatternMatcher` (expr.h, `match_pattern`).
- **`std::function` justified keeps** — three categories where type erasure is genuinely required: (a) boundary erasure into a typed `thread_local` or across an ABI boundary (e.g. `FuncInverter`, which returns `std::vector<ExprPtr>` since 2026-05-08 to support multiple inverse branches per builtin; sin/cos each have two), (b) optional callbacks stored as a nullable `const std::function*` parameter when the callable is conditionally provided (e.g. the analytic-derivative `fp_fn` in Newton's method), (c) heterogeneous lambda storage in `std::vector<T>` where each row holds a different concrete lambda type (e.g. `OuterBuiltin::fn`, `BuiltinInner::fn` in fit.h).

Every surviving `std::function` declaration carries a trailing `// std::function: <reason>` annotation on the same line. Any new site without one is a reviewer finding. Lock: `grep -nE 'std::function' src/expr.h src/system.h src/fit.h | grep -v '// std::function:'` must return 0 lines.

**`vector<ExprPtr>` iteration forms:** `ExprPtr` is `Expr*`; treat it as a raw pointer everywhere. For range-for: `for (const auto* x : c)`, not `for (const auto& x : c)` — the latter deduces `Expr* const&` and triggers cppcheck `constVariableReference`. For std::algorithm lambdas over `vector<ExprPtr>`: `[](ExprPtr x)` (by-value) is the house style; `[](const Expr* x)` is correct when the called function accepts `const Expr&` via dereference. Do NOT write `[](const ExprPtr& x)` — it compiles and is semantically equivalent but violates the "ExprPtr = raw pointer, not a reference type" convention and will be flagged at review.

### `[[nodiscard]]` discipline

Pure factories, predicates, value-returning queries, tree transformers, evaluators, and `Checked<T>` accessors must carry `[[nodiscard]]`. Discarding their return is almost always a logic error, and the annotation turns it into a compile-time `-Wunused-result` (under `-Wextra`). Placement: `[[nodiscard]]` goes BEFORE `inline` / `constexpr` / `static` / template headers and class-member return types. Side-effect-only callers (e.g. `build_alias_table()` invoked just to populate `aliases_`) discard explicitly with `(void)`. The `modernize-use-nodiscard` clang-tidy check (in `analyze-full`) flags new pure functions added without the annotation at the next batch run.

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
- **Sibling exceptions** — errors that must reach the user and must NOT be swallowed by solver-internal `catch (const std::runtime_error&)` sites derive directly from `std::exception` (NOT from `std::runtime_error`). Current sibling exceptions: `SolveBudgetExceededError` (system.h), `CrossFileResolutionCycleError` (system.h, since 2026-05-13), `RaggedMatrixError` (parser.h, since 2026-05-13), `BindingAnnotationError` (parser.h, since gen-3 cycle 2 2026-05-15 — raised when operators appear inside `(...)` dimension annotation). The top-level catch in `main.cpp` and the test harness `get_error` both catch `const std::exception&` and see them. Convention: "errors that must reach the user are sibling exceptions; errors that are per-call recoverable derive from `std::runtime_error`."
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
- **Per-cycle gate** before committing: `make test && make sanitize && make analyze-fast` (cppcheck — ~1-2 min). `make analyze-full` (clang-tidy — **~10 s** post-2026-05-07 hang fix; was hanging indefinitely on `bugprone-exception-escape` before that) is a user-triggered batch; the orchestrator tracks "cycles since last run" and surfaces it in `next-priorities.md`. `make analyze` runs both tiers. The `bugprone-exception-escape` and `bugprone-unchecked-optional-access` checks are excluded with rationale comments — see the Makefile and `.fwiz-workflow/debug-analyze-full-hang.md`.
- **Optional cross-compiler sanity** — `make test-clang` rebuilds and runs the test suite under `clang++` with the same warning flag set as the GCC build. Catches Clang-specific issues (stricter `[[nodiscard]]` handling, C++20 extension warnings, divergent codegen on undefined behavior). Soft-skips if `clang++` is not on PATH; not part of the per-cycle gate.
- **cppcheck inline suppressions** require the `--inline-suppr` flag, which is included in the Makefile's `analyze-fast` target. A `// cppcheck-suppress <id>` comment has no effect unless the tool is invoked with `--inline-suppr`.

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

### Commit message conventions

The commit title is what every GitHub viewer sees first — make it describe the **user-facing change**, not the internal cycle slug.

- **Lead with the WHAT.** Title should describe what the commit changes from a user/code perspective ("Add main.cpp dispatch arms for periodic ValueSet results", "Strategy 4 perf guard — 30s → 1.8s on triangle CSE roundtrip", "Replace vacuous trig precision test with quintic"). Outsiders without the audit-roadmap or `next-priorities.md` context can still tell what changed.
- **Cycle/issue references go at the END of the title in parentheses, OR in the commit body.** `"... (Periodicity #12g)"` or `"... (Cycle 8)"` is fine as a trailing tag; do NOT lead with `"Cycle 8 — ..."` or `"#12g — ..."`. The audit roadmap is internal-tracking metadata, not the headline.
- **Body is for the why and the cycle context.** Detailed cycle linkage, Future.md reopen triggers, and any `clang-tidy: green` / `clang-tidy: pending` annotations live in the body. Body convention: a 1–3 sentence summary, then sections (Strategy / What changed / Per-cycle gates / LOC delta / Closes / Co-Authored-By).
- **Co-Authored-By** trailer at the bottom for orchestrator-driven commits.
