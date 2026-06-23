#pragma once
#include "expr.h"
#include "fit.h"
#include "lexer.h"
#include "parser.h"
#include "trace.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <iostream>

// ============================================================================
//  Shared utility
// ============================================================================

[[nodiscard]] inline std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// ============================================================================
//  Formula system
// ============================================================================

struct Equation {
    std::string lhs_var;
    ExprPtr rhs;
    std::optional<Condition> condition;
    bool bidirectional = false;  // true for "iff", false for "if" or ":"
};

struct VerifyResult {
    std::string equation_desc;
    double computed;
    bool pass;
};

struct FormulaCall {
    std::string file_stem;    // e.g. "rectangle"
    std::string query_var;    // internal to sub-system, e.g. "area"
    std::string output_var;   // exposed to parent scope, e.g. "floor"
    // sub_system_var -> parent expression (evaluated at call time)
    std::map<std::string, ExprPtr> bindings;
};

// Shared dead-end set: keys are (variable_name, binding_name_keyset).
// Threaded through solve_recursive / solve_all / try_resolve by reference
// so sibling candidates within one top-level query avoid redundantly
// re-exploring the same unreachable variables. Scoped per top-level query
// (each resolve/resolve_all constructs its own); sub-systems from
// formula calls construct their own independently (no leakage).
using DeadEndSet = std::set<std::pair<std::string, std::set<std::string>>>;

// Thrown when the per-query solve budget is exhausted. Signals a critical
// bug (should never fire in practice given dead-end sharing); distinct from
// regular solve failures so CLI can return a dedicated exit code.
// Intentionally NOT derived from std::runtime_error so the many
// `catch (const std::runtime_error&)` sites in the solver don't swallow it —
// a budget breach must propagate to the top-level caller to signal the bug.
struct SolveBudgetExceededError : std::exception {
    [[nodiscard]] const char* what() const noexcept override { return "TIMEOUT: solve budget exceeded"; }
};

// Thrown when `load_sub_system` re-enters the same cache_key during a single
// load — i.e. a `.fw` file recursively loads itself by name (e.g. matmul.fw
// containing `matmul(A, B)`). Intentionally NOT derived from std::runtime_error
// so the many `catch (const std::runtime_error&)` sites in the solver don't
// swallow it — a cross-file cycle must propagate to the top-level caller so
// the user sees a clear error message instead of "Cannot solve for X".
struct CrossFileResolutionCycleError : std::exception {
    std::string msg;
    explicit CrossFileResolutionCycleError(std::string m) : msg(std::move(m)) {}
    [[nodiscard]] const char* what() const noexcept override { return msg.c_str(); }
};

// Thrown when a cross-file formula call cannot be resolved in --strict-includes
// mode (Future #80 M2). Intentionally NOT derived from std::runtime_error so the
// many `catch (const std::runtime_error&)` sites in the solver
// (extract_positional_calls, unfold_formula_call_body, ...) don't swallow it —
// the helpful "add @include" message must propagate to the top-level caller
// instead of being downgraded to a generic "Cannot solve for X". Sibling of
// CrossFileResolutionCycleError.
struct StrictIncludeError : std::exception {
    std::string msg;
    explicit StrictIncludeError(std::string m) : msg(std::move(m)) {}
    [[nodiscard]] const char* what() const noexcept override { return msg.c_str(); }
};

// Thrown when `formula_depth_` reaches `max_formula_depth`. Replaces the
// stringly-typed `msg.find("depth") != std::string::npos` substring match
// pre-cycle-3j at the two depth-aware catch sites in solve_recursive
// (try_formula at ~4455, try_resolve at ~4577) — those sites need to detect
// depth exhaustion to propagate it (rather than swallowing as a normal solve
// failure). Behaviorally equivalent to the pre-cycle-3j substring match;
// purely a structural-legibility cleanup. Derives from std::runtime_error
// so callers that catch runtime_error still see the signal at outer layers.
struct FormulaDepthExceededError : std::runtime_error {
    FormulaDepthExceededError()
        : std::runtime_error("Maximum formula call depth exceeded (possible infinite recursion)") {}
};

// ============================================================================
//  CSE (common subexpression elimination) for --derive output (Option C)
// ============================================================================
//
// `cse_extract(exprs, cap, occupied)` walks the input expressions and
// identifies high-value subtrees. At most `cap` helpers are returned, ranked
// by `value = (occurrences - 1) * (leaves - 1)` — the approximate character
// savings each helper would yield.
//
// Eligibility (a subtree is a candidate iff):
//   - it is NOT atomic (Var/Num) — naming a single Var as `t1 = x` is noise
//   - it contains at least one free Var (purely numeric subtrees collapse to
//     constants; naming `t1 = sin(3.14)` is useless)
//   - it occurs >= 2 times across all input expressions (single use carries
//     no value; counted by stringification — see `expr_to_string`).
//
// Selection: candidates are sorted by `value` descending; the top `cap` are
// kept. `cap == 0` returns the empty vector (CSE silently disabled).
//
// Topological order: the selected helpers are then re-sorted by node count
// ascending so smaller dependencies appear first. When each helper's RHS is
// later re-walked through `replace_subtree_by_name` against earlier helpers, it picks up
// nested references (e.g. `t2 = sin(t1)` when `t1 = x^2`).
//
// `occupied` is a name set the allocator must avoid (existing variables,
// section args/return_vars, builtins). Helper names are `t1, t2, ...`,
// skipping any name in `occupied`.
//
// Returns a vector in emission order (helpers first by topological depth).
[[nodiscard]] inline std::vector<std::pair<std::string, ExprPtr>> cse_extract(
        const std::vector<ExprPtr>& exprs,
        int cap,
        const std::set<std::string>& occupied) {
    std::vector<std::pair<std::string, ExprPtr>> out;
    if (cap <= 0) return out;

    // Step 1: count occurrences of every non-atomic subtree by string key.
    // Keep a representative ExprPtr per key for later emission.
    std::map<std::string, int> counts;
    std::map<std::string, ExprPtr> reps;

    struct Walker {
        std::map<std::string, int>& counts;
        std::map<std::string, ExprPtr>& reps;
        // const: mutates through reference members (counts, reps), not the struct itself
        void operator()(ExprPtr e) const {
            if (!e) return;
            switch (e->type) {
                case ExprType::NUM:
                case ExprType::VAR:
                    return;  // atomic — never extracted
                case ExprType::UNARY_NEG:
                    (*this)(e->child);
                    break;
                case ExprType::BINOP:
                    (*this)(e->left);
                    (*this)(e->right);
                    break;
                case ExprType::FUNC_CALL:
                    for (auto& a : e->args) (*this)(a);
                    break;
                case ExprType::COUNT_: assert(false && "invalid ExprType"); return;
            }
            // Eligibility: must contain at least one free Var.
            std::set<std::string> vars;
            collect_vars(*e, vars);
            if (vars.empty()) return;
            std::string key = expr_to_string(e);
            auto [it, inserted] = counts.emplace(key, 0);
            it->second++;
            if (inserted) reps.emplace(key, e);
        }
    };
    const Walker walker{counts, reps};
    for (auto& e : exprs) walker(e);

    // Pre-compute node counts (full tree size) and leaf counts (token count)
    // for every candidate subtree in a single recursive sweep with shared
    // memoization. Both metrics share the same recursion shape, so they're
    // computed and cached together — one map lookup per node, two metrics
    // returned. O(N×depth) once, vs O(N²×depth) in the original lambda-in-
    // comparator pattern.
    //
    // leaves (tokens): Var, Num, and FUNC_CALL function names. A function
    // name is itself a token in the printed form ("acos" + "(" + args + ")"),
    // so naming the call shaves the name plus the inside. UNARY_NEG sigil
    // ("-") contributes 0 leaves directly. This matches the value formula's
    // "approximate character savings" intent.
    struct TreeCounts { int nodes; int leaves; };
    std::map<ExprPtr, TreeCounts> count_cache;
    struct TreeCounter {
        std::map<ExprPtr, TreeCounts>& cache;
        TreeCounts operator()(ExprPtr e) {
            if (!e) return {0, 0};
            auto it = cache.find(e);
            if (it != cache.end()) return it->second;
            TreeCounts c{0, 0};
            switch (e->type) {
                case ExprType::NUM:
                case ExprType::VAR:       c = {1, 1}; break;
                case ExprType::UNARY_NEG: {
                    auto cc = (*this)(e->child);
                    c = {1 + cc.nodes, cc.leaves};
                    break;
                }
                case ExprType::BINOP: {
                    auto lc = (*this)(e->left);
                    auto rc = (*this)(e->right);
                    c = {1 + lc.nodes + rc.nodes, lc.leaves + rc.leaves};
                    break;
                }
                case ExprType::FUNC_CALL: {
                    c = {1, 1};  // own node + function name token
                    for (auto& a : e->args) {
                        auto ac = (*this)(a);
                        c.nodes += ac.nodes;
                        c.leaves += ac.leaves;
                    }
                    break;
                }
                case ExprType::COUNT_: assert(false && "invalid ExprType"); return {0, 0};
            }
            cache[e] = c;
            return c;
        }
    };
    TreeCounter tree_counts{count_cache};
    auto node_count = [&](ExprPtr e) { return tree_counts(e).nodes; };
    auto leaf_count = [&](ExprPtr e) { return tree_counts(e).leaves; };

    // Step 2: build candidates with (count >= 2). Compute value per candidate.
    struct Cand {
        std::string key;
        ExprPtr expr;
        int count;
        int leaves;
        int value;
    };
    std::vector<Cand> candidates;
    candidates.reserve(counts.size());
    for (auto& [key, count] : counts) {
        if (count < 2) continue;
        auto rep = reps[key];
        const int leaves = leaf_count(rep);
        const int value = (count - 1) * (leaves - 1);
        if (value <= 0) continue;  // 1-leaf subtree saves nothing
        candidates.push_back({key, rep, count, leaves, value});
    }

    // Step 3: rank by value descending; tiebreak by node count ascending then
    // stringified key (deterministic). Take top `cap`.
    std::sort(candidates.begin(), candidates.end(),
              [&](const Cand& a, const Cand& b) {
                  if (a.value != b.value) return a.value > b.value;
                  const int na = node_count(a.expr);
                  const int nb = node_count(b.expr);
                  if (na != nb) return na < nb;
                  return a.key < b.key;
              });
    if (static_cast<int>(candidates.size()) > cap) candidates.resize(static_cast<size_t>(cap));

    // Step 4: re-sort the selected candidates topologically (smallest first)
    // so dependent helpers see their deps already named (D8 invariant).
    std::sort(candidates.begin(), candidates.end(),
              [&](const Cand& a, const Cand& b) {
                  const int na = node_count(a.expr);
                  const int nb = node_count(b.expr);
                  if (na != nb) return na < nb;
                  return a.key < b.key;
              });

    // Step 5: assign helper names t1, t2, ... skipping any occupied name.
    int next_idx = 1;
    auto fresh_name = [&]() {
        for (;;) {
            std::string name = "t" + std::to_string(next_idx++);
            if (occupied.count(name) == 0) return name;
        }
    };
    out.reserve(candidates.size());
    // not std::transform: fresh_name() mutates the captured next_idx counter; algorithm form would hide the side-effecting lambda
    for (const auto& c : candidates) {
        // cppcheck-suppress useStlAlgorithm
        out.emplace_back(fresh_name(), c.expr);
    }
    return out;
}

// Periodicity Detection (Future #12):
// `trig_period(name)` — symbolic-table lookup mapping a periodic builtin
// function name to its period as ExprPtr. Returns nullptr for non-periodic
// names. ExprArena::Scope must be active (uses Expr::Num / Expr::BinOpExpr).
[[nodiscard]] inline ExprPtr trig_period(const std::string& fn_name) {
    if (fn_name == "sin" || fn_name == "cos")
        return Expr::BinOpExpr(BinOp::MUL, Expr::Num(2), Expr::Var("pi"));
    if (fn_name == "tan")
        return Expr::Var("pi");
    return nullptr;
}

// `detect_trig_origin(target, equations)` — scan the equations list for an
// equation of shape `result = FUNC_CALL(name in {sin,cos,tan}, args=[Var(target)])`.
// Returns the function name when matched (so trig_period can be looked up),
// or empty string otherwise. Compound-arg cases (sin(2*x+1)) deferred to
// Future #12a.
[[nodiscard]] inline std::string detect_trig_origin(
        const std::string& target,
        const std::vector<Equation>& eqs) {
    for (const auto& eq : eqs) {
        const ExprPtr rhs = eq.rhs;
        if (!rhs || rhs->type != ExprType::FUNC_CALL) continue;
        if (rhs->args.size() != 1) continue;
        const ExprPtr arg = rhs->args[0];
        if (!arg || arg->type != ExprType::VAR) continue;
        if (arg->name != target) continue;
        if (rhs->name == "sin" || rhs->name == "cos" || rhs->name == "tan")
            return rhs->name;
    }
    return "";
}

// BindingType + SetDef now live in expr.h (gen-5 cycle 3a, M3) — they are
// referenced by check_condition's is_in dispatch which sits in expr.h.
// Pure value types with no FormulaSystem dependency; the move is purely
// dependency-ordering. See expr.h for the canonical definitions.

class FormulaSystem {
public:
    mutable ExprArena arena;
    std::vector<Equation> equations;
    std::map<std::string, double> defaults;
    std::vector<FormulaCall> formula_calls;
    std::vector<Condition> global_conditions;
    std::vector<RewriteRule> rewrite_rules;

    struct RewriteRuleGroup {
        std::string pattern_key;             // expr_to_string(pattern)
        std::vector<size_t> rule_indices;    // into rewrite_rules
        bool exhaustive = false;             // all conditions cover (-inf, +inf)
    };
    std::vector<RewriteRuleGroup> rewrite_rule_groups_;
    std::vector<bool> rewrite_exhaustive_flags_;  // indexed by group_index

    // Custom function registry (per-system, for C++ API)
    std::map<std::string, double(*)(double)> custom_functions_;
    std::map<std::string, std::string> custom_function_defs_;  // name → .fw definition

    std::string base_dir;
    // @include search path (Future #80, M1 — COEXIST infra). Populated from
    // the CLI `-I <dir>` flag (order-preserving) then `FWIZ_PATH` env-var dirs.
    // Searched AFTER the file-relative `base_dir` by both `@include` resolution
    // (process_includes / resolve_file_path) and — as a COEXIST fallback —
    // cross-file formula-call resolution (load_sub_system). Propagated to
    // sub-systems via copy_metadata_to_sub.
    std::vector<std::string> include_dirs;
    // Canonical abs_paths registered by @include directives (Future #80, M1).
    // Records every file pulled in by process_includes so strict-mode (M2/M3)
    // can use it as the cross-file-call allow-list. In M1 (COEXIST) it is
    // populated but not yet gating. Propagated via copy_metadata_to_sub.
    std::set<std::string> included_files_;
    // Future #80 M2: explicit-include enforcement flag (default false). When
    // true, cross-file formula-call resolution in load_sub_system SKIPS the
    // base_dir filesystem auto-probe entirely — a call resolves ONLY via the
    // in-system / custom @def: cache, the @include allow-list (included_files_),
    // or the -I/FWIZ_PATH search path. An unresolved strict-mode call throws
    // StrictIncludeError naming the call and suggesting @include. Set by the
    // CLI `--strict-includes` flag; propagated via copy_metadata_to_sub so a
    // strict parent's sub-systems inherit strict resolution. Default is true
    // since M3 (Future #80 final); --legacy-implicit opts back into the old
    // implicit base_dir filesystem auto-probe for one backward-compat release.
    bool strict_includes_ = true;
    // Human-readable source label — file stem for load_file, the passed
    // `label` argument for load_string, or empty for a fresh-constructed
    // system. Used by build_alias_table() as the stem qualifier on
    // cross-file constant-name collisions.
    std::string source_label_;
    // gen-5 cycle 3g (2026-05-16): function-section self-reference for
    // recursive bodies. Set by register_function_section AFTER
    // sub->load_lines() returns (lazy timing avoids cycle-3d's load-time
    // stack-overflow trap — load_lines doesn't call load_sub_system, so
    // setting the name post-load defers resolution to solve time).
    // load_sub_system short-circuits to *this when file_part == self_name_,
    // so the body's recursive `fibonacci(n-1)` reaches the same sub without
    // inserting a cyclic shared_ptr. Empty for non-FUNCTION_SECTION subs
    // (default-constructed std::string).
    std::string self_name_;
    mutable Trace trace;
    mutable int max_formula_depth = 1000;
    mutable bool numeric_mode = false;
    mutable bool approximate_mode = false;  // --approximate: collapse symbolic to floats in derive output
    int numeric_samples = NUMERIC_DEFAULT_SAMPLES;
    int fit_depth = FIT_DEFAULT_DEPTH;
    int next_call_id_ = 0;  // counter for positional-call output variable names
    static inline thread_local int formula_depth_ = 0;

    // --- Budget sentinel (Part C) ---
    // Thread-local counter decremented per try_resolve / try_resolve_numeric.
    // Initialized at the OUTERMOST top-level query (resolve/resolve_all/
    // verify_variable); nested internal resolves (e.g. resolve_memoized
    // during numeric probing) share the same envelope. On breach, throws
    // SolveBudgetExceededError (not a runtime_error — bypasses the many
    // silent-catch sites in the solver). Insurance net: should never fire
    // in practice given Part A's dead-end sharing.
    //
    // Value chosen at 100k (not the triangle-hang design's original 1k intent)
    // because the rectangle puzzle test (area=12, perimeter=14 solve for w)
    // legitimately consumes ~12k charges — 200 scan samples × 2 probe_vars ×
    // recursive resolves each. 1k crashed that test. 100k gives a ~60s
    // wall-clock ceiling on truly pathological inputs (e.g. genuinely
    // under-constrained triangle queries) while never firing on legitimate
    // hard problems. A principled reduction would require shrinking
    // NUMERIC_DEFAULT_SAMPLES or refining the system-probe fallback — logged
    // as a follow-up in docs/Future.md if it becomes a user complaint.
    static constexpr int MAX_SOLVE_BUDGET = 100000;
    static inline thread_local int solve_budget_remaining_ = 0;
    static inline thread_local int solve_budget_depth_ = 0; // nesting depth

    // RAII: outermost guard initializes the budget; nested guards no-op.
    struct BudgetGuard {
        bool outermost;
        BudgetGuard() : outermost(solve_budget_depth_ == 0) {
            if (outermost) solve_budget_remaining_ = MAX_SOLVE_BUDGET;
            solve_budget_depth_++;
        }
        ~BudgetGuard() { solve_budget_depth_--; }
    };

    static void enforce_solve_budget() {
        if (solve_budget_depth_ == 0) return; // uninitialized (direct test calls)
        if (--solve_budget_remaining_ < 0) throw SolveBudgetExceededError();
    }

    // Collect the names (keys) of a bindings map as a set. Used to key
    // dead-end entries by available-binding context rather than specific values.
    template <typename Value>
    [[nodiscard]] static std::set<std::string> bindings_keyset(
            const std::map<std::string, Value>& bindings) {
        std::set<std::string> keys;
        for (auto& [k, _] : bindings) keys.insert(k);
        return keys;
    }

    mutable std::map<std::string, std::shared_ptr<FormulaSystem>> sub_systems;
    mutable std::unordered_map<std::string, double> numeric_memo_;
    mutable std::map<std::string, bool> numeric_results_; // var → true if exact (verified)

    // Provenance: parallel symbolic carrier. Populated at the binding-commit
    // point (try_resolve T10, try_formula T7) with the post-recognizer
    // simplified ExprPtr. Trace sites read from here so trace and final
    // output share the same data — they cannot disagree by construction.
    // Cleared at top of resolve() / resolve_all() (per-query lifetime,
    // mirrors `bindings`).
    mutable std::map<std::string, ExprPtr> solved_symbolic_;
    // Cached alias table built during build_alias_table(). Universal
    // alias-resolution table — used by fmt_trace's fallback path
    // (defaults / givens / @extern) where no symbolic source exists.
    mutable std::map<std::string, double> aliases_;
    // Cycle 3a of gen-5 Types-as-Named-Sets arc (2026-05-15).
    // Per-binding type record — replaces cycle-2's `dim_map_` (still the
    // 3rd parallel-map, richer value type). `BindingType.dim` holds the
    // atomic dim section name; `BindingType.sets` holds set-membership
    // atoms. Populated by (a) `[mass]`-style dim section scan and (b)
    // `var:type = expr` annotation parse (atomic OR intersection form).
    // Propagated to sub-systems on load_sub_system. Read by `check_condition`'s
    // `is_in` predicate path (see expr.h). The `.dim` field is a DimMap
    // exponent algebra (gen-5 cycle 3c, Future #7b FULL) — see expr.h.
    std::map<std::string, BindingType> type_map_;
    // Cycle 3a (gen-5, 2026-05-15): named-set registry. Built-ins
    // (int/real/rational/imaginary) registered in load_builtins(); dim
    // sections register their own SetDef in register_dim_section().
    // Cycle 3b (2026-05-16): also holds USER_PREDICATE entries registered by
    // register_predicate_section() from `[name(param)] iff ...` sections.
    // Read by check_condition's `is_in` predicate via SimplifyContext
    // thread-local (M3). Non-mutable — load-time-fixed, solver reads only.
    // See SetDef above for Kind dispatch contract.
    std::map<std::string, SetDef> set_definitions_;
    // Dirty-flag for resolve_diff_in_equations: tracks how far we've already
    // walked. Equations only ever grow, so on a second load_string the pass
    // skips already-rewritten equations. Eliminates redundant double-walk on
    // the CLI diff path (perf-auditor WARN, polish-pass Item 5).
    size_t diff_resolved_up_to_ = 0;
    // Symmetric dirty-flag for resolve_integral_in_equations (Future #16).
    size_t integral_resolved_up_to_ = 0;
    // Symmetric dirty-flag for resolve_aggregate_in_equations (gen-6 Step C):
    // unrolls formula-bodied aggregations (sum(score(roll), …)) into N concrete
    // FormulaCall terms after positional/diff/integral passes.
    size_t agg_resolved_up_to_ = 0;

    [[nodiscard]] std::set<std::string> all_variables() const {
        std::set<std::string> vars;
        for (auto& eq : equations) {
            vars.insert(eq.lhs_var);
            collect_vars(eq.rhs, vars);
        }
        for (auto& [k, v] : defaults) vars.insert(k);
        for (auto& fc : formula_calls) {
            vars.insert(fc.output_var);
            for (auto& [sub_var, expr] : fc.bindings)
                collect_vars(expr, vars);
        }
        return vars;
    }

    // ────────────── Subsection: Loading and parsing ──────────────

    // Stored sections from multi-system files: [name(args) -> return]
    struct Section {
        std::string name;
        std::vector<std::string> positional_args;  // e.g., {"x", "y"} for [func(x, y)]
        std::string return_var;                     // e.g., "result" for [func(x) -> result]
        std::string extern_func;                    // e.g., "sin" from @extern sin
        std::vector<std::string> lines;
    };
    std::vector<Section> sections_;

    // Pre-parse: split raw lines into sections by [name] headers
    // Returns the section list. Lines before the first [name] go into section ""
    // Parse section header: [name], [name(x, y)], or [name(x, y) -> result]
    [[nodiscard]] static Section parse_section_header(const std::string& header) {
        Section sec;
        // Strip [ and ]
        std::string inner = trim(header.substr(1, header.size() - 2));

        // Check for -> return_var
        auto arrow = inner.find("->");
        if (arrow != std::string::npos) {
            sec.return_var = trim(inner.substr(arrow + 2));
            inner = trim(inner.substr(0, arrow));
        }

        // Check for (args)
        auto lparen = inner.find('(');
        if (lparen != std::string::npos) {
            auto rparen = inner.find(')', lparen);
            if (rparen != std::string::npos) {
                const std::string args_str = inner.substr(lparen + 1, rparen - lparen - 1);
                // Split by comma
                std::istringstream ss(args_str);
                std::string arg;
                while (std::getline(ss, arg, ',')) {
                    arg = trim(arg);
                    if (!arg.empty()) sec.positional_args.push_back(arg);
                }
                inner = trim(inner.substr(0, lparen));
            }
        }

        sec.name = inner;
        return sec;
    }

    [[nodiscard]] static std::vector<Section> split_sections(const std::vector<std::string>& all_lines) {
        std::vector<Section> result;
        result.push_back({"", {}, {}, {}, {}}); // top-level (unnamed)
        for (const auto& line : all_lines) {
            auto trimmed = trim(line);
            // Section header: [name(args) -> return] optional_first_line
            if (trimmed.size() >= 3 && trimmed.front() == '[') {
                auto rbracket = trimmed.find(']');
                if (rbracket != std::string::npos) {
                    auto header = trimmed.substr(0, rbracket + 1);
                    auto rest = trim(trimmed.substr(rbracket + 1));
                    // Only treat as section if the [...] part has no '=' (not an equation)
                    if (header.find('=') == std::string::npos) {
                        auto sec = parse_section_header(header);
                        if (!sec.name.empty()) {
                            sec.lines = {};
                            if (!rest.empty()) {
                                // Sugar: [f(x) -> result] = x^2 → result = x^2
                                if (rest[0] == '=' && !sec.return_var.empty())
                                    rest = sec.return_var + " " + rest;
                                sec.lines.push_back(rest);
                            }
                            result.push_back(std::move(sec));
                            continue;
                        }
                    }
                }
            }
            // Annotation: @name value
            if (!trimmed.empty() && trimmed[0] == '@') {
                auto space = trimmed.find(' ');
                const std::string tag = trimmed.substr(1, space == std::string::npos ? std::string::npos : space - 1);
                const std::string val = (space != std::string::npos) ? trim(trimmed.substr(space + 1)) : "";
                if (tag == "extern") result.back().extern_func = val;
                // Future annotations handled here
                continue; // don't add to lines
            }
            result.back().lines.push_back(line);
        }
        return result;
    }

    // Load parsed lines into this system (shared by load_file and load_string)
    void load_lines(const std::vector<std::string>& lines) {
        int line_num = 0;
        for (const auto& raw : lines) {
            line_num++;
            std::string line = trim(raw);
            if (line.empty() || line[0] == '#') continue;
            // Strip inline comments (# not inside parentheses)
            { int pd = 0;
              // justified: token-cursor (offset arithmetic on char stream)
              for (size_t ci = 0; ci < line.size(); ci++) {
                  if (line[ci] == '(') pd++;
                  else if (line[ci] == ')') pd--;
                  else if (line[ci] == '#' && pd == 0) { line = trim(line.substr(0, ci)); break; }
              }
              if (line.empty()) continue;
            }
            try { parse_line(line); }
            catch (const std::runtime_error& e) {
                // Per-line resilience: normal parse errors become trace warnings;
                // the file continues loading subsequent lines. Sibling exceptions
                // that are NOT std::runtime_error (SolveBudgetExceededError,
                // CrossFileResolutionCycleError, RaggedMatrixError,
                // BindingAnnotationError) deliberately propagate — they signal
                // user-facing fatal conditions whose diagnostic is the entire
                // point. See parser.h RaggedMatrixError / BindingAnnotationError
                // for the convention.
                //
                // gen-5 cycle 3i (Fix W): include the line content alongside
                // the line number so `--steps` traces let users/LLMs see what
                // was discarded (Future #95 surface-gap PARKED — the named-arg
                // call to an unknown formula section is one common cause).
                trace.step("  warning: skipping line " + std::to_string(line_num)
                           + " '" + line + "': " + e.what());
            }
        }
    }

    // Load a specific section with cascading inheritance.
    // "triangle.right" loads: top-level → [triangle] → [triangle.right]
    void load_section(const std::string& section) {
        // Always load top-level (unnamed section)
        auto top_it = std::find_if(sections_.begin(), sections_.end(),
            [](const Section& s) { return s.name.empty(); });
        if (top_it != sections_.end()) load_lines(top_it->lines);

        if (section.empty()) {
            // No specific section requested but file has sections
            // Load nothing extra (top-level only)
            return;
        }

        // Build inheritance chain: "a.b.c" → ["a", "a.b", "a.b.c"]
        std::vector<std::string> chain;
        size_t pos = 0;
        while (pos <= section.size()) {
            size_t dot = section.find('.', pos);
            if (dot == std::string::npos) dot = section.size();
            chain.push_back(section.substr(0, dot));
            pos = dot + 1;
        }

        // Load each ancestor section in order
        for (const auto& ancestor : chain) {
            bool found = false;
            // not std::find_if: body applies return_var sugar transform + load_lines, sets found, breaks
            for (const auto& s : sections_) {
                // cppcheck-suppress useStlAlgorithm
                if (s.name == ancestor) {
                    // Apply return_var sugar: lines starting with "=" get return_var prepended
                    if (!s.return_var.empty()) {
                        auto sugared = s.lines;
                        for (auto& ln : sugared) {
                            auto t = trim(ln);
                            if (!t.empty() && t[0] == '=')
                                ln = s.return_var + " " + t;
                        }
                        load_lines(sugared);
                    } else {
                        load_lines(s.lines);
                    }
                    found = true;
                    break;
                }
            }
            if (!found && ancestor == section) {
                throw std::runtime_error("Section not found: [" + section + "]");
            }
        }
    }

    // Check if a variable name is a builtin constant not overridden by this system
    [[nodiscard]] bool is_active_builtin(const std::string& name) const {
        auto& consts = builtin_constants();
        const auto it = consts.find(name);
        if (it == consts.end()) return false;
        // Skip NaN-valued builtin constants (e.g. `i`): they have no real numeric
        // value and must not be auto-bound by the resolver. The pattern matcher
        // still treats them as literal-match constants via builtin_constants().count(name)
        // — that path does not consult is_active_builtin.
        //
        // Note: this guard closes the auto-binding side-channel for NaN-valued
        // builtin constants. The deeper `flatten_additive` NaN-propagation bug
        // (Future.md #13c) remains; it is unreachable from user-facing input
        // *while `i` is the only NaN-bound constant*. A second NaN-valued
        // builtin (e.g. imaginary infinity) would require fixing #13c first.
        if (std::isnan(it->second)) return false;
        if (defaults.count(name)) return false;
        return std::none_of(equations.begin(), equations.end(),
            [&name](const Equation& eq) { return eq.lhs_var == name; });
    }

    void trace_loaded() const {
        if (!trace.show_steps()) return;
        for (const auto& eq : equations)
            trace.step("  equation: " + eq.lhs_var + " = " + expr_to_string(eq.rhs));
        for (auto& [k, v] : defaults)
            trace.step("  default: " + k + " = " + fmt_num(v));
        for (const auto& fc : formula_calls)
            trace.step("  formula call: " + fc.file_stem + "(" + fc.query_var + "=?" + fc.output_var + ")");
    }

    // Read all lines from a stream, stripping BOM and splitting on semicolons
    [[nodiscard]] static std::vector<std::string> read_all_lines(std::istream& in) {
        std::vector<std::string> lines;
        std::string line;
        // bool first: not separator-join shape — guards a one-shot BOM strip on the first line
        bool first = true;
        while (std::getline(in, line)) {
            if (first) { first = false; strip_bom(line); }
            // Split on semicolons (as line separator)
            size_t pos = 0;
            while (pos < line.size()) {
                const size_t semi = line.find(';', pos);
                if (semi == std::string::npos) {
                    lines.push_back(line.substr(pos));
                    break;
                }
                lines.push_back(line.substr(pos, semi - pos));
                pos = semi + 1;
            }
            if (pos == 0 && line.empty()) lines.push_back("");
        }
        return lines;
    }

    // ────────────── Subsection: Builtins and rewrite rules ──────────────

    // Load lines with section selection (shared by load_file and load_string)
    // Built-in rewrite rules — loaded automatically, replace hardcoded C++ simplifier rules.
    // These are the .fw equivalents; the file stdlib/builtin.fw mirrors this for documentation.
    static constexpr const char* BUILTIN_REWRITE_RULES = R"(
sin(-x) = -sin(x)
cos(-x) = cos(x)
asin(sin(x)) = x
acos(cos(x)) = x
atan(tan(x)) = x
sin(asin(x)) = x
cos(acos(x)) = x
tan(atan(x)) = x
abs(abs(x)) = abs(x)
abs(-x) = abs(x)
sqrt(x^2) = abs(x)
sqrt(x)^2 = x iff x >= 0
log(e^x) = x
e^log(x) = x
log(x^n) = n * log(x) iff x != 0
x/x = 1 iff x != 0
x/x = undefined iff x = 0
k * x / (k * y) = x / y iff k != 0
x / (1 / y) = x * y iff y != 0
x^0 = 1
x^1 = x
x^(1/2) = sqrt(x)
# Complex identity i^2 = -1 — both forms because the simplifier
# canonicalizes MUL(i,i) → POW(i,2) during multiplicative flattening,
# so the post-flatten POW form is what fires on `i*i`. The pre-flatten
# MUL form is defensive (catches paths that bypass flatten).
i * i = -1
i ^ 2 = -1
(x^a)^b = x^(a*b)
x^a / x^b = x^(a - b) iff x != 0
abs(x) / x = sign(x) iff x != 0
abs(x) / x = undefined iff x = 0
# Negative-exponent canonicalization (T3.6, Future #53): x^(-N) → 1/x^N.
# The `is_neg_num(n)` predicate gates firing on numeric-negative exponents only,
# so symbolic exponents (`x^y`) do not trigger infinite rewrite loops.
x^n = 1 / x^(-n) iff is_neg_num(n)
)";

    // Built-in function definitions — loaded as sub-systems when called.
    // Each maps a function name to its .fw section content.
    static const std::map<std::string, std::string>& builtin_function_defs() {
        // static const: std::map runtime-init, not constexpr-able in C++17
        // sin/cos: two inverse equations cover the second principal-cycle
        // branch (sin: pi - asin(r); cos: 2*pi - acos(r)). Tan stays single
        // (period = branch shift, single family). Spaces around operators
        // are required by the .fw lexer's tokenization (whitespace-tolerant
        // but reviewer-flagged as a safety detail).
        static const std::map<std::string, std::string> defs = {
            {"sin",  "[sin(x) -> result] @extern sin; x = asin(result); x = pi - asin(result)"},
            {"cos",  "[cos(x) -> result] @extern cos; x = acos(result); x = 2 * pi - acos(result)"},
            {"tan",  "[tan(x) -> result] @extern tan; x = atan(result)"},
            {"asin", "[asin(x) -> result] @extern asin; x = sin(result)"},
            {"acos", "[acos(x) -> result] @extern acos; x = cos(result)"},
            {"atan", "[atan(x) -> result] @extern atan; x = tan(result)"},
            {"sqrt", "[sqrt(x) -> result] @extern sqrt; x = result^2; result >= 0"},
            {"log",  "[log(x) -> result] @extern log; x = e^result"},
            {"abs",  "[abs(x) -> result] @extern abs; = x iff x >= 0; = -x iff x < 0"},
        };
        return defs;
    }

    void load_builtins() {
        std::istringstream ss(BUILTIN_REWRITE_RULES);
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            parse_line(line);
        }
        compute_rewrite_groups();

        // Built-in named sets — BUILTIN_PREDICATE Kind dispatches via fn ptr.
        // Design invariant (gen-5 cycle 3a): each built-in could be re-expressed
        // as a user [name(n)] iff ... declaration; the C++ fast path is an
        // optimization. Cycle 3b (USER_PREDICATE) makes that equivalence
        // demonstrable.
        // `imaginary` membership is the NaN-sentinel test — `i` binding
        // (cycle 2 invariant) carries NaN to mark the imaginary unit; future
        // full-complex-number values would need a richer Kind. Today: any NaN
        // value is in `imaginary`. (Renamed from `complex` in cycle 3b per
        // D8 R2: `complex` is the proper superset
        // `is_in(n, real) || is_in(n, imaginary)`, parked as a future
        // user-defined set once complex arithmetic ships.)
        // `rational`: cycle-3a fast path is integer-only; cycle 3b refines via
        // symbolic is_int_frac on the bound ExprPtr.
        auto reg_builtin = [this](std::string nm, bool (*pred)(double)) {
            // Trailing parameter/predicate fields explicitly default — cycle
            // 3b USER_PREDICATE-only; built-ins leave them empty/nullopt.
            set_definitions_[nm] = SetDef{nm, SetDef::Kind::BUILTIN_PREDICATE, pred, {}, std::nullopt, {}};
        };
        reg_builtin("int",      [](double v) { return is_integer_value(v); });
        reg_builtin("real",     [](double v) { return std::isfinite(v); });
        reg_builtin("rational", [](double v) { return is_integer_value(v); });
        reg_builtin("imaginary", [](double v) { return std::isnan(v); });
    }

    // Group rewrite rules by LHS pattern and check exhaustiveness.
    // Rules with the same pattern string are grouped (e.g., "x / x").
    // A group is exhaustive if its conditions' union covers all reals for every
    // constrained variable.
    void compute_rewrite_groups() {
        rewrite_rule_groups_.clear();
        std::map<std::string, size_t> key_to_group;

        // not std::algorithm: dual-output loop (rule.group_index assigned, group container appended-or-extended); cppcheck did not flag this site
        for (size_t i = 0; i < rewrite_rules.size(); i++) {
            auto& rule = rewrite_rules[i];
            auto key = expr_to_string(rule.pattern);
            auto it = key_to_group.find(key);
            if (it == key_to_group.end()) {
                key_to_group[key] = rewrite_rule_groups_.size();
                rule.group_index = static_cast<int>(rewrite_rule_groups_.size());
                rewrite_rule_groups_.push_back({key, {i}, false});
            } else {
                rule.group_index = static_cast<int>(it->second);
                rewrite_rule_groups_[it->second].rule_indices.push_back(i);
            }
        }

        // Check exhaustiveness for groups with multiple rules
        for (auto& group : rewrite_rule_groups_) {
            if (group.rule_indices.size() < 2) continue;

            // Collect all condition variables and their ValueSets
            std::map<std::string, ValueSet> var_coverage;

            for (const size_t idx : group.rule_indices) {
                const auto& rule = rewrite_rules[idx];
                if (!rule.condition.has_value()) {
                    // Unconditional rule → covers everything
                    group.exhaustive = true;
                    break;
                }
                const Condition& cond = *rule.condition;

                // Extract constrained variables from condition
                for (const auto& clause : cond.clauses) {
                    std::string var;
                    if (is_var(clause.lhs)) var = clause.lhs->name;
                    else if (is_var(clause.rhs)) var = clause.rhs->name;
                    if (var.empty()) continue;

                    auto vs = cond.to_valueset(var);
                    if (var_coverage.count(var))
                        var_coverage[var] = var_coverage[var].unite(vs);
                    else
                        var_coverage[var] = vs;
                }
            }

            if (group.exhaustive) continue;  // already set by unconditional rule
            if (var_coverage.empty()) continue;

            // Exhaustive if every constrained variable covers all reals
            group.exhaustive = true;
            for (auto& [var, vs] : var_coverage) {
                if (!vs.covers_reals()) {
                    group.exhaustive = false;
                    break;
                }
            }
        }

        // Build flat flags vector for thread-local access
        rewrite_exhaustive_flags_.resize(rewrite_rule_groups_.size());
        std::transform(rewrite_rule_groups_.begin(), rewrite_rule_groups_.end(),
            rewrite_exhaustive_flags_.begin(),
            [](const RewriteRuleGroup& g) { return g.exhaustive; });
    }

    // ────────────── Subsection: Loading and parsing (continued — formula calls, sub-systems) ──────────────

    // Walk an expression, find FUNC_CALL nodes that aren't builtins, and convert them
    // to formula calls using positional arg metadata from the sub-system's section header.
    // Returns the expression with formula calls replaced by their output variables.
    [[nodiscard]] ExprPtr extract_positional_calls(const ExprPtr& e,
                                     std::vector<FormulaCall>& calls) {
        if (!e) return e;
        if (e->type == ExprType::FUNC_CALL
            && !is_aggregate_reducer(e->name)       // Step C: don't probe load_sub_system("sum")
            && !builtin_functions().count(e->name)
            && !custom_functions_.count(e->name)) {
            // Not a builtin — try loading as sub-system formula
            const std::string file_stem = e->name;
            try {
                auto& sub = load_sub_system(file_stem);
                // Find section metadata with positional args
                std::vector<std::string> pos_args;
                std::string return_var;
                for (const auto& sec : sub.sections_) {
                    if (sec.name == "" || sec.name == file_stem
                        || file_stem.find('.') != std::string::npos) {
                        if (!sec.positional_args.empty()) {
                            pos_args = sec.positional_args;
                            return_var = sec.return_var;
                            break;
                        }
                    }
                }
                // Also check first section with matching args count
                if (pos_args.empty()) {
                    // not std::find_if: body assigns 2 captured outputs (pos_args, return_var) and breaks
                    for (const auto& sec : sub.sections_) {
                        // cppcheck-suppress useStlAlgorithm
                        if (!sec.positional_args.empty()) {
                            pos_args = sec.positional_args;
                            return_var = sec.return_var;
                            break;
                        }
                    }
                }
                if (pos_args.empty()) return e;  // no positional metadata

                // Build FormulaCall with positional bindings
                FormulaCall call;
                call.file_stem = file_stem;
                if (return_var.empty()) return_var = "result";
                call.query_var = return_var;

                // Generate unique output variable name (per-instance counter)
                call.output_var = "_fc" + std::to_string(next_call_id_++);

                // Map positional args
                // justified: parallel iteration with min(e->args.size(), pos_args.size())
                for (size_t i = 0; i < e->args.size() && i < pos_args.size(); i++) {
                    // Recursively process nested calls in the argument
                    auto arg = extract_positional_calls(e->args[i], calls);
                    call.bindings[pos_args[i]] = arg;
                }

                calls.push_back(std::move(call));
                return Expr::Var(calls.back().output_var);
            } catch (const StrictIncludeError&) {
                // Inline post-load/simplifier builtin (diff/integral/vec/mat/...) — not a
                // cross-file call; leave it for the post-load pass / simplifier. A genuine
                // un-@include'd cross-file call rethrows so the "add @include" hint surfaces.
                if (is_postload_builtin(e->name)) return e;
                throw;
            } catch (const std::runtime_error&) {
                // Sub-system not found — leave as FUNC_CALL
                return e;
            }
        }

        // Recurse into sub-expressions
        if (e->type == ExprType::UNARY_NEG)
            return Expr::Neg(extract_positional_calls(e->child, calls));
        if (e->type == ExprType::BINOP)
            return Expr::BinOpExpr(e->op,
                extract_positional_calls(e->left, calls),
                extract_positional_calls(e->right, calls));
        if (e->type == ExprType::FUNC_CALL) {
            // Step C: aggregate reducer bodies must NOT have their nested formula
            // calls eagerly extracted here — iterator substitution (and thus
            // concrete per-term bindings) happens in resolve_aggregate_in_equations
            // (post-load). Recurse into NO args (they are only body/Var/range).
            // Premature extraction would bind score(roll) once and fold N copies
            // of the same output var → Bug-B `N*score(last)`.
            if (is_aggregate_reducer(e->name)) return e;
            std::vector<ExprPtr> args;
            args.reserve(e->args.size());
            // not std::transform: extract_positional_calls mutates the captured `calls` vector via reference
            for (auto& a : e->args)
                // cppcheck-suppress useStlAlgorithm
                args.push_back(extract_positional_calls(a, calls));
            return Expr::Call(e->name, args);
        }
        return e;
    }

    // Post-load: convert FUNC_CALL nodes that match sub-systems into formula calls
    void resolve_positional_calls() {
        for (auto& eq : equations) {
            std::vector<FormulaCall> new_calls;
            eq.rhs = extract_positional_calls(eq.rhs, new_calls);
            // not std::transform: move-append into a different container; std::move_iterator is less readable here
            for (auto& c : new_calls)
                // cppcheck-suppress useStlAlgorithm
                formula_calls.push_back(std::move(c));
        }
    }

    // ------------------------------------------------------------------------
    // Dim section registration (gen-3 cycle 2, 2026-05-14; updated gen-5
    // cycle 3a 2026-05-15 — Types as Named Sets).
    //
    // A bare `[name]` Section header (no parens, no arrow) declares `name` as
    // a *dimension*. Its body is parsed into a sub-FormulaSystem; each LHS
    // identifier in the body is registered in `type_map_` with `name` as its
    // `BindingType.dim` field (cycle 3a: BindingType replaces the bare string
    // value, carrying both `dim` and `sets` fields per the named-sets
    // unification). The section is ALSO registered in `set_definitions_` with
    // `SetDef::Kind::DIM_SECTION`, so `is_in(v, name)` predicates dispatch
    // correctly. The sub is routed through the existing `@def:` cache-key
    // path (D6, reviewer-resolved 2026-05-14) so `mass.kg=?` dot-access
    // lookups hit the cached sub instead of attempting a file load.
    // ------------------------------------------------------------------------
    [[nodiscard]] static bool is_dimension_section(const Section& s) {
        return s.return_var.empty() && s.positional_args.empty();
    }

    // ------------------------------------------------------------------------
    // Predicate section registration (gen-5 cycle 3b, 2026-05-16).
    //
    // A `[name(param)]` Section with exactly one positional arg and no return
    // variable is a *predicate section*. Its body declares membership
    // conditions either inline (`[name(n)] iff <expr>`) or multi-line
    // (one clause per body line; implicit AND across lines). The body is
    // parsed once into a Condition; `set_definitions_[name]` is populated as
    // `Kind::USER_PREDICATE` with the parameter name and stored predicate.
    //
    // Section disambiguation:
    //   `[name]`           dim section       (args empty, no return)
    //   `[name(arg)]`      predicate section (one arg, no return)  ← this
    //   `[name(...) -> v]` formula section   (return non-empty)
    // ------------------------------------------------------------------------
    [[nodiscard]] static bool is_predicate_section(const Section& s) {
        return s.positional_args.size() == 1
            && s.return_var.empty()
            && !s.lines.empty();
    }

    // ------------------------------------------------------------------------
    // Function section registration (gen-5 cycle 3d, 2026-05-16).
    //
    // A `[name(arg) -> ret]` Section with exactly one positional arg and a
    // non-empty return variable is a *function section* — testable as a set
    // via existential solving: `is_in(x, name)` ↔ `∃ n: name(n) = x`.
    //
    // Multi-arg formula sections (`args.size() > 1`) are explicitly excluded:
    // `is_in(3, add)` would be semantically ambiguous (which arg maps to 3?).
    // They remain callable as functions via `add(a=1, b=2)` syntax but NOT
    // set-testable. Future multi-arg relation syntax is Future #88.
    //
    // Complete section-shape disambiguation (cycle 3d):
    //   `[name]`           → dim section       (args empty, no return)  cycle 3a
    //   `[name(arg)]`      → predicate section (one arg, no return)     cycle 3b
    //   `[name(arg)->ret]` → function section  (one arg, return set)    cycle 3d
    //   `[name(a,b)->ret]` → callable formula  (multi-arg, NOT a set)
    // ------------------------------------------------------------------------
    [[nodiscard]] static bool is_function_section(const Section& s) {
        return s.positional_args.size() == 1
            && !s.return_var.empty();
    }

    void register_predicate_section(const Section& s) {
        assert(s.positional_args.size() == 1);
        const std::string& param = s.positional_args[0];

        // Unified inline-vs-multi-line body parsing:
        //   inline:     s.lines[0] starts with "iff " → strip prefix on line 0
        //   multi-line: no "iff " prefix on any line
        // Join all non-blank lines with " && " (implicit AND across lines).
        std::string joined;
        for (size_t i = 0; i < s.lines.size(); i++) {
            std::string ln = trim(s.lines[i]);
            if (ln.empty()) continue;
            if (i == 0 && ln.size() >= 4 && ln.substr(0, 4) == "iff ") {
                ln = trim(ln.substr(4));
            }
            if (ln.empty()) continue;
            if (!joined.empty()) joined += " && ";
            joined += ln;
        }
        if (joined.empty()) return; // empty body — silently inert

        std::optional<Condition> cond_opt;
        try {
            cond_opt = parse_condition(joined);
        // NOLINTNEXTLINE(bugprone-empty-catch) — malformed predicate body → silently skip (consistent with parse_line best-effort posture)
        } catch (const std::runtime_error&) { return; }
        if (!cond_opt) return; // parse returned nullopt

        SetDef sd;
        sd.name = s.name;
        sd.kind = SetDef::Kind::USER_PREDICATE;
        sd.parameter = param;
        sd.predicate = std::move(*cond_opt);
        set_definitions_[s.name] = std::move(sd);
    }

    void register_dim_section(const Section& s) {
        // D1 edge case: bare `[name] @extern foo` is classified as a dim
        // section but @extern is meaningless here (no return_var to bridge).
        // Practically unreachable today (all extern sections have -> result),
        // but emit a warning per the design commitment.
        if (!s.extern_func.empty()) {
            std::cerr << "warning: @extern on bare section '[" << s.name
                      << "]' ignored — bare sections are dimension declarations, "
                      << "not formula stubs\n";
        }
        auto sub = std::make_shared<FormulaSystem>();
        sub->load_lines(s.lines);
        // A dim-section body uses both forms: `g = 1` parses as a *default*
        // (RHS pure number → goes into `defaults`, not `equations`), while
        // `kg = 1000 * g` parses as an equation. Both are dim-typed.
        for (const auto& eq : sub->equations)
            type_map_[eq.lhs_var].dim = DimMap{{s.name, 1}};
        for (const auto& [name, _value] : sub->defaults)
            type_map_[name].dim = DimMap{{s.name, 1}};
        // Cycle 3a (gen-5): dim section also registers a SetDef so that
        // `is_in(v, mass)` can dispatch via DIM_SECTION Kind.
        // Trailing fields explicitly default for cppcheck-clean compile.
        set_definitions_[s.name] = SetDef{s.name, SetDef::Kind::DIM_SECTION, nullptr, {}, std::nullopt, {}};
        // Serialize the section body into `custom_function_defs_` so
        // load_sub_system's dispatcher routes dotted lookups (`mass.kg`)
        // through the @def: cache-key path (matches D6 / Issue-R2 resolution).
        std::ostringstream oss;
        oss << "[" << s.name << "]\n";
        for (const auto& line : s.lines) oss << line << "\n";
        custom_function_defs_[s.name] = oss.str();
        // Store the in-memory sub under the matching cache key so the
        // dispatcher short-circuits the would-be re-parse.
        sub_systems[std::string("@def:") + s.name] = sub;
    }

    // ------------------------------------------------------------------------
    // Function section registration (gen-5 cycle 3d, 2026-05-16).
    //
    // Mirrors `register_dim_section`'s dual-registration pattern: serialize
    // the section header+body into `custom_function_defs_` (so a later
    // `load_sub_system(name)` call — e.g. the M3 ExistenceChecker callback —
    // finds the inline definition instead of attempting a filesystem read)
    // AND cache the parsed sub under `@def:<name>` so the first lookup
    // short-circuits the re-parse step.
    //
    // The pre-parse uses `sub->load_lines(s.lines)` — NOT `load_string` —
    // so the sub does NOT recursively trigger `load_with_sections` (and thus
    // does NOT recursively call `register_function_section` on itself). That
    // matters for self-recursive formula sections like `[fib(n)->r] = ...
    // fib(n-1) + fib(n-2)`: if the sub re-registered, the sub's recursive
    // resolve_positional_calls walk would hit `load_sub_system("fib")` on
    // the sub, and the still-in-flight outer `currently_loading[@def:fib]`
    // guard would fire — falsely flagging legitimate recursion as a cross-
    // file cycle.
    //
    // Field-copy block kept minimal per critic D4 — no `trace`/`numeric_mode`/
    // etc. copied. If tests show field-copy gaps, add the missing fields then.
    // ------------------------------------------------------------------------
    void register_function_section(const Section& s) {
        assert(s.positional_args.size() == 1 && !s.return_var.empty());
        // Pre-parse the body into a sub via load_lines (NOT load_string —
        // load_string would re-run load_with_sections and re-register, which
        // breaks legitimate self-recursive sections — see doc comment above).
        // Apply the `= ...` return_var sugar transform that load_section
        // would normally apply: lines starting with `=` get the return_var
        // prepended.
        std::vector<std::string> body_lines;
        body_lines.reserve(s.lines.size());
        for (const auto& raw : s.lines) {
            std::string ln = trim(raw);
            if (!ln.empty() && ln[0] == '=' && (ln.size() == 1 || ln[1] != '='))
                body_lines.push_back(s.return_var + " " + ln);
            else
                body_lines.push_back(raw);
        }
        auto sub = std::make_shared<FormulaSystem>();
        // gen-5 cycle 3h (2026-05-16, closes #92 dependency): propagate parent
        // settings to the pre-cached sub. Without this, `numeric_mode=false` on
        // the sub suppressed Strategy 6 during recursive FUNCTION_SECTION
        // reverse-solve (the ExistenceChecker callback `is_in(N, fibonacci)`
        // would never fire the numeric scan that proves N is in the sequence).
        copy_metadata_to_sub(*sub);
        // Serialize the full header (with arg + return_var) and body into the
        // sub's custom_function_defs_ so a later load_sub_system call on the
        // sub finds the inline definition (defensive — self_name_ short-circuit
        // primarily fires first, but matches what load_with_sections would
        // have populated on a normally-loaded sub).
        std::ostringstream oss;
        oss << "[" << s.name << "(" << s.positional_args[0]
            << ") -> " << s.return_var << "]\n";
        for (const auto& line : s.lines) oss << line << "\n";
        sub->custom_function_defs_[s.name] = oss.str();
        // gen-5 cycle 3i: enable extract_formula_calls' named-arg branch to
        // find return_var DURING sub->load_lines below. self_name_ +
        // sections_.push_back must happen pre-load_lines so the in-body
        // call site `fibonacci(n=n-1)` resolves via the self-reference
        // short-circuit at parse time (load_sub_system returns *sub directly,
        // then return_var is read from sub.sections_[0]).
        //
        // Cycle-3g comment (preserved for context): lazy post-load_lines
        // timing was originally chosen to avoid cycle-3d's load-time stack
        // overflow. That overflow lived in the PARENT system's
        // resolve_positional_calls running inside load_with_sections, not in
        // the sub's load_lines — load_lines → parse_line → extract_formula_calls
        // → self->load_sub_system on the SUB hits the self_name_ short-circuit
        // and returns immediately (no recursion). The cycle-3d guard remains
        // load-bearing in load_with_sections; this pre-cache path is safe.
        sub->self_name_ = s.name;
        // Populate sub.sections_ with this section so extract_positional_calls
        // and extract_formula_calls' named-arg branch can find positional
        // metadata / return_var when reverse-resolving cross-section calls.
        sub->sections_.push_back(s);
        sub->load_lines(body_lines);
        // Parent's custom_function_defs_ entry — needs to be after load_lines
        // only by convention (the actual data is identical to sub's copy
        // above). Keeping the pair adjacent preserves the historical comment
        // attribution.
        custom_function_defs_[s.name] = oss.str();
        // gen-5 cycle 3i (Fix Z, closes Future #91 positional-body gap):
        // resolve_positional_calls converts direct-body positional recursive
        // forms like `result = fibonacci(n-1) + fibonacci(n-2)` into
        // FormulaCall entries at load time. The normal load path runs this
        // from load_with_sections (line ~1236); the pre-cache path silently
        // skipped it. Same shape of substrate gap as cycle 3h Fix A's
        // copy_metadata_to_sub — see Future #96 PARKED for the consolidation
        // trigger (third such pass = extract finalize_sub_after_load_lines).
        sub->resolve_positional_calls();
        // Pre-cache the parsed sub so load_sub_system's first lookup returns
        // it immediately.
        //
        // Cycle-3d caveat: this pre-cache disables the LOAD-time cross-file
        // cycle guard for single-arg FUNCTION_SECTION-eligible files (Future
        // #69 narrows). Self-recursive function sections like `[fib(n)->r] =
        // fib(n-1) + fib(n-2)` are accepted at load time; their reverse-solve
        // (n given r) is bounded by max_formula_depth at resolve time. Pure-
        // degenerate cases like `[myfn(x)->r] = myfn(x)` (no base case) also
        // load successfully but reverse-resolve unconditionally fails the
        // budget guard.
        //
        // Deliberately NOT propagating parent's custom_function_defs_ or
        // adding self-reference to sub.sub_systems[@def:s.name]. Two reasons:
        // (1) Forward / reverse solving of self-recursive bodies via the
        //     pre-cached sub blows the stack on a tight self-call before
        //     max_formula_depth fires (templates inflate per-frame to ~30KB;
        //     8MB default stack ≈ 270 frames). Without self-reference, the
        //     sub treats the inner self-call as an unresolved FUNC_CALL —
        //     no recursion, fail-safe NaN.
        // (2) For non-recursive function sections (perfect_square, double_it,
        //     cube, sqp1) the body has no FUNC_CALLs that need resolution,
        //     so this restriction is invisible. These cases work end-to-end
        //     (verified in M3 C4/C5/C8 tests).
        // Recursive function-section reverse-solve requires formula-call
        // memoization — Future #85. Documented as cycle-3d limitation.
        const std::string cache_key = "@def:" + s.name;
        sub_systems[cache_key] = sub;
        // Register the SetDef. `parameter` carries the formal arg name;
        // `function_section_name` carries the return-var name. Both are used
        // by the ExistenceChecker callback (M3) to build the reverse query:
        //   sub.resolve(sd.parameter, {{sd.function_section_name, value}})
        SetDef sd;
        sd.name = s.name;
        sd.kind = SetDef::Kind::FUNCTION_SECTION;
        sd.parameter = s.positional_args[0];
        sd.function_section_name = s.return_var;
        set_definitions_[s.name] = std::move(sd);
    }

    // ──────────────── @include support (Future #80, M1 — COEXIST) ────────────────
    //
    // Resolve a file reference against the search path. When `is_literal` is
    // true (used by `@include "path.fw"`) the reference is used verbatim — no
    // `.fw` is appended. When false (used by cross-file formula-call stem
    // lookup) `.fw` is appended if the reference has no extension dot.
    //
    // Search order: (1) absolute path used directly; (2) relative to base_dir
    // (the including file's directory); (3) each include_dirs entry in order
    // (-I dirs then FWIZ_PATH dirs). Returns the canonical abs_path of the
    // first existing match, or "" on miss. Every directory probed is appended
    // to `searched` (if non-null) so the caller can build a "not found" error
    // that names the searched locations.
    //
    // `exclude_base_dir` (Future #80 M2): when true, step (2) is skipped so the
    // base_dir co-location auto-probe does NOT participate. Used by strict-mode
    // cross-file resolution, where co-location alone is not a valid channel —
    // only the -I/FWIZ_PATH search path and the @include allow-list are.
    [[nodiscard]] std::string resolve_file_path(
            const std::string& ref, bool is_literal,
            std::vector<std::string>* searched = nullptr,
            bool exclude_base_dir = false) const {
        std::string name = ref;
        if (!is_literal && name.find('.') == std::string::npos) name += ".fw";

        auto try_path = [&](const std::string& candidate) -> std::string {
            std::error_code ec;
            const std::filesystem::path p(candidate);
            if (std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec)) {
                std::string abs;
                try { abs = std::filesystem::weakly_canonical(p).string(); }
                catch (const std::filesystem::filesystem_error&) { abs = candidate; }
                return abs;
            }
            return "";
        };

        // (1) Absolute path: use verbatim.
        if (std::filesystem::path(name).is_absolute()) {
            if (searched) searched->push_back(name);
            return try_path(name);
        }
        // (2) Relative to the including file's directory (skipped in strict mode).
        if (!exclude_base_dir) {
            const std::string dir = base_dir.empty() ? "." : base_dir;
            const std::string candidate = dir + "/" + name;
            if (searched) searched->push_back(candidate);
            if (std::string hit = try_path(candidate); !hit.empty()) return hit;
        }
        // (3) Each include_dirs entry (-I dirs, then FWIZ_PATH dirs).
        for (const auto& dir : include_dirs) {
            if (dir.empty()) continue;
            const std::string candidate = dir + "/" + name;
            if (searched) searched->push_back(candidate);
            if (std::string hit = try_path(candidate); !hit.empty()) return hit;
        }
        return "";
    }

    // Future #80 M2: strict-mode cross-file resolution. Scans the @include
    // allow-list (included_files_) for an entry whose filename stem matches
    // `stem`, returning its canonical abs_path or "" on miss. This is the ONLY
    // filesystem channel a strict-mode formula call may use (the base_dir
    // auto-probe is gated out); the search path is consulted separately by
    // load_sub_system via resolve_file_path.
    [[nodiscard]] std::string resolve_from_included(const std::string& stem) const {
        auto inc = std::find_if(included_files_.begin(), included_files_.end(),
            [&](const std::string& p) {
                return std::filesystem::path(p).stem().string() == stem;
            });
        return inc != included_files_.end() ? *inc : std::string{};
    }

    // Future #80 M2: comma-joined stems of every @include'd file, for the
    // strict-mode "currently included" diagnostic. Empty string when nothing
    // has been @include'd yet.
    [[nodiscard]] std::string list_included_stems() const {
        std::string out;
        for (const auto& p : included_files_) {
            if (!out.empty()) out += ", ";
            out += std::filesystem::path(p).stem().string();
        }
        return out;
    }

    // Future #80 M2: the helpful strict-mode resolution error. Names the call,
    // explains why it failed, and tells the user exactly how to fix it.
    [[nodiscard]] std::string build_strict_include_error(const std::string& file_part) const {
        std::string searched_dirs = base_dir.empty() ? "." : base_dir;
        for (const auto& dir : include_dirs)
            if (!dir.empty()) searched_dirs += ", " + dir;
        const std::string included = list_included_stems();
        return "Cannot resolve cross-file call '" + file_part
             + "' (strict-includes mode): add @include \"" + file_part
             + ".fw\" or place it on the include path (searched: " + searched_dirs + ")"
             + (included.empty() ? "" : ". Currently @include'd: " + included);
    }

    [[nodiscard]] static bool is_include_line(const std::string& line) {
        const std::string t = trim(line);
        // "@include" followed by end-of-token (space or quote) — guards against
        // a hypothetical "@includeX" annotation.
        if (t.rfind("@include", 0) != 0) return false;
        if (t.size() == 8) return true;  // bare "@include" (no path — caught later)
        const char c = t[8];
        return c == ' ' || c == '\t' || c == '"';
    }

    [[nodiscard]] static std::string extract_include_path(const std::string& line) {
        std::string p = trim(trim(line).substr(8));  // strip "@include"
        // Strip a single pair of surrounding double-quotes (quoted form
        // primary; unquoted tolerated after the trim above).
        if (p.size() >= 2 && p.front() == '"' && p.back() == '"')
            p = p.substr(1, p.size() - 2);
        return trim(p);
    }

    // Pre-pass: resolve and merge every `@include` directive, then blank the
    // line so the downstream section splitter / line loader ignore it. Each
    // included file is loaded into *this (its equations/constants merge into the
    // parent namespace) and recorded in `included_files_`. Cycle detection
    // reuses a thread-local set (distinct from load_sub_system's
    // `currently_loading`); base_dir is saved/restored around each recursive
    // load (load_file overwrites base_dir, which would otherwise break the
    // parent's subsequent file-relative formula-call resolution).
    void process_includes(std::vector<std::string>& lines) {
        static thread_local std::set<std::string> currently_including;
        // RAII restore of base_dir around the recursive load_file (R1).
        struct BaseDirGuard {
            FormulaSystem& sys; std::string saved;
            explicit BaseDirGuard(FormulaSystem& s) : sys(s), saved(s.base_dir) {}
            ~BaseDirGuard() { sys.base_dir = saved; }
        };
        struct IncludeGuard {
            std::set<std::string>& s; const std::string& k;
            ~IncludeGuard() { s.erase(k); }
        };
        for (auto& line : lines) {
            if (!is_include_line(line)) continue;
            const std::string raw_path = extract_include_path(line);
            if (raw_path.empty())
                throw std::runtime_error("@include with no path");
            std::vector<std::string> searched;
            const std::string abs_path = resolve_file_path(raw_path, /*is_literal=*/true, &searched);
            if (abs_path.empty()) {
                std::string msg = "@include \"" + raw_path + "\": file not found. Searched:";
                for (const auto& s : searched) msg += "\n  " + s;
                throw std::runtime_error(msg);
            }
            if (currently_including.count(abs_path))
                throw std::runtime_error("@include cycle detected: " + abs_path);
            currently_including.insert(abs_path);
            const IncludeGuard _ig{currently_including, abs_path};
            {
                const BaseDirGuard _bd{*this};  // restores base_dir on scope exit
                load_file(abs_path);            // merges content; overwrites base_dir
            }
            included_files_.insert(abs_path);
            line.clear();  // blank so split_sections / load_lines skip it
        }
    }

    void load_with_sections(const std::vector<std::string>& all_lines, const std::string& section) {
        auto filtered = all_lines;        // mutable copy; @include lines blanked in-place
        process_includes(filtered);       // Future #80 M1: resolve + merge @include directives
        sections_ = split_sections(filtered);
        // gen-3 cycle 2 (2026-05-14) / gen-5 cycle 3a (2026-05-15): walk
        // sections BEFORE top-level/section load so type_map_ is populated
        // before any equation that references a dim-section variable is
        // parsed.
        // Pass 1: dim sections (cycle 3a) — type_map_ populated before any
        // annotation parse references them.
        for (const auto& s : sections_)
            if (!s.name.empty() && is_dimension_section(s))
                register_dim_section(s);
        // Pass 2: predicate sections (cycle 3b) — USER_PREDICATE entries.
        // Set-name lookup inside predicate Conditions is dispatch-time, so
        // forward references between predicate sections (a refers to b
        // before b is registered in this loop) work transparently.
        for (const auto& s : sections_)
            if (!s.name.empty() && is_predicate_section(s))
                register_predicate_section(s);
        // Pass 3: function sections (cycle 3d) — FUNCTION_SECTION entries.
        // Ordering after predicate is conventional (matches the 4-flavor
        // enum order); function-section existential queries don't reference
        // other set names at parse time, so the pass-2/pass-3 ordering is
        // flexible.
        for (const auto& s : sections_)
            if (!s.name.empty() && is_function_section(s))
                register_function_section(s);
        if (sections_.size() <= 1 && section.empty())
            load_lines(filtered);  // @include lines blanked by process_includes
        else
            load_section(section);
        resolve_positional_calls();
        compute_rewrite_groups();  // regroup after user rules loaded
        resolve_diff_in_equations();  // Future #6: rewrite diff(...) calls
        resolve_integral_in_equations();  // Future #16 (M1): rewrite integral(...) calls
        resolve_aggregate_in_equations();  // gen-6 Step C: unroll formula-bodied aggregations
        trace_loaded();
    }

    // ------------------------------------------------------------------------
    // Generic post-load tree-rewriting primitive (Future.md #48, extracted
    // 2026-05-10 as part of Future #16 M1). Walks every equation RHS from
    // `up_to` onward through `rewriter`, then advances `up_to` to the new
    // tail. Equations only ever grow, so subsequent load_string calls skip
    // already-rewritten equations.
    //
    // Two consumers today: `resolve_diff_in_equations` and
    // `resolve_integral_in_equations`. Future tree-rewriting passes (e.g.
    // typed-binding predicates per Future #53) plug in here.
    // ------------------------------------------------------------------------
    template <typename Rewriter>
    void resolve_at_load(Rewriter rewriter, size_t& up_to) {
        const ExprArena::Scope scope(arena);
        // justified: starts mid-array at `up_to` (incremental dirty-flag)
        for (size_t i = up_to; i < equations.size(); ++i)
            equations[i].rhs = rewriter(equations[i].rhs);
        up_to = equations.size();
    }

    // ------------------------------------------------------------------------
    // Post-load symbolic differentiation (Future #6).
    //
    // Rewrites every `diff(target, var)` call in equation RHS expressions to
    // its derivative tree. Three cases (in order of preference):
    //
    //   1. `target` is a Var that is the LHS of a system equation
    //      → substitute the equation's RHS and call `symbolic_diff_simplified`.
    //
    //   2. `target` is a Var that is the `output_var` of a FormulaCall
    //      → inline the sub-system equation body via the same logic
    //        `derive_recursive` uses (substitute call.bindings into the body).
    //
    //   3. Otherwise: treat `target` as a literal expression (the inline form,
    //      e.g. `diff(x^2 + 1, x)`).
    //
    // Throws on `diff(<non-var second arg>, ...)` since the design forbids
    // `d/d(<expression>)` semantics — clearer parse-time error than silent
    // failure. Unknown function forms inside the target propagate `nullptr`
    // from `symbolic_diff_simplified`; that is converted into a `0` sentinel
    // so the equation remains parseable (matches the historical "treat
    // unknown as constant" behavior, surfaced via trace).
    // ------------------------------------------------------------------------
    void resolve_diff_in_equations() {
        resolve_at_load(
            [this](ExprPtr e) { return resolve_diff_calls(e); },
            diff_resolved_up_to_);
    }

    // Post-load symbolic integration (Future #16, M1). Same 3-case dispatch
    // shape as diff (named-var → equation RHS / FormulaCall output / literal),
    // dispatching through `symbolic_integrate_simplified`. Unrecognized forms
    // preserve the original `integral(...)` FUNC_CALL.
    void resolve_integral_in_equations() {
        resolve_at_load(
            [this](ExprPtr e) { return resolve_integral_calls(e); },
            integral_resolved_up_to_);
    }

    // Post-load aggregate unroll (gen-6 Step C). Formula-bodied aggregations
    // (`sum(score(roll), roll in [1..6])`) cannot resolve at the expr.h simplify
    // layer because the formula body must be loaded and substituted with concrete
    // iterator values. This pass — mirroring diff/integral — unrolls every
    // reducer node into N concrete FormulaCall terms folded by `fold_aggregate`,
    // the same fold-policy table the simplify-time unroll uses. Pure-numeric and
    // pure-expression bodies (Steps A/B) are already handled at simplify time and
    // are simply re-folded here to identical Nums (idempotent).
    void resolve_aggregate_in_equations() {
        resolve_at_load(
            [this](ExprPtr e) { return resolve_aggregate_calls(e); },
            agg_resolved_up_to_);
    }

    // Walks `e` post-order; replaces every aggregate reducer FUNC_CALL with its
    // unrolled fold tree (see try_unroll_aggregate_with_calls). Nested aggregates
    // resolve inner-first via tree_map's post-order recursion.
    [[nodiscard]] ExprPtr resolve_aggregate_calls(ExprPtr e) {
        return tree_map(e, [&](ExprPtr node) -> ExprPtr {
            if (node->type != ExprType::FUNC_CALL || !is_aggregate_reducer(node->name))
                return node;
            return try_unroll_aggregate_with_calls(node);
        });
    }

    // Clone a pre-extracted FormulaCall for one aggregate domain value: fresh
    // output_var (_fcN), and `var → Num(v)` substituted into EVERY binding value
    // (so `atk=Var("f")` becomes `atk=Num(v)`, and compound bindings like
    // `atk=f+1` fold too). Pushes the clone into formula_calls and returns its
    // new output_var. Used by Shape A-named (Shape B keeps its own inline clone
    // loop because it must also rewrite the range-literal binding `atk=[1..6]`,
    // which is not a Var substitution). ONE-LEVEL: substitutes into this call's
    // own bindings only; nested/chained FormulaCall bindings are out of scope
    // (cartesian/nested cycle).
    [[nodiscard]] std::string clone_call_with_subst(
            const FormulaCall& tmpl, const std::string& var, double v) {
        FormulaCall clone = tmpl;
        clone.output_var = "_fc" + std::to_string(next_call_id_++);
        for (auto& [param, val] : clone.bindings)
            val = simplify(substitute(val, var, Expr::Num(v)));
        formula_calls.push_back(clone);
        return clone.output_var;
    }

    // Unroll one aggregate node whose body may contain formula calls. Shapes:
    //
    //   Shape A — explicit iterator: sum(body, Var(iter), range(lo, hi[, step])).
    //     For each domain value v: term = simplify(substitute(body, iter, Num(v))),
    //     then extract_positional_calls on the term so any nested formula call
    //     (score(v)) becomes a FormulaCall. fold_aggregate folds the terms.
    //
    //   Shape A-named — explicit iterator inside a NAMED formula-call binding:
    //     sum(dmg(atk=f, def=k), f in [...]). The call is pre-extracted at parse
    //     time, so the body reaching here is a bare Var("_fcN") and the iterator
    //     `f` lives in the call's bindings. Clone the call per domain value with
    //     `iter → Num(v)` substituted into its bindings (clone_call_with_subst).
    //
    //   Shape B — broadcast: sum(Var(_fcN)) where _fcN is a FormulaCall already
    //     extracted by extract_formula_calls with a range-literal binding (atk=[1..6]).
    //     Lift the single range binding to an anonymous iterator: clone the call N
    //     times with concrete Num(v) bindings (and lockstep substitution for any
    //     other binding equal to Var(range_param)). fold_aggregate folds the clones.
    //
    // Broadcast handles all four populations: 1 range → lift; 1 range + lockstep
    // → clone with concrete bindings; 2+ ranges → UNEVALUATED + stderr warning
    // (cartesian is a later step); 0 ranges → UNEVALUATED (no iterator domain).
    // Returns the original node unchanged when the shape is unsupported or the
    // range is symbolic (folds once bound). Never returns a node that still
    // contains a reducer for a numeric-bound formula body (graceful-degrade:
    // missing sub-systems become unresolved FUNC_CALLs in the fold, not a
    // surviving sum() that simplify-unroll would mis-fold as N*body(last)).
    [[nodiscard]] ExprPtr try_unroll_aggregate_with_calls(ExprPtr node) {
        const std::string& name = node->name;

        // Shape A: explicit iterator (bodied 3-arg, or count 2-arg body-free).
        const bool is_count = (name == "count");
        const size_t want_arity = is_count ? 2 : 3;
        if (node->args.size() == want_arity) {
            const ExprPtr iter = is_count ? node->args[0] : node->args[1];
            const ExprPtr rng  = is_count ? node->args[1] : node->args[2];
            if (is_var(iter)) {
                std::vector<double> values;
                if (!extract_range_values(rng, values)) return node;  // symbolic → unevaluated
                const std::string iter_var = iter->name;
                const ExprPtr body = is_count ? nullptr : node->args[0];

                // Shape A-named: the body is a bare Var("_fcN") naming a formula
                // call extracted at PARSE time (`sum(dmg(atk=f, def=k), f in ...)`
                // — named-arg calls are pre-extracted, so the iterator `f` lives
                // in the call's BINDINGS, not in the body expression). Substituting
                // the inert body would never touch `f`; instead CLONE the call per
                // domain value with `iter_var → Num(v)` applied to its bindings —
                // exactly Shape B's clone mechanism, generalized to an explicit
                // (rather than range-literal-derived) iterator. ONE-LEVEL only:
                // bindings are `atk=Num(i), def=Var("k")`; nested/chained call
                // bindings are out of scope (cartesian/nested cycle).
                if (!is_count && is_var(body)) {
                    const FormulaCall* orig = nullptr;
                    // not std::find_if: needs the element's address to snapshot it
                    for (const auto& c : formula_calls)
                        // cppcheck-suppress useStlAlgorithm
                        if (c.output_var == body->name) { orig = &c; break; }
                    if (orig && formula_call_bindings_contain(body, iter_var)) {
                        const std::string tmpl_var = body->name;
                        const FormulaCall tmpl = *orig;  // snapshot (vector may grow)
                        ExprPtr folded = fold_aggregate(name, values, [&](double v) {
                            return Expr::Var(clone_call_with_subst(tmpl, iter_var, v));
                        });
                        // Drop the original template call: it still carries the
                        // UN-substituted iterator binding (`atk=f`, f unbound), and
                        // it is no longer referenced by any equation (the fold uses
                        // the per-value clones). Leaving it would let the solver
                        // probe its unbound iterator across the whole numeric range
                        // during a reverse solve, returning spurious roots.
                        // not std::remove_if: single-element removal by output_var
                        for (auto it = formula_calls.begin(); it != formula_calls.end(); ++it)
                            // cppcheck-suppress useStlAlgorithm
                            if (it->output_var == tmpl_var) { formula_calls.erase(it); break; }
                        return folded ? folded : node;
                    }
                }

                ExprPtr folded = fold_aggregate(name, values,
                    is_count ? std::function<ExprPtr(double)>(nullptr)
                             : std::function<ExprPtr(double)>([&](double v) {
                        ExprPtr term = simplify(substitute(body, iter_var, Expr::Num(v)));
                        std::vector<FormulaCall> term_calls;
                        term = extract_positional_calls(term, term_calls);
                        // not std::transform: move-append into a different container
                        for (auto& c : term_calls)
                            // cppcheck-suppress useStlAlgorithm
                            formula_calls.push_back(std::move(c));
                        return term;
                    }));
                return folded ? folded : node;
            }
        }

        // Shape B: broadcast — arity-1 reducer over a FormulaCall output var.
        if (node->args.size() == 1 && is_var(node->args[0])) {
            const std::string fc_name = node->args[0]->name;
            FormulaCall* orig = nullptr;
            // not std::find_if: needs the element's address (&c) to read bindings below
            for (auto& c : formula_calls)
                // cppcheck-suppress useStlAlgorithm
                if (c.output_var == fc_name) { orig = &c; break; }
            if (!orig) return node;  // not a formula call → unsupported shape

            // Count range-literal bindings; remember the single range param + domain.
            std::string range_param;
            std::vector<double> values;
            int range_count = 0;
            for (auto& [param, val] : orig->bindings) {
                std::vector<double> v;
                if (extract_range_values(val, v)) {
                    range_count++;
                    range_param = param;
                    values = v;
                }
            }
            if (range_count == 0) return node;       // 0 ranges → unevaluated
            if (range_count >= 2) {                   // 2+ ranges → unevaluated + warn
                std::cerr << "warning: aggregation over multiple ranges ("
                          << range_param << ", …) is not supported (cartesian product "
                             "is a future step); leaving "
                          << name << "(...) unevaluated\n";
                return node;
            }

            // Snapshot the template (orig may dangle as formula_calls grows below).
            const FormulaCall tmpl = *orig;
            ExprPtr folded = fold_aggregate(name, values, [&](double v) {
                FormulaCall clone = tmpl;
                clone.output_var = "_fc" + std::to_string(next_call_id_++);
                for (auto& [param, val] : clone.bindings) {
                    if (param == range_param) val = Expr::Num(v);          // range → concrete
                    else if (is_var(val) && val->name == range_param)
                        val = Expr::Num(v);                                // lockstep (def=atk)
                }
                formula_calls.push_back(clone);
                return Expr::Var(clone.output_var);
            });
            return folded ? folded : node;
        }

        return node;  // unsupported shape — leave unevaluated
    }

    // Walks `e` post-order; replaces any `diff(target, var)` FUNC_CALL with the
    // corresponding derivative tree (simplified). The post-order tree_map
    // recursion handles nested `diff(diff(x^3, x), x)` naturally — by the time
    // the outer node is examined, inner diff(...) calls are already expanded.
    [[nodiscard]] ExprPtr resolve_diff_calls(ExprPtr e) {
        return tree_map(e, [&](ExprPtr node) -> ExprPtr {
            if (node->type != ExprType::FUNC_CALL || node->name != "diff" ||
                node->args.size() != 2) return node;
            const Expr* target_expr = node->args[0];
            const Expr* var_expr    = node->args[1];
            if (!is_var(var_expr))
                throw std::runtime_error("diff: second argument must be a variable name");
            const std::string& var = var_expr->name;

            ExprPtr derived = nullptr;

            // Case 1: target is a Var that names a system equation.
            if (is_var(target_expr)) {
                const std::string& tname = target_expr->name;
                // not std::find_if: body invokes symbolic_diff_simplified (allocates) and assigns captured `derived`
                for (const auto& eq : equations) {
                    // cppcheck-suppress useStlAlgorithm
                    if (eq.lhs_var == tname) {
                        derived = symbolic_diff_simplified(*eq.rhs, var);
                        break;
                    }
                }
                // Case 2: target is a Var that names a FormulaCall output.
                if (!derived) {
                    for (const auto& call : formula_calls) {
                        if (call.output_var != tname) continue;
                        const Expr* unfolded = unfold_formula_call_body(call);
                        if (unfolded) {
                            derived = symbolic_diff_simplified(*unfolded, var);
                        }
                        break;
                    }
                }
            }

            // Case 3 (or fallback when 1/2 produced nullptr): treat target as
            // a literal expression.
            if (!derived) {
                derived = symbolic_diff_simplified(*target_expr, var);
            }

            // If everything failed (unknown function inside target, etc.), keep
            // the original `diff(...)` call so downstream stages can surface a
            // useful error rather than a silent zero.
            if (!derived) {
                trace.step("  diff: cannot differentiate " + expr_to_string(target_expr)
                           + " w.r.t. " + var + " — keeping symbolic form");
                return node;
            }
            return derived;
        });
    }

    // Walks `e` post-order; replaces any `integral(target, var)` FUNC_CALL
    // with the corresponding antiderivative tree (simplified). Mirrors
    // `resolve_diff_calls` exactly, dispatching through `symbolic_integrate_simplified`.
    // Three cases (in order of preference): equation RHS / FormulaCall output
    // / literal expression. Unrecognized forms preserve the original
    // `integral(...)` call so downstream stages can surface a useful error.
    //
    // M2: also handles the 4-arg definite form `integral(f, x, a, b)`.
    // Strategy: try symbolic F(b) - F(a); if that fails, fall back to
    // adaptive Simpson when both bounds evaluate numerically. If both paths
    // fail, preserve the unevaluated FUNC_CALL.
    [[nodiscard]] ExprPtr resolve_integral_calls(ExprPtr e) {
        return tree_map(e, [&](ExprPtr node) -> ExprPtr {
            if (node->type != ExprType::FUNC_CALL || node->name != "integral")
                return node;
            const size_t nargs = node->args.size();
            if (nargs != 2 && nargs != 4) return node;
            const Expr* target_expr = node->args[0];
            const Expr* var_expr    = node->args[1];
            if (!is_var(var_expr))
                throw std::runtime_error("integral: second argument must be a variable name");
            const std::string& var = var_expr->name;

            ExprPtr antideriv = nullptr;

            // Case 1: target is a Var that names a system equation.
            if (is_var(target_expr)) {
                const std::string& tname = target_expr->name;
                // not std::find_if: body invokes symbolic_integrate_simplified (allocates) and assigns captured `antideriv`
                for (const auto& eq : equations) {
                    // cppcheck-suppress useStlAlgorithm
                    if (eq.lhs_var == tname) {
                        antideriv = symbolic_integrate_simplified(*eq.rhs, var);
                        break;
                    }
                }
                // Case 2: target is a Var that names a FormulaCall output.
                if (!antideriv) {
                    for (const auto& call : formula_calls) {
                        if (call.output_var != tname) continue;
                        const Expr* unfolded = unfold_formula_call_body(call);
                        if (unfolded) {
                            antideriv = symbolic_integrate_simplified(*unfolded, var);
                        }
                        break;
                    }
                }
            }

            // Case 3 (or fallback when 1/2 produced nullptr): treat target as
            // a literal expression.
            if (!antideriv) {
                antideriv = symbolic_integrate_simplified(*target_expr, var);
            }

            // 2-arg: indefinite integral. Substitute the antiderivative
            // directly (or keep symbolic if integration failed).
            if (nargs == 2) {
                if (!antideriv) {
                    trace.step("  integral: cannot integrate " + expr_to_string(target_expr)
                               + " w.r.t. " + var + " — keeping symbolic form");
                    return node;
                }
                return antideriv;
            }

            // 4-arg: definite integral. Symbolic path: F(b) - F(a) when an
            // antiderivative is available; otherwise adaptive Simpson on the
            // raw integrand. Both fail → keep symbolic FUNC_CALL.
            const Expr* lo_expr = node->args[2];
            const Expr* hi_expr = node->args[3];
            if (antideriv) {
                // const_cast: ExprPtr is `Expr*`; the arena owns nodes mutably
                // even when held through const Expr* aliases (substitute does
                // not mutate through this argument).
                auto F_hi = simplify(substitute(antideriv, var, const_cast<Expr*>(hi_expr)));
                auto F_lo = simplify(substitute(antideriv, var, const_cast<Expr*>(lo_expr)));
                auto diff = simplify(Expr::BinOpExpr(BinOp::SUB, F_hi, F_lo));
                // If the symbolic difference collapses to a finite numeric,
                // we're done — return the constant. If it stays symbolic
                // (free variables in bounds), return the symbolic form too.
                auto val = evaluate(*diff);
                if (val) {
                    if (std::isfinite(val.value())) return Expr::Num(val.value());
                    // NaN/inf — fall through to numeric path.
                } else {
                    return diff;  // symbolic bounds — keep the closed form
                }
            }

            // Numeric fallback: adaptive Simpson. Requires both bounds to
            // evaluate to finite numbers.
            auto lo_val = evaluate(*lo_expr);
            auto hi_val = evaluate(*hi_expr);
            if (lo_val && hi_val
                && std::isfinite(lo_val.value()) && std::isfinite(hi_val.value())) {
                // Build a numeric closure: substitute `var` with sample x,
                // simplify+evaluate. Constant-fold once outside the loop is
                // not safe — different x produces different trees.
                ExprPtr target_ptr = const_cast<Expr*>(target_expr);
                auto fn = [&](double x) -> double {
                    const ExprPtr subst = substitute(target_ptr, var, Expr::Num(x));
                    auto v = evaluate(*subst);
                    return v ? v.value() : std::nan("");
                };
                const double result = adaptive_simpson(fn, lo_val.value(), hi_val.value());
                if (std::isfinite(result)) return Expr::Num(result);
            }

            trace.step("  integral: cannot evaluate definite " + expr_to_string(target_expr)
                       + " w.r.t. " + var + " — keeping symbolic form");
            return node;
        });
    }

    // Inline a FormulaCall body into the parent scope: substitute the
    // call.bindings (sub-system var → parent expr) into the sub-system
    // equation RHS that produces `call.query_var`. Mirrors the FORMULA_FWD
    // unfold path in `derive_recursive` (system.h ~line 2860). Returns the
    // unfolded ExprPtr or nullptr if no matching sub-system equation exists.
    //
    // LIMITATION: when a sub-system has multiple equations defining the
    // output (piecewise/conditional formulas like `abs` defined via two
    // `iff` branches), only the FIRST matching equation is used. This
    // silently uses one branch's derivative. See Future #51 for the
    // multi-branch follow-up.
    [[nodiscard]] ExprPtr unfold_formula_call_body(const FormulaCall& call) const {
        try {
            auto& sub_sys = load_sub_system(call.file_stem);
            for (auto& eq : sub_sys.equations) {
                if (eq.lhs_var != call.query_var) continue;
                ExprPtr unfolded = eq.rhs;
                for (auto& [sv, pe] : call.bindings)
                    unfolded = substitute(unfolded, sv, pe);
                for (auto& [k, v] : sub_sys.defaults) {
                    if (call.bindings.count(k)) continue;
                    if (k == call.query_var) continue;
                    unfolded = substitute(unfolded, k, Expr::Num(v));
                }
                return simplify(unfolded);
            }
        // NOLINTNEXTLINE(bugprone-empty-catch) — sub-system load failure → leave unfolded null, caller falls through to literal-expression path
        } catch (const std::runtime_error&) {}
        return nullptr;
    }

    void load_string(const std::string& source, const std::string& label = "<inline>",
                     const std::string& section = "") {
        const ExprArena::Scope scope(arena);
        if (base_dir.empty()) base_dir = ".";
        if (rewrite_rules.empty()) load_builtins();
        if (source_label_.empty()) source_label_ = label;
        trace.step("loading " + label);
        std::istringstream ss(source);
        load_with_sections(read_all_lines(ss), section);
    }

    // Register a custom C++ function with optional .fw definition for inverse solving.
    // The .fw definition should include a section header and equations, e.g.:
    //   "[sigmoid(x) -> result]\n@extern sigmoid\nx = -log(1/result - 1)\n"
    void register_function(const std::string& name, double(*fn)(double),
                           const std::string& fw_def = "") {
        custom_functions_[name] = fn;
        if (!fw_def.empty())
            custom_function_defs_[name] = fw_def;
    }

    void load_file(const std::string& path, const std::string& section = "") {
        const ExprArena::Scope scope(arena);
        if (path.empty())
            throw std::runtime_error("No file path provided");
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
            throw std::runtime_error("Path is a directory, not a file: " + path);

        base_dir = std::filesystem::path(path).parent_path().string();
        if (base_dir.empty()) base_dir = ".";
        if (rewrite_rules.empty()) load_builtins();
        if (source_label_.empty())
            source_label_ = std::filesystem::path(path).stem().string();

        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open file: " + path);

        trace.step("loading " + path);
        load_with_sections(read_all_lines(f), section);

        // Extended trace for file loads
        if (trace.show_steps()) {
            for (auto& fc : formula_calls) {
                std::string s = "  formula call: " + fc.file_stem + "(" + fc.query_var + "=?" + fc.output_var;
                for (auto& [sv, expr] : fc.bindings) { s += ", "; s += sv; s += "="; s += expr_to_string(expr); }
                trace.step(s + ")");
            }
        }
    }

    // ────────────── Subsection: CLI orchestration helpers ──────────────

    [[nodiscard]] static bool approx_equal(double a, double b) {
        if (std::isnan(a) || std::isnan(b)) return false;
        if (std::isinf(a) || std::isinf(b)) return a == b;
        const double eps = std::max(EPSILON_REL, EPSILON_REL * std::max(std::abs(a), std::abs(b)));
        return std::abs(a - b) < eps;
    }

    std::vector<VerifyResult> verify_variable(
        const std::string& target, double known_value,
        std::map<std::string, double> bindings) const
    {
        const ExprArena::Scope scope(arena);
        const BudgetGuard budget_guard; // Part C
        std::vector<VerifyResult> results;
        bindings.erase(target);
        for (auto& [k, v] : defaults)
            if (k != target && !bindings.count(k)) bindings[k] = v;

        auto try_verify_expr = [&](const ExprPtr& expr, const std::string& desc) {
            std::set<std::string> vars;
            collect_vars(expr, vars);
            ExprPtr resolved = expr;
            for (auto& v : vars) {
                if (v == target) return;
                if (auto it = bindings.find(v); it != bindings.end()) {
                    resolved = substitute(resolved, v, Expr::Num(it->second));
                } else {
                    try {
                        auto b2 = bindings;
                        DeadEndSet de;
                        std::set<std::string> v0{target};
                        const double val = solve_recursive(v, b2, v0, 0, de);
                        resolved = substitute(resolved, v, Expr::Num(val));
                    } catch (const std::runtime_error&) { return; }
                }
            }
            auto computed_opt = evaluate(simplify(resolved));
            if (!computed_opt) return;
            const double computed = computed_opt.value();
            if (std::isnan(computed) || std::isinf(computed)) return;
            results.push_back({desc, computed, approx_equal(computed, known_value)});
        };

        auto try_verify_formula = [&](const FormulaCall& call, const std::string& resolve_var,
                                      const std::string& desc) {
            try {
                auto sub_binds = prepare_sub_bindings(call, bindings, {}, 0, target, false);
                auto& sub_sys = load_sub_system(call.file_stem);
                const double computed = sub_sys.resolve(resolve_var, sub_binds);
                if (!std::isnan(computed) && !std::isinf(computed))
                    results.push_back({desc, computed, approx_equal(computed, known_value)});
            } catch (const std::runtime_error&) { return; }
        };

        enumerate_candidates(target, [&](const Candidate& c) {
            switch (c.type) {
                case CandidateType::EXPR:
                    try_verify_expr(c.expr, c.desc); break;
                case CandidateType::FORMULA_FWD:
                    try_verify_formula(*c.call, c.call->query_var, c.desc); break;
                case CandidateType::FORMULA_REV:
                    try_verify_formula(*c.call, c.sub_var, c.desc); break;
                case CandidateType::NUMERIC: break; // numeric not used for verify
                case CandidateType::COUNT_: assert(false); break;
            }
            return false; // verify collects ALL, never stops
        });

        return results;
    }

    // --- Derive (symbolic) ---

    // Prepare symbolic bindings for derive
    [[nodiscard]] std::map<std::string, ExprPtr> prepare_derive_bindings(
            const std::string& target,
            const std::map<std::string, double>& numeric_bindings,
            const std::map<std::string, std::string>& symbolic_bindings) const {
        std::map<std::string, ExprPtr> bindings;
        for (auto& [k, v] : numeric_bindings) bindings[k] = Expr::Num(v);
        for (auto& [k, v] : symbolic_bindings) bindings[k] = Expr::Var(v);
        auto& consts = builtin_constants();
        for (auto& [k, v] : defaults)
            if (!bindings.count(k) && k != target && !consts.count(k))
                bindings[k] = Expr::Num(v);
        return bindings;
    }

    // Build a user-alias table: every `name = <num>` default from this
    // FormulaSystem and its cached sub-systems, keyed for recognition by
    // expr_recognize_constants / fmt_exact_double.
    //
    //   - Entries whose NAME collides with a builtin constant (pi, e, phi)
    //     are excluded — the builtin symbolic form always wins.
    //   - Same NAME across multiple systems AGREEING in value (within
    //     EPSILON_REL) → one unqualified entry.
    //   - Same NAME across multiple systems DISAGREEING → one qualified
    //     "stem.name" entry per distinct value, no unqualified form.
    //
    // The stem is the FormulaSystem's source_label_ (file stem from load_file,
    // or the label argument from load_string). Called at format_derived time
    // (after enumerate_candidates has populated sub_systems).
    //
    // Two entry points:
    //   - populate_aliases_() — side-effect-only (writes the `aliases_` cache);
    //     used by resolve/resolve_all and the explore-fast-path branch in main.
    //   - build_alias_table() — pure query (returns a copy of the freshly
    //     populated cache); used by format_derived and derive_all.
    void populate_aliases_() const {
        auto& builtins = builtin_constants();
        // Raw (name, value, stem) tuples.
        struct Entry { double value; std::string stem; };
        std::map<std::string, std::vector<Entry>> grouped;
        auto add_entries = [&](const std::map<std::string, double>& defs,
                               const std::string& stem) {
            for (const auto& [name, value] : defs) {
                if (builtins.count(name)) continue;
                grouped[name].push_back({value, stem});
            }
        };
        add_entries(this->defaults, this->source_label_);
        for (const auto& [key, sub_ptr] : this->sub_systems) {
            if (!sub_ptr) continue;
            (void)key;
            add_entries(sub_ptr->defaults, sub_ptr->source_label_);
        }

        std::map<std::string, double> out;
        for (auto& [name, entries] : grouped) {
            // All agree? → unqualified.
            const double first_val = entries.front().value;
            const bool all_agree = std::all_of(entries.begin(), entries.end(),
                [first_val](const auto& e) { return approx_equal(e.value, first_val); });
            if (all_agree) {
                out[name] = first_val;
                continue;
            }
            // Disagreement → emit one qualified entry per distinct value.
            // Skip entries with empty stem (can't qualify) to keep output
            // unambiguous.
            std::set<std::string> seen_qualified;
            for (const auto& e : entries) {
                if (e.stem.empty()) continue;
                const std::string qname = e.stem + "." + name;
                if (seen_qualified.insert(qname).second)
                    out[qname] = e.value;
            }
        }
        // Cache as a side effect so fmt_trace's alias-table fallback can read
        // without rebuilding (called from every resolve/resolve_all entry).
        aliases_ = std::move(out);
    }

    [[nodiscard]] std::map<std::string, double> build_alias_table() const {
        populate_aliases_();
        return aliases_;
    }

    // Format a derived ExprPtr as a string.
    // Default (exact) mode:
    //   - If the tree collapses to a pure number, emit fmt_exact_double —
    //     this yields 'pi' for M_PI, '5 / 3' for 1.666..., etc., matching
    //     the solve path and closing the former solve/derive asymmetry.
    //   - Otherwise, walk the tree with expr_recognize_constants for clean
    //     symbolic output (log(2), sqrt(3), 1/3 fractions in coefficients).
    // --approximate mode:
    //   - Substitute builtin constants (pi, e, phi) with their numeric values,
    //     then re-simplify so adjacent Nums fold (2 * pi * r → 6.2831 * r).
    //   - If the result is fully numeric, emit fmt_num; otherwise stringify
    //     the folded tree without triggering recognition (we don't want
    //     freshly-folded 3.14159 to get re-promoted back to 'pi').
    [[nodiscard]] std::string format_derived(const ExprPtr& result) const {
        // Back-compat entry point: build the alias table on the fly. Callers in
        // hot loops (e.g. derive_all) should pre-compute the table once and
        // use the overload below.
        return format_derived(result, build_alias_table());
    }

    [[nodiscard]] std::string format_derived(const ExprPtr& result,
                               const std::map<std::string, double>& aliases) const {
        // format_derived allocates via the arena (fmt_exact_double builds
        // Num nodes; substitute_builtin_constants rewrites the tree). Open
        // our own scope so callers don't have to — scopes nest, and the
        // cost of an extra stack frame is negligible for a one-shot format.
        const ExprArena::Scope scope(arena);
        // Distribute division over addition when the denominator is a numeric
        // literal, then re-simplify. This exposes like-terms hidden inside
        // (a + b) / k nodes so the simplifier can cancel them — e.g.
        //   -b/2 - c/2 + (b+4)/2 - 2  →  -c/2
        // Cross-equation elimination often emits such shapes; local to derive
        // output so the general simplifier is not affected.
        auto distributed = simplify(distribute_over_sum(result));
        if (approximate_mode) {
            const auto* subbed = simplify(substitute_builtin_constants(distributed));
            if (auto val = evaluate(subbed)) {
                if (!std::isinf(val.value())) return fmt_num(val.value());
            }
            return expr_to_string(subbed);
        }
        if (auto val = evaluate(distributed)) {
            // Checked<double> already excludes NaN; only guard against infinity.
            if (!std::isinf(val.value())) return fmt_exact_double(val.value(), aliases);
        } else {
            trace.calc("derive: symbolic result (cannot evaluate)");
        }
        // Recognize constants and fractions in the expression tree
        const auto* recognized = expr_recognize_constants(distributed, aliases);
        return expr_to_string(recognized);
    }

    // ────────────── Subsection: Derive ──────────────

    // Derive single result (backwards compatible)
    [[nodiscard]] std::string derive(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings) const {
        const ExprArena::Scope scope(arena);
        auto bindings = prepare_derive_bindings(target, numeric_bindings, symbolic_bindings);
        DeadEndSet dead_ends; // Fix 1: per-top-level-query dead-end set
        auto result = derive_recursive(target, bindings, {}, 0, dead_ends);
        if (!result) throw std::runtime_error("Cannot derive equation for '" + target + "'");
        return format_derived(result);
    }

    // Derive ALL results (for multi-valued inversions: abs, quadratic, etc.)
    //
    // CSE parameters (Option C — top-N by value):
    //   out_helpers   — if non-null AND cse_threshold >= 1, populated with
    //                   "tN = expr" preamble lines (in topological order).
    //   cse_threshold — cap on helper count; the top-N candidates by value
    //                   = (count-1)*(leaves-1) are kept (0 disables CSE).
    //   output_cap    — if > 0, truncate winners to first N (after canonicity
    //                   sort) BEFORE the CSE pass, so helpers reflect printed
    //                   equations only (replaces caller-side resize).
    [[nodiscard]] std::vector<std::string> derive_all(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings,
                       std::vector<std::string>* out_helpers = nullptr,
                       int cse_threshold = 0,
                       int output_cap = 0) const {
        const ExprArena::Scope scope(arena);
        const SimplifyContext simplify_ctx{&type_map_, &set_definitions_};
        const RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &numeric_bindings, &custom_functions_, &simplify_ctx);
        const FuncInverterGuard fi_guard(make_func_inverter());
        const ExistenceCheckerGuard ec_guard(  // cycle 3d: wires FUNCTION_SECTION dispatch
            [this](const std::string& set_name, double v) -> bool {
                return this->exists_for_function_section(set_name, v);
            });
        auto bindings = prepare_derive_bindings(target, numeric_bindings, symbolic_bindings);

        // Alias table is stable across the whole call; build once and reuse
        // for every format_derived invocation below (sentinel hashing path and
        // final emit phase).
        const auto aliases = build_alias_table();

        // --- Semantic fingerprint dedup setup (2026-04-19 cycle; #12f extended 2026-05-09) ---
        // Build 5 prime test points for every free variable in the output.
        // Free vars are the VALUES of symbolic_bindings — the aliased names
        // that actually appear as VARs in the derived expression after
        // build_alias_table() substitution (see ~l.880). Schwartz–Zippel:
        // distinct small primes per variable per row minimize accidental
        // cancellations.
        //
        // Row construction: 5 explicit (small,large) prime pairs, plus
        // cyclic-prime fill for free vars beyond column 1. The pairs are
        // chosen so that, for typical 2-side-and-target-angle geometry
        // workloads (e.g., triangle.fw with `a` ~ small integer), most rows
        // satisfy both the triangle inequality and the asin/acos input-domain
        // constraint. This adds branch-cut discriminating power to expose
        // semantic non-duplicates that 3 close primes cannot distinguish (e.g.
        // `b*sin(X)/4` vs `sin(pi - asin(c*sin(X)/4)) * b/c`, which coincide
        // on small-magnitude inputs but diverge when the asin argument exits
        // [-1, 1] for one variant but not the other). Resolves Future.md #12f.
        //
        // For >2 free vars, the 3rd column onward uses the original
        // cyclic-prime scheme — geometry-domain heuristics don't apply
        // generically beyond two variables.
        std::vector<std::string> free_vars;
        free_vars.reserve(symbolic_bindings.size());
        for (auto& [k, v] : symbolic_bindings) { (void)k; free_vars.push_back(v); }
        // Per-row (col0, col1) pair: covers obtuse-A (small, small),
        // mirror-asymmetric pairs, and acute-A (twin-prime medium and
        // medium/large). All consecutive cycles satisfy |Δ| ≤ 4 OR are
        // explicit pairs that the M1 branch test exercises analogously.
        static constexpr double row_pairs[5][2] = {
            {2.0, 3.0},    // obtuse A (b²+c² < typical a²); also covers
                           //   small-magnitude where asin args stay in domain
            {3.0, 2.0},    // mirror — distinguishes b/c-asymmetric forms
            {5.0, 7.0},    // acute A; gap=2; medium magnitude
            {7.0, 5.0},    // mirror
            {11.0, 13.0},  // acute A; gap=2; twin primes; large magnitude
                           //   exercises forms that domain-fail at smaller scales
        };
        // Cyclic primes for 3rd+ free vars (geometry heuristics don't apply).
        static constexpr double primes[5] = {2.0, 3.0, 5.0, 7.0, 11.0};
        std::vector<std::map<std::string, double>> test_points(5);
        // justified: row_pairs and primes are 2D/1D indexed lookup tables
        for (size_t i = 0; i < 5; i++) {
            for (size_t j = 0; j < free_vars.size(); j++) {
                if (j < 2) {
                    test_points[i][free_vars[j]] = row_pairs[i][j];
                } else {
                    test_points[i][free_vars[j]] = primes[(i + j) % 5];
                }
            }
        }

        // Winners map: fingerprint_key → {score, representative ExprPtr}.
        // Key shape uses a leading discriminator byte to make the two buckets
        // (empty-fp sentinel vs real fingerprint) structurally disjoint:
        //   {0, fp_0, fp_1, ..., fp_k}  — real fingerprint buckets (sort first).
        //   {1, counter}                — sentinel (sorts last → always-NaN
        //                                  candidates appear at the bottom
        //                                  after the emit-loop ascending
        //                                  canonicity sort).
        // The discriminator guarantees no cross-bucket collision is possible
        // regardless of fingerprint magnitude.
        using Key = std::vector<int64_t>;
        std::map<Key, std::pair<std::pair<int, int>, ExprPtr>> winners;
        int64_t empty_fp_counter = 0;

        // Substitute all non-free numeric bindings (e.g. defaults like
        // `deg = 0.01745...`) into a fingerprint probe so evaluation sees
        // concrete numbers for every Var node that isn't a free variable.
        // Without this, a surviving Var("deg") makes evaluate() return
        // empty and the candidate falls into the unique-sentinel path,
        // preventing the merge we want.
        auto subst_for_fingerprint = [&](ExprPtr e) {
            if (!e) return e;
            std::set<std::string> const free_set(free_vars.begin(), free_vars.end());
            for (auto& [name, val] : bindings) {
                if (free_set.count(name)) continue;
                if (name == target) continue;
                if (auto nv = evaluate(*val)) {
                    e = substitute(e, name, Expr::Num(nv.value()));
                }
            }
            return e;
        };

        // Empty-fingerprint candidates (all test points domain-excluded) use
        // a formatted-string sentinel: identical rendered output collapses
        // to one entry, but structurally distinct "always-NaN" trees (e.g.
        // log(b) vs log(-b) at positive test points) stay separated by
        // their distinct strings. This sits within the design's intent —
        // the rule "must NOT merge" domain-distinct candidates is upheld
        // (distinct strings → distinct keys), while the pathological case
        // where many candidates format identically collapses gracefully.
        std::map<std::string, int64_t> empty_fp_keys;
        auto consider_result = [&](const ExprPtr& result) {
            if (!result) return;
            auto probe = subst_for_fingerprint(result);
            auto fp = fingerprint_expr(probe, free_vars, test_points);
            Key key;
            if (fp.empty()) {
                // Hash by formatted string so structurally different
                // always-NaN candidates stay separate, but exact string
                // clones collapse.
                auto s = format_derived(result, aliases);
                auto [it, inserted] = empty_fp_keys.emplace(s, empty_fp_counter);
                if (inserted) empty_fp_counter++;
                key = {1, it->second};  // sentinel discriminator (sorts last)
            } else {
                key.reserve(fp.size() + 1);
                key.push_back(0);  // real-fingerprint discriminator (sorts first)
                std::transform(fp.begin(), fp.end(), std::back_inserter(key),
                    [](double v) { return llround(v * FINGERPRINT_SCALE); });
            }
            auto score = canonicity_score(result);
            auto it = winners.find(key);
            if (it == winners.end() || score < it->second.first) {
                winners[key] = {score, result};
            }
        };

        std::map<std::string, double> numeric;
        for (auto& [k, v] : bindings) {
            if (auto nv = evaluate(*v)) numeric[k] = nv.value();
        }

        // Fix 1: per-top-level-query dead-end set shared across sibling
        // candidates within this derive_all pass.
        DeadEndSet dead_ends;

        enumerate_candidates(target, [&](const Candidate& c) {
            if (c.condition && !check_condition(*c.condition, numeric)) return false;

            if (c.type == CandidateType::EXPR) {
                auto b = bindings;
                consider_result(try_derive(c.expr, target, b, {}, 0, dead_ends));
            } else if (c.type == CandidateType::FORMULA_REV) {
                // Unfold sub-system equations and collect all solutions
                // (sub-system load failure → no reverse solutions from this candidate)
                try {
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    std::map<std::string, ExprPtr> parent_map;
                    for (auto& [sv, expr] : c.call->bindings)
                        parent_map[sv] = expr;
                    const std::string sub_target = c.sub_var;
                    ExprPtr binding_expr = parent_map[sub_target];

                    for (const auto& eq : sub_sys.equations) {
                        if (eq.lhs_var != c.call->query_var) continue;
                        if (eq.condition && !check_condition(*eq.condition, numeric))
                            continue;
                        ExprPtr unfolded = eq.rhs;
                        for (auto& [sv, pe] : parent_map) {
                            if (sv == sub_target) continue;
                            unfolded = substitute(unfolded, sv, pe);
                        }
                        for (auto& [k, v] : sub_sys.defaults) {
                            if (parent_map.count(k) || k == c.call->query_var || k == sub_target) continue;
                            unfolded = substitute(unfolded, k, Expr::Num(v));
                        }
                        unfolded = simplify(unfolded);
                        auto sols = solve_for_all(Expr::Var(c.call->output_var), unfolded, sub_target);
                        for (auto& sol : sols) {
                            if (!sol.expr) continue;
                            if (is_var(binding_expr) && binding_expr->name == target) {
                                auto b = bindings;
                                consider_result(try_derive(sol.expr, target, b, {}, 0, dead_ends));
                            } else {
                                auto final_sols = solve_for_all(sol.expr, binding_expr, target);
                                for (auto& fs : final_sols) {
                                    if (!fs.expr) continue;
                                    auto b = bindings;
                                    consider_result(try_derive(fs.expr, target, b, {}, 0, dead_ends));
                                }
                            }
                        }
                    }
                // NOLINTNEXTLINE(bugprone-empty-catch) — sub-system load failure → no reverse solutions
                } catch (const std::runtime_error&) {}
            } else if (c.type == CandidateType::FORMULA_FWD) {
                // Forward formula call — derive into sub-system
                auto b = bindings;
                auto result = derive_recursive(target, b, {}, 0, dead_ends);
                consider_result(result);
                return result != nullptr; // stop if found (forward is single-valued)
            }
            return false; // don't stop — collect all
        });

        // Emit winners phase: sort ascending by canonicity_score so the
        // simplest form appears first and always-NaN sentinels appear last.
        // Ordering within equal canonicity is stable (std::map is ordered by
        // fingerprint key — sentinels sort after real fingerprints because
        // their discriminator byte is 1 vs 0).
        //
        // CSE (Option C): when cse_threshold >= 1 AND out_helpers != nullptr,
        // a CSE pass runs over the sorted-and-truncated ExprPtrs BEFORE
        // formatting. Per amendment Q1, each winner is pre-canonicalized via
        // simplify(distribute_over_sum(e)) — the same transformation
        // format_derived applies internally — so structurally-equivalent
        // expressions key identically in the CSE counter. Per amendment Q3,
        // the output_cap is applied here (NOT post-format in main.cpp), so
        // helpers reflect only the printed equations.
        std::vector<std::pair<std::pair<int, int>, ExprPtr>> sorted_exprs;
        sorted_exprs.reserve(winners.size());
        for (auto& [key, sc_expr] : winners) {
            (void)key;
            sorted_exprs.push_back({sc_expr.first, sc_expr.second});
        }
        std::sort(sorted_exprs.begin(), sorted_exprs.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        // Amendment Q3: truncate to output_cap BEFORE CSE so helpers reflect
        // only the printed equations. output_cap == 0 means unbounded.
        if (output_cap > 0 && static_cast<int>(sorted_exprs.size()) > output_cap)
            sorted_exprs.resize(static_cast<size_t>(output_cap));

        // CSE pass — only if requested. N1: pre-canonicalization runs ONLY
        // when CSE is active. The no-CSE path formats `sorted_exprs` directly,
        // bypassing the simplify(distribute_over_sum(...)) sweep that exists
        // solely to make structurally-equivalent winners key identically for
        // the CSE counter.
        std::vector<ExprPtr> for_format;
        const bool cse_active = (cse_threshold >= 1 && out_helpers != nullptr);
        if (cse_active) {
            // Amendment Q1: pre-canonicalize each winner via the same path that
            // format_derived runs internally. Without this, two semantically
            // identical winners (e.g. (b+c)/2 vs b/2 + c/2) would key differently
            // for CSE counting and the duplication would be missed.
            for_format.reserve(sorted_exprs.size());
            for (auto& [score, expr] : sorted_exprs) {
                (void)score;
                for_format.push_back(simplify(distribute_over_sum(expr)));
            }

            // Amendment A1: occupied set must include all variables, section
            // args, section return_vars, target, the symbolic_bindings keys
            // and values, the numeric_bindings keys, and the builtin
            // constants pi/e/phi.
            std::set<std::string> occupied = all_variables();
            for (const auto& sec : sections_) {
                for (const auto& a : sec.positional_args) occupied.insert(a);
                if (!sec.return_var.empty()) occupied.insert(sec.return_var);
            }
            occupied.insert(target);
            for (auto& [k, v] : symbolic_bindings) {
                occupied.insert(k);
                occupied.insert(v);
            }
            for (auto& [k, v] : numeric_bindings) { (void)v; occupied.insert(k); }
            occupied.insert("pi");
            occupied.insert("e");
            occupied.insert("phi");

            auto helpers = cse_extract(for_format, cse_threshold, occupied);

            // Format each helper RHS — CSE-replace using earlier helpers so
            // nested helpers compose (D8 invariant: t2 = sin(t1), not
            // t2 = sin(x^2)).
            // justified: prefix-slice `helpers.begin() + i` for nested-helper composition
            for (size_t i = 0; i < helpers.size(); i++) {
                std::vector<std::pair<std::string, ExprPtr>> const earlier(
                    helpers.begin(), helpers.begin() + static_cast<long>(i));
                auto rhs = replace_subtree_by_name(helpers[i].second, earlier);
                out_helpers->push_back(
                    helpers[i].first + " = " + format_derived(rhs, aliases));
            }
            // Substitute helpers into each main equation.
            std::transform(for_format.begin(), for_format.end(), for_format.begin(),
                [&helpers](ExprPtr expr) { return replace_subtree_by_name(expr, helpers); });
        } else {
            // No-CSE path: format directly from sorted_exprs (zero overhead).
            for_format.reserve(sorted_exprs.size());
            for (auto& [score, expr] : sorted_exprs) {
                (void)score;
                for_format.push_back(expr);
            }
        }

        // Now format the (possibly substituted) ExprPtrs.
        std::vector<std::string> results;
        results.reserve(for_format.size());
        std::transform(for_format.begin(), for_format.end(), std::back_inserter(results),
            [this, &aliases](ExprPtr expr) { return format_derived(expr, aliases); });

        // If no equation-based results, check iff conditions for constraint inversion.
        // For piecewise functions: "result = 1 iff x > 0" → "x > 0 if result = 1"
        //
        // Fingerprinting does not apply to inverted conditions (they are
        // formatted strings, not expression trees). Scope a local `seen`
        // set here to keep condition-string dedup without reintroducing a
        // top-level dedup mechanism that would clash with the winner map.
        if (results.empty()) {
            std::set<std::string> seen;
            for (auto& eq : equations) {
                if (!eq.condition || !eq.bidirectional) continue;
                bool target_in_cond = false;
                for (auto& cl : eq.condition->clauses)
                    if (contains_var(cl.lhs, target) || contains_var(cl.rhs, target))
                        { target_in_cond = true; break; }
                if (!target_in_cond) continue;

                const Expr* rhs_val = substitute_bindings(eq.rhs, bindings, target);
                bool matches = true;
                if (auto it = bindings.find(eq.lhs_var); it != bindings.end()) {
                    auto lhs_num = evaluate(*it->second);
                    auto rhs_num = evaluate(*rhs_val);
                    if (lhs_num && rhs_num && !approx_equal(lhs_num.value(), rhs_num.value())) matches = false;
                }
                if (!matches) continue;

                const std::string cond_str = eq.condition
                    ? eq.condition->to_valueset(target, {}).to_string()
                    : std::string{};
                bool body_is_known = false;
                if (auto it = bindings.find(eq.lhs_var); it != bindings.end()) {
                    if (evaluate(*it->second)) body_is_known = true;
                }

                // Check if the inversion is iff (exclusive) or just if.
                // Start with iff, downgrade if another equation with the same LHS
                // could produce the same RHS value under a different condition.
                bool exclusive = true;
                ExprPtr this_rhs = substitute_bindings(eq.rhs, bindings, target);
                for (auto& other : equations) {
                    if (&other == &eq) continue;
                    if (other.lhs_var != eq.lhs_var) continue;
                    ExprPtr other_rhs = substitute_bindings(other.rhs, bindings, target);
                    if (expr_equal(simplify(this_rhs), simplify(other_rhs))) {
                        exclusive = false; break;
                    }
                    // Also check numeric equality
                    auto a = evaluate(*this_rhs);
                    auto b = evaluate(*other_rhs);
                    if (a && b && approx_equal(a.value(), b.value())) { exclusive = false; break; }
                }

                const std::string link = exclusive ? " iff " : " if ";
                const std::string eq_str = eq.lhs_var + " = " + expr_to_string(rhs_val);
                const std::string inverted = body_is_known
                    ? cond_str
                    : cond_str + link + eq_str;
                if (seen.insert(inverted).second) results.push_back(inverted);
            }
        }

        if (results.empty())
            throw std::runtime_error("Cannot derive equation for '" + target + "'");
        return results;
    }

    struct FitOutput {
        std::string equation;
        double r_squared = 0;
        double max_error = 0;
        bool exact = false;
        ExprPtr expr = nullptr;
        std::vector<FitOutput> alternatives;
    };

    FitOutput fit(const std::string& target,
                  const std::map<std::string, double>& numeric_bindings,
                  const std::map<std::string, std::string>& symbolic_bindings) const {
        const ExprArena::Scope scope(arena);

        // Identify the free variable (exactly one symbolic binding expected)
        std::string free_var;
        for (auto& [k, v] : symbolic_bindings) {
            if (!free_var.empty())
                throw std::runtime_error("--fit requires exactly one symbolic variable, got multiple");
            free_var = v;
        }
        if (free_var.empty())
            throw std::runtime_error("--fit requires a symbolic variable (e.g., x=x)");

        // Find which symbolic binding key maps to the free var
        std::string bind_key;
        for (auto& [k, v] : symbolic_bindings) bind_key = k;

        // Extract bounds for the free variable
        const std::map<std::string, double>& bounds_bindings = numeric_bindings;
        auto [lo, hi] = extract_bounds(bind_key, bounds_bindings);

        // Build evaluation lambda
        auto f = [&](double x) -> double {
            try {
                auto binds = numeric_bindings;
                binds[bind_key] = x;
                return resolve(target, binds);
            } catch (const std::runtime_error&) { return std::numeric_limits<double>::quiet_NaN(); }
        };

        auto samples = sample_function(f, lo, hi, numeric_samples);
        if (samples.size() < 3)
            throw std::runtime_error("Not enough valid samples for fitting (got "
                + std::to_string(samples.size()) + ")");

        auto fits = fit_all(samples, free_var, defaults, 0.9, fit_depth);
        if (fits.empty())
            throw std::runtime_error("No fit found with R² > 0.9");

        FitOutput out;
        out.equation = expr_to_string(fits[0].expr);
        out.r_squared = fits[0].r_squared;
        out.max_error = fits[0].max_error;
        out.exact = fits[0].exact;
        out.expr = fits[0].expr;

        // Include alternative fits
        // justified: starts at index 1; fits[0] already consumed into `out` above
        for (size_t i = 1; i < fits.size(); i++) {
            FitOutput alt;
            alt.equation = expr_to_string(fits[i].expr);
            alt.r_squared = fits[i].r_squared;
            alt.max_error = fits[i].max_error;
            alt.exact = fits[i].exact;
            alt.expr = fits[i].expr;
            out.alternatives.push_back(alt);
        }
        return out;
    }

    // ────────────── Subsection: Resolution / solving ──────────────

    // Build a function inverter that resolves via .fw sub-system definitions.
    // Given f(inner) = rhs, loads f's sub-system and solves for the input variable.
    [[nodiscard]] FuncInverter make_func_inverter() const {
        return [this](const std::string& func_name, const ExprPtr& rhs) -> std::vector<ExprPtr> {
            std::vector<ExprPtr> branches;
            // gen-5 cycle 3g RECOVERY: re-entrance guard for self-referential
            // function sections. The cycle-3g M1 self_name_ short-circuit in
            // load_sub_system exposes a `solve_by_inversion → inverter lambda
            //   → solve_for → solve_for_all → solve_by_inversion` cycle when
            // the section body itself contains the same FUNC_CALL — depth
            // resets to 0 across the solve_for_all boundary, defeating the
            // `depth > 20` guard in solve_by_inversion. Mirrors the existing
            // guard pattern (evaluating_predicates_ at cycle 3b USER_PREDICATE
            // dispatch; currently_loading at cycle 2 cross-file resolution
            // cycle detection). Fail-safe contract: re-entrant invocation
            // returns empty branches → algebraic solver gives up cleanly.
            static thread_local std::set<std::string> currently_inverting;
            if (currently_inverting.count(func_name)) return branches;
            currently_inverting.insert(func_name);
            struct InverterGuard {
                std::set<std::string>& s; const std::string& n;
                ~InverterGuard() { s.erase(n); }
            } const _ig{currently_inverting, func_name};
            try {
                auto& sub = load_sub_system(func_name);
                // Find the section with positional args (the function definition)
                for (const auto& sec : sub.sections_) {
                    if (sec.positional_args.empty()) continue;
                    // The input variable is the first positional arg
                    // The return variable is sec.return_var (or "result")
                    const std::string input_var = sec.positional_args[0];
                    const std::string return_var = sec.return_var.empty() ? "result" : sec.return_var;
                    // Collect ALL inverse equations (multiple inverses for sin/cos
                    // give the second principal-cycle branch — Future #12).
                    for (auto& eq : sub.equations) {
                        if (eq.lhs_var == input_var)
                            branches.push_back(simplify(substitute(eq.rhs, return_var, rhs)));
                    }
                    // Fall back to algebraic inversion if no explicit inverse equations
                    if (branches.empty()) {
                        for (auto& eq : sub.equations) {
                            if (eq.lhs_var == return_var && contains_var(eq.rhs, input_var)) {
                                auto result = solve_for(Expr::Var(return_var), eq.rhs, input_var);
                                if (result) {
                                    branches.push_back(simplify(substitute(result, return_var, rhs)));
                                    break;
                                }
                            }
                        }
                    }
                    break;  // only check first section with positional args
                }
            // NOLINTNEXTLINE(bugprone-empty-catch) — sub-system load or solve failure → no inverse available
            } catch (const std::runtime_error&) {}
            return branches;
        };
    }

    // RAII guard for function inverter thread-local
    struct FuncInverterGuard {
        explicit FuncInverterGuard(FuncInverter fn) { solve_set_func_inverter(std::move(fn)); }
        ~FuncInverterGuard() { solve_set_func_inverter(nullptr); }
    };

    // RAII guard for ExistenceChecker thread-local (gen-5 cycle 3d). Mirrors
    // FuncInverterGuard above; installed at every SimplifyContext construction
    // site (derive_all, resolve, resolve_all) so check_condition's
    // FUNCTION_SECTION dispatch arm finds the callback.
    //
    // Restore-not-clear semantics: the destructor restores the PREVIOUS
    // checker rather than clearing to empty. Critical for the nested case —
    // when an outer is_in clause invokes the checker, which calls sub.resolve,
    // which installs its OWN guard, the outer guard would otherwise be wiped
    // out on the inner guard's exit, breaking subsequent clauses in the same
    // outer condition. (FuncInverterGuard does NOT have this problem because
    // its nullptr-clear is symmetric — the outer caller never reads the
    // inverter mid-flight. The ExistenceChecker IS read mid-flight by
    // sibling AND-connected clauses.)
    struct ExistenceCheckerGuard {
        ExistenceChecker prev;
        explicit ExistenceCheckerGuard(ExistenceChecker fn)
            : prev(solve_existence_checker_()) {
            solve_set_existence_checker(std::move(fn));
        }
        ~ExistenceCheckerGuard() { solve_set_existence_checker(std::move(prev)); }
    };

    // ------------------------------------------------------------------------
    // ExistenceChecker callback body (gen-5 cycle 3d). Wraps the reverse-solve
    // primitive that backs `is_in(x, sec)` for FUNCTION_SECTION sets:
    //   `is_in(x, sec)` ↔ `∃ n: sec(n) = x`
    // Implementation: bind the section's return variable to `value`, resolve
    // for the parameter. Any non-throwing resolve indicates existence. All
    // budget/cycle/runtime errors caught → fail-safe false.
    //
    // Public (not lambda-inlined) so tests can drive it directly through an
    // ExistenceCheckerGuard. The 3 solver-entry sites construct guards with
    // this method as the callback body (`[this](...){return this->...;}`).
    // ------------------------------------------------------------------------
    [[nodiscard]] bool exists_for_function_section(const std::string& set_name, double value) const {
        auto sdef_it = set_definitions_.find(set_name);
        if (sdef_it == set_definitions_.end()) return false;
        const SetDef& sd = sdef_it->second;
        if (sd.kind != SetDef::Kind::FUNCTION_SECTION) return false;
        if (sd.parameter.empty() || sd.function_section_name.empty()) return false;
        try {
            auto& sub = load_sub_system(set_name);
            std::map<std::string, double> sub_binds;
            sub_binds[sd.function_section_name] = value;
            (void)sub.resolve(sd.parameter, sub_binds);
            return true;
        // NOLINTNEXTLINE(bugprone-empty-catch) — solve/budget/cycle failure → no such n exists (fail-safe false)
        } catch (const std::runtime_error&) { return false; }
          // Sibling exceptions (NOT runtime_error subclasses) — also fail-safe
          // false for membership probe context. The siblings normally surface
          // to top-level callers; here, exists_for_function_section IS the
          // fail-safe layer for is_in dispatch, so we swallow them too.
          // Required by Final Design D6b (cycle 3d reviewer-flagged 2026-05-16).
          catch (const SolveBudgetExceededError&) { return false; }
          catch (const CrossFileResolutionCycleError&) { return false; }
    }

    [[nodiscard]] double resolve(const std::string& target,
                   std::map<std::string, double> bindings) const {
        const ExprArena::Scope scope(arena);
        const BudgetGuard budget_guard; // Part C: initialize budget at top-level entry
        solved_symbolic_.clear(); // provenance carrier: per-query lifetime
        populate_aliases_(); // for fmt_trace fallback
        // gen-3 cycle 2 (2026-05-14): dotted-target dispatch for dim-section
        // lookups (`mass.kg`). Splits `file.section` at the first dot and
        // routes through load_sub_system → @def: cache key (set by
        // register_dim_section). Only kicks in when the dot-split file_part
        // is a registered dim section AND there's no literal equation/default
        // with the dotted name; otherwise the standard path runs.
        if (const auto dot = target.find('.'); dot != std::string::npos
                && !defaults.count(target)
                && std::none_of(equations.begin(), equations.end(),
                    [&target](const Equation& eq){ return eq.lhs_var == target; })) {
            const std::string file_part = target.substr(0, dot);
            const std::string sub_var   = target.substr(dot + 1);
            if (custom_function_defs_.count(file_part)) {
                auto& sub = load_sub_system(file_part);
                return sub.resolve(sub_var, bindings);
            }
        }
        auto prepared = prepare_bindings(target, bindings);
        const SimplifyContext simplify_ctx{&type_map_, &set_definitions_};
        const RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &prepared, &custom_functions_, &simplify_ctx);
        const FuncInverterGuard fi_guard(make_func_inverter());
        const ExistenceCheckerGuard ec_guard(  // cycle 3d: wires FUNCTION_SECTION dispatch
            [this](const std::string& set_name, double v) -> bool {
                return this->exists_for_function_section(set_name, v);
            });
        if (auto it = prepared.find(target); it != prepared.end()) return it->second;
        DeadEndSet dead_ends; // Part A: per-top-level-query dead-end set
        std::set<std::string> v0;
        return solve_recursive(target, prepared, v0, 0, dead_ends);
    }

    [[nodiscard]] ValueSet resolve_all(const std::string& target,
                          std::map<std::string, double> bindings) const {
        const ExprArena::Scope scope(arena);
        const BudgetGuard budget_guard; // Part C: initialize budget at top-level entry
        solved_symbolic_.clear(); // provenance carrier: per-query lifetime
        populate_aliases_(); // for fmt_trace fallback
        auto prepared = prepare_bindings(target, bindings);
        const SimplifyContext simplify_ctx{&type_map_, &set_definitions_};
        const RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &prepared, &custom_functions_, &simplify_ctx);
        const FuncInverterGuard fi_guard(make_func_inverter());
        const ExistenceCheckerGuard ec_guard(  // cycle 3d: wires FUNCTION_SECTION dispatch
            [this](const std::string& set_name, double v) -> bool {
                return this->exists_for_function_section(set_name, v);
            });
        if (auto it = prepared.find(target); it != prepared.end())
            return ValueSet::eq(it->second);

        // Try solving for exact values
        std::vector<double> exact_results;
        try {
            DeadEndSet dead_ends; // Part A: per-top-level-query dead-end set
            exact_results = solve_all(target, prepared, {}, 0, dead_ends);

            // Cross-equation validation: verify each candidate against ALL equations
            // For each equation, substitute all known values + candidate,
            // then check LHS == evaluated RHS
            // Only cross-validate when there are multiple equations with known LHS values
            // (single-equation multiple roots are already valid by construction)
            const int known_lhs_count = static_cast<int>(std::count_if(
                equations.begin(), equations.end(),
                [&prepared](const Equation& eq) { return prepared.count(eq.lhs_var) > 0; }));
            if (exact_results.size() > 1 && known_lhs_count > 1) {
                std::vector<double> validated;
                for (const double r : exact_results) {
                    auto test = prepared;
                    test[target] = r;
                    bool valid = true;
                    for (auto& eq : equations) {
                        // Need LHS value in bindings to compare against
                        auto lhs_it = test.find(eq.lhs_var);
                        if (lhs_it == test.end()) continue;
                        if (eq.condition && !check_condition(*eq.condition, test))
                            continue;
                        // Evaluate this equation's RHS with all known bindings
                        if (auto computed = evaluate(*simplify(
                                substitute_bindings(eq.rhs, test)))) {
                            if (!std::isfinite(computed.value())) continue;
                            if (!approx_equal(computed.value(), lhs_it->second)) {
                                valid = false; break;
                            }
                        }
                    }
                    if (valid) validated.push_back(r);
                }
                exact_results = validated; // may be empty — all rejected
            }

            if (numeric_mode && numeric_results_.count(target)) {
                const bool all_exact = std::all_of(exact_results.begin(), exact_results.end(),
                    [](double r) { return std::abs(r - std::round(r)) <= EPSILON_ZERO; });
                numeric_results_[target] = all_exact;
            }
        // NOLINTNEXTLINE(bugprone-empty-catch) — solve_all failure → no exact results; fall through to constraints
        } catch (const std::runtime_error&) {}

        // Collect constraints from iff conditions (range-valued results).
        // Ranges from iff branches may contribute even when algebraic results exist.
        ValueSet constraints = ValueSet::all();
        bool has_iff_constraints = false;
        for (auto& eq : equations) {
            if (eq.lhs_var == target && eq.condition)
                constraints = constraints.intersect(eq.condition->to_valueset(target, prepared));

            if (eq.condition && eq.bidirectional && eq.lhs_var != target
                && !contains_var(eq.rhs, target)) {
                has_iff_constraints = true;
                // Check if this equation's body is satisfied
                if (auto it = prepared.find(eq.lhs_var); it != prepared.end()) {
                    if (auto rhs_val = evaluate(*substitute_bindings(eq.rhs, prepared, target))) {
                        if (approx_equal(it->second, rhs_val.value())) {
                            // Equation body matches — condition constrains target
                            auto cond_vs = eq.condition->to_valueset(target, prepared);
                            constraints = constraints.intersect(cond_vs);
                        }
                    }
                }
            }
        }
        // not std::accumulate: ValueSet::intersect is non-trivial (allocates) and the
        // accumulator pattern is clearer in loop form here
        for (const auto& gc : global_conditions)
            // cppcheck-suppress useStlAlgorithm
            constraints = constraints.intersect(gc.to_valueset(target, prepared));

        const bool has_constraints = !constraints.empty()
            && constraints.to_string() != ValueSet::all().to_string();

        // Combine results: only unite algebraic + ranges when iff constraints contributed
        if (!exact_results.empty() && has_iff_constraints && has_constraints) {
            auto combined = constraints;
            // not std::accumulate: ValueSet::unite is non-trivial; accumulator pattern is clearer in loop form
            for (const double r : exact_results)
                // cppcheck-suppress useStlAlgorithm
                combined = combined.unite(ValueSet::eq(r));
            return combined;
        }
        if (!exact_results.empty()) {
            // Periodicity Detection (Future #12): when target's source equation
            // is a single periodic builtin call (sin/cos/tan), wrap each root
            // as a PeriodicFamily with the symbolic period.
            const std::string trig_name = detect_trig_origin(target, equations);
            if (!trig_name.empty()) {
                if (ExprPtr period = trig_period(trig_name)) {
                    std::vector<PeriodicFamily> fams;
                    fams.reserve(exact_results.size());
                    std::transform(exact_results.begin(), exact_results.end(),
                                   std::back_inserter(fams),
                                   [period](double r) { return PeriodicFamily{r, period}; });
                    return ValueSet::periodic(std::move(fams));
                }
            }
            return ValueSet::discrete(exact_results);
        }
        if (has_constraints)
            return constraints;

        throw std::runtime_error("Cannot solve for '" + target + "'");
    }

    [[nodiscard]] double resolve_one(const std::string& target,
                        std::map<std::string, double> bindings) const {
        auto result = resolve_all(target, std::move(bindings));
        auto& disc = result.discrete();
        if (disc.empty())
            throw std::runtime_error("Cannot solve for '" + target + "': result is a range " + result.to_string());
        if (disc.size() > 1) {
            std::string vals;
            for (auto r : disc) vals += (vals.empty() ? "" : ", ") + fmt_num(r);
            throw std::runtime_error("Multiple solutions for '" + target + "': " + vals);
        }
        return disc[0];
    }

private:
    // ────────────── Subsection: private solver ──────────────

    // Unified trace-render helper. Trace sites call this with a double `v`
    // (the numeric value being shown) and EITHER a direct symbolic source
    // (`sym`) OR a key into `solved_symbolic_`. Resolution order:
    //   1. --approximate mode → fmt_num(v) (user opted out of recognition).
    //   2. `sym` provided → expr_to_string(sym) (already-recognized form
    //      stored at write-time by T10 / T7).
    //   3. `key` provided → look up solved_symbolic_[key], render if found.
    //   4. fall back to fmt_exact_double(v, aliases_) — covers defaults /
    //      givens / @extern returns where no symbolic source exists.
    [[nodiscard]] std::string fmt_trace(double v, const Expr* sym = nullptr,
                          const std::string& key = "") const {
        if (approximate_mode) return fmt_num(v);
        if (!sym && !key.empty()) {
            auto it = solved_symbolic_.find(key);
            if (it != solved_symbolic_.end()) sym = it->second;
        }
        if (sym) return expr_to_string(sym);
        return fmt_exact_double(v, aliases_);
    }

    [[nodiscard]] std::map<std::string, double> prepare_bindings(const std::string& target,
                                                    std::map<std::string, double>& bindings) const {
        trace.step("\nsolving for: " + target);
        for (auto& [k, v] : defaults) {
            if (k != target && !bindings.count(k)) {
                bindings[k] = v;
                trace.step("  using default: " + k + " = " + fmt_trace(v));
            }
        }
        if (trace.show_steps() && !bindings.empty()) {
            trace.step("  given:");
            for (auto& [k, v] : bindings)
                trace.step("    " + k + " = " + fmt_trace(v));
        }
        return bindings;
    }

    // Like solve_recursive but collects ALL valid results instead of stopping at first
    [[nodiscard]] std::vector<double> solve_all(const std::string& target,
                                   std::map<std::string, double>& bindings,
                                   std::set<std::string> visited, int depth,
                                   DeadEndSet& dead_ends) const {
        if (auto it = bindings.find(target); it != bindings.end()) {
            return {it->second};
        }
        if (visited.count(target)) {
            return {};
        }
        visited.insert(target);

        std::vector<double> results;
        bool had_nan_inf = false;
        std::set<std::string> missing;

        auto try_expr_all = [&](const ExprPtr& expr,
                                [[maybe_unused]] const std::string& label,
                                const Condition* cond) {
            auto b = bindings; // copy — each attempt gets fresh bindings
            bool nan_inf = false;
            if (try_resolve(expr, target, b, visited, depth, nan_inf, missing, dead_ends)) {
                const double val = b.at(target);
                // Check equation condition
                if (cond && !check_condition(*cond, b)) return;
                // Check global conditions
                if (std::any_of(global_conditions.begin(), global_conditions.end(),
                        [&b](const Condition& gc) { return !check_condition(gc, b); }))
                    return;
                // Deduplicate
                if (std::any_of(results.begin(), results.end(),
                        [val](double r) { return std::abs(r - val) < EPSILON_ZERO; }))
                    return;
                results.push_back(val);
            }
            if (nan_inf) had_nan_inf = true;
        };

        // Part B: first-successful-EXPR-source policy. Candidates from the same
        // source equation share a source_group id (Strategy 2's multi-root from
        // quadratic formula all fall in the same group). Once a source group has
        // produced >=1 finite result, subsequent EXPR candidates from a DIFFERENT
        // group are skipped. NUMERIC candidates (Strategy 6) still fire subject
        // to their own gate below for single-variable equations.
        int winning_expr_group = -1;
        enumerate_candidates(target, [&](const Candidate& c) {
            enforce_solve_budget(); // Part C: insurance — per-candidate-evaluation
            if (c.type == CandidateType::EXPR) {
                if (winning_expr_group >= 0 && c.source_group != winning_expr_group)
                    return true; // moved to a new source group — stop enumeration
                // Check pre-condition
                if (c.condition && !check_condition(*c.condition, bindings)) return false;
                const size_t before = results.size();
                try_expr_all(c.expr, c.desc, c.condition);
                // cppcheck-suppress knownConditionTrueFalse
                // try_expr_all mutates results via a captured-by-reference lambda
                // closure (see try_expr_all definition earlier in this method);
                // cppcheck cannot trace that and constant-folds the comparison.
                if (results.size() > before && winning_expr_group < 0)
                    winning_expr_group = c.source_group;
            } else if (c.type == CandidateType::FORMULA_FWD) {
                if (formula_depth_ >= max_formula_depth) return false;
                try {
                    formula_depth_++;
                    struct DepthGuard { ~DepthGuard() { formula_depth_--; } } const guard;
                    auto sub_binds = prepare_sub_bindings(*c.call, bindings, visited, depth,
                                                          "", true, &dead_ends);
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    sub_sys.max_formula_depth = max_formula_depth;
                    const double val = sub_sys.resolve(c.call->query_var, sub_binds);
                    if (!std::isnan(val) && !std::isinf(val)) {
                        if (std::any_of(results.begin(), results.end(),
                                [val](double r) { return std::abs(r - val) < EPSILON_ZERO; }))
                            return false;
                        // Check global conditions
                        auto b = bindings; b[target] = val;
                        if (std::any_of(global_conditions.begin(), global_conditions.end(),
                                [&b](const Condition& gc) { return !check_condition(gc, b); }))
                            return false;
                        results.push_back(val);
                    }
                // NOLINTNEXTLINE(bugprone-empty-catch) — sub-system resolve failure → no result from this candidate
                } catch (const std::runtime_error&) {}
            } else if (c.type == CandidateType::NUMERIC) {
                // Skip multi-variable NUMERIC candidates unconditionally. The
                // system-probe fallback is expensive and rarely helpful when
                // multiple variables are still free; single-variable NUMERIC
                // (cvars empty after erasures) still fires for transcendental
                // fallback (e.g., x + sin(x) = 1) and as the under-constrained
                // fast-fail gate (no single-variable candidate → no results →
                // clean "Cannot solve" exit 1 instead of a budget breach).
                std::set<std::string> cvars;
                collect_vars(c.expr, cvars);
                cvars.erase(target);
                for (auto& [k, v] : bindings) cvars.erase(k);
                for (auto& [k, v] : builtin_constants()) cvars.erase(k);
                // Query-alias placeholders (e.g. `?prev` in factorial(result=?prev, ...))
                // are synthesized as Var nodes by the parser but aren't true free
                // variables — they're bound by formula-call resolution. Exclude
                // them from the residual.
                for (auto& fc : formula_calls) cvars.erase(fc.output_var);
                if (!cvars.empty()) return false; // multi-variable → skip
                // Cap numeric contributions to prevent explosion with trig equations
                constexpr size_t MAX_NUMERIC_RESULTS = 50;
                if (results.size() >= MAX_NUMERIC_RESULTS) return false;
                auto roots = try_resolve_numeric(c.expr, target, bindings,
                    visited, depth, c.condition, dead_ends);
                for (const double val : roots) {
                    if (results.size() >= MAX_NUMERIC_RESULTS) break;
                    const bool dup = std::any_of(results.begin(), results.end(),
                        [val](double r) { return approx_equal(r, val); });
                    if (!dup) {
                        if (!numeric_results_.count(target))
                            numeric_results_[target] = false;
                        results.push_back(val);
                    }
                }
            }
            // FORMULA_REV handled similarly but less common for multi-return
            return false; // never stop — collect ALL results
        }, &bindings);

        if (results.empty() && !missing.empty()) {
            // Part A: record dead-end — target unreachable from current bindings.
            dead_ends.insert({target, bindings_keyset(bindings)});
            std::string list;
            for (const auto& v : missing) list += (list.empty() ? "" : ", ") + ("'" + v + "'");
            throw std::runtime_error("Cannot solve for '" + target + "': no value for " + list);
        }
        if (results.empty()) {
            dead_ends.insert({target, bindings_keyset(bindings)});
            throw std::runtime_error("Cannot solve for '" + target + "'");
        }

        return results;
    }
    static void strip_bom(std::string& line) {
        if (line.size() >= 3
            && (unsigned char)line[0] == 0xEF
            && (unsigned char)line[1] == 0xBB
            && (unsigned char)line[2] == 0xBF)
            line = line.substr(3);
    }

    // --- Formula call extraction ---
    // These helpers are public so that the free function `parse_cli_query`
    // (Future #21, nested form) can reuse the same token-level primitive
    // `.fw`-file equation parsing uses (see `parse_line` at the call site
    // around system.h:2340). They are pure functions over tokens — no
    // instance state — so exposing them does not widen the class API
    // surface in any meaningful sense.
public:
    [[nodiscard]] static size_t find_matching_rparen(const std::vector<Token>& tok, size_t lparen_pos) {
        int depth = 1;
        // justified: token-cursor — returns the matching offset
        for (size_t i = lparen_pos + 1; i < tok.size(); i++) {
            if (tok[i].type == TokenType::LPAREN) depth++;
            else if (tok[i].type == TokenType::RPAREN) { if (--depth == 0) return i; }
        }
        return std::string::npos;
    }

    [[nodiscard]] static bool has_question_in_range(const std::vector<Token>& tok, size_t from, size_t to) {
        return std::any_of(tok.begin() + static_cast<long>(from),
                           tok.begin() + static_cast<long>(to),
                           [](const Token& t) { return t.type == TokenType::QUESTION; });
    }

    [[nodiscard]] static FormulaCall parse_call_args(const std::vector<Token>& tok, size_t name_pos, size_t rparen_pos) {
        FormulaCall call;
        call.file_stem = tok[name_pos].text;

        // Parse comma-separated args between LPAREN and RPAREN
        size_t i = name_pos + 2; // skip IDENT and LPAREN
        while (i < rparen_pos) {
            // Skip commas
            if (tok[i].type == TokenType::COMMA) { i++; continue; }

            // gen-5 cycle 3f: accept IN as parameter-name token in binding
            // position. `in` is reserved in expression context (cycle 3f D2)
            // but ALLOWED as a parameter name here — formula-call bindings
            // are name=expr positions, not expressions. Python's `class`/`def`
            // precedent: reserved words can appear as keyword-arg names in
            // some call surfaces. This site, line ~+8 (alias-after-?), and
            // extract_formula_calls' implied-alias guard form the 3-site set.
            if (tok[i].type != TokenType::IDENT
                && tok[i].type != TokenType::IN) { i++; continue; }

            // Check for query: IDENT EQUALS QUESTION [IDENT]
            if (i + 2 < rparen_pos
                && tok[i + 1].type == TokenType::EQUALS
                && tok[i + 2].type == TokenType::QUESTION) {
                call.query_var = tok[i].text;
                // Check for alias after ?
                if (i + 3 < rparen_pos
                    && (tok[i + 3].type == TokenType::IDENT
                        || tok[i + 3].type == TokenType::IN)) {
                    call.output_var = tok[i + 3].text;
                    i += 4;
                } else {
                    call.output_var = call.query_var;
                    i += 3;
                }
            }
            // Check for binding: IDENT EQUALS expr (up to next COMMA or RPAREN)
            else if (i + 1 < rparen_pos && tok[i + 1].type == TokenType::EQUALS) {
                const std::string sub_var = tok[i].text;
                // Collect tokens from after = until COMMA or RPAREN
                const size_t expr_start = i + 2;
                size_t expr_end = expr_start;
                int pd = 0;
                while (expr_end < rparen_pos) {
                    if (tok[expr_end].type == TokenType::LPAREN) pd++;
                    else if (tok[expr_end].type == TokenType::RPAREN) pd--;
                    // M3 vec/mat literals embed COMMAs inside [...]; track
                    // bracket depth alongside paren depth so a binding like
                    // `f(v=[1, 2, 3], result=?)` does not truncate at the
                    // first inner comma. Reviewer Cycle B 2026-05-10.
                    else if (tok[expr_end].type == TokenType::LBRACKET) pd++;
                    else if (tok[expr_end].type == TokenType::RBRACKET) pd--;
                    else if (tok[expr_end].type == TokenType::COMMA && pd == 0) break;
                    expr_end++;
                }
                std::vector<Token> expr_tok(
                    tok.begin() + static_cast<std::ptrdiff_t>(expr_start),
                    tok.begin() + static_cast<std::ptrdiff_t>(expr_end));
                expr_tok.push_back({TokenType::END, "", 0});
                call.bindings[sub_var] = Parser(expr_tok).parse_expr();
                i = expr_end;
            }
            // Shorthand binding: bare IDENT → Var with same name
            else {
                call.bindings[tok[i].text] = Expr::Var(tok[i].text);
                i++;
            }
        }

        // gen-5 cycle 3i (Fix Y, closes Future #91): no-`?` call sites are now
        // legal — extract_formula_calls' unified outer loop dispatches the
        // named-arg-only flavor (`func(a=expr, b=expr)` in arithmetic context)
        // separately. parse_call_args returns the populated bindings; the
        // caller assigns a synthetic output_var and looks up the sub's
        // return_var for the query_var. Hard throw on empty query_var was
        // structurally a duplication smell — the caller now handles both
        // flavors. See extract_formula_calls' main loop below.
        return call;
    }

    // Scan tok[from, to) for EQUALS at paren/bracket depth == 0 relative to
    // `from`. Bracket-symmetric (LBRACKET/RBRACKET tracked alongside paren) so
    // a binding like `f(v=[1, 2, 3])` does not see the `[` as opening a depth
    // level that would mask its inner contents. Mirrors the depth-tracking
    // pattern used by parse_call_args' binding sub-loop (system.h:2791-2798).
    [[nodiscard]] static bool has_named_eq_in_range(const std::vector<Token>& tok, size_t from, size_t to) {
        int pd = 0;
        // justified: token-cursor with paren-depth state — std::any_of would not carry the depth across iterations
        for (size_t k = from; k < to; k++) {
            // cppcheck-suppress useStlAlgorithm
            if (tok[k].type == TokenType::LPAREN) pd++;
            else if (tok[k].type == TokenType::RPAREN) pd--;
            else if (tok[k].type == TokenType::LBRACKET) pd++;
            else if (tok[k].type == TokenType::RBRACKET) pd--;
            else if (tok[k].type == TokenType::EQUALS && pd == 0) return true;
        }
        return false;
    }

    // Try to extract a named-arg-only formula call at tok[name_pos..rparen_pos].
    // Returns std::nullopt to signal "fall through — let the parser handle the
    // tokens as written"; returns the constructed FormulaCall otherwise. Hoisted
    // out of `extract_formula_calls`'s main loop to keep that loop's stack frame
    // small — the named-arg branch allocates a try/catch + std::string locals
    // that would otherwise live in the loop's frame even on the common non-entry
    // path. parse_line is called once per equation but the binary's TEST mode
    // runs ~thousands of equations through this routine; trimming the loop frame
    // matters under -fsanitize=address where each frame inflates 4×.
    [[nodiscard]] static std::optional<FormulaCall>
    try_extract_named_call(const std::vector<Token>& tok, size_t name_pos, size_t rparen_pos,
                           FormulaSystem& self) {
        const std::string& name = tok[name_pos].text;
        if (builtin_functions().count(name) || self.custom_functions_.count(name))
            return std::nullopt;

        std::string return_var;
        try {
            auto& sub = self.load_sub_system(name);
            for (const auto& sec : sub.sections_) {
                // cppcheck-suppress useStlAlgorithm
                if (!sec.return_var.empty()) { return_var = sec.return_var; break; }
            }
        } catch (const std::runtime_error&) {
            return std::nullopt;
        }
        if (return_var.empty()) return std::nullopt;

        // gen-5 cycle 3i (reviewer Issue 1): parse_call_args can throw via Parser
        // on lexer-ambiguous shapes like `f(a==0)` where the leading `=` of `==`
        // is consumed as a binding separator and the inner Parser sees an
        // unexpected `=`. Treat any parse failure as a fall-through (returns
        // nullopt so the outer loop pushes tokens verbatim and the parser will
        // throw with Fix W's enhanced warning). Mirrors the load_sub_system catch
        // above — `try_extract_named_call`'s contract is "nullopt on any reason
        // to fall through; never throw."
        FormulaCall call;
        try {
            call = parse_call_args(tok, name_pos, rparen_pos);
        } catch (const std::runtime_error&) {
            return std::nullopt;
        }
        call.query_var = return_var;
        call.output_var = "_fc" + std::to_string(self.next_call_id_++);
        return call;
    }

    static std::pair<std::vector<Token>, std::vector<FormulaCall>>
    extract_formula_calls(const std::vector<Token>& tok, FormulaSystem* self = nullptr) {
        // Quick check: any QUESTION inside parens? Or (gen-5 cycle 3i, Fix Y) any
        // EQUALS at paren-depth 1 when we have a `self` to dispatch the named-arg
        // branch through. CLI path (`self == nullptr`) keeps the `?`-only contract.
        int paren_depth = 0;
        bool has_call = false;
        for (const auto& t : tok) {
            if (t.type == TokenType::LPAREN) paren_depth++;
            else if (t.type == TokenType::RPAREN) paren_depth--;
            else if (t.type == TokenType::QUESTION && paren_depth > 0) { has_call = true; break; }
            else if (self != nullptr && t.type == TokenType::EQUALS && paren_depth > 0) { has_call = true; break; }
        }
        if (!has_call) return {tok, {}};

        std::vector<Token> result;
        std::vector<FormulaCall> calls;
        size_t i = 0;

        while (i < tok.size()) {
            if (tok[i].type == TokenType::IDENT
                && i + 1 < tok.size()
                && tok[i + 1].type == TokenType::LPAREN) {
                const size_t rparen = find_matching_rparen(tok, i + 1);
                // gen-6 Step C (Bug A guard): never extract an aggregate reducer
                // (sum/product/…) as a formula call. The `?`/`=` inside the
                // reducer's range belongs to a NESTED formula call (e.g. dmg=?
                // in sum(combat(...,dmg=?))); the inner call is extracted on a
                // later loop iteration, the reducer passes through verbatim.
                if (rparen != std::string::npos && !is_aggregate_reducer(tok[i].text)
                    && has_question_in_range(tok, i + 2, rparen)) {
                    auto call = parse_call_args(tok, i, rparen);

                    // Implied alias: if preceded by "IDENT =" and call has no explicit alias
                    // gen-5 cycle 3f: accept IN as the output-var token in
                    // the implied-alias guard (parallel to parse_call_args).
                    if (call.output_var == call.query_var
                        && result.size() >= 2
                        && result[result.size() - 1].type == TokenType::EQUALS
                        && (result[result.size() - 2].type == TokenType::IDENT
                            || result[result.size() - 2].type == TokenType::IN)) {
                        call.output_var = result[result.size() - 2].text;
                    }

                    calls.push_back(call);
                    result.push_back(Token{TokenType::IDENT, call.output_var, 0});
                    i = rparen + 1;
                    continue;
                }
                // gen-5 cycle 3i (Fix Y, closes Future #91): no-? named-arg form.
                // Dispatch via try_extract_named_call which handles
                // builtin/custom_functions_ skip, load_sub_system try/catch
                // fallback, return_var lookup, and synthetic output_var
                // generation. Falls through (push token as-is) on any rejection
                // so the parser can try its luck (and Fix W's enhanced warning
                // will name the line if the parser also fails).
                if (rparen != std::string::npos && self != nullptr
                    && !is_aggregate_reducer(tok[i].text)  // Step C Bug A guard (see above)
                    && has_named_eq_in_range(tok, i + 2, rparen)) {
                    if (auto opt_call = try_extract_named_call(tok, i, rparen, *self)) {
                        calls.push_back(*opt_call);
                        result.push_back(Token{TokenType::IDENT, opt_call->output_var, 0});
                        i = rparen + 1;
                        continue;
                    }
                }
            }
            result.push_back(tok[i]);
            i++;
        }

        return {result, calls};
    }

private:
    // ────────────── Subsection: private parsing helpers ──────────────

    // Parse a condition string like "x > 0" or "x > 0 && x < 100"
    std::optional<Condition> parse_condition(const std::string& cond_str) {
        if (cond_str.empty()) return std::nullopt;

        Condition cond;
        // Split on && and || — collect clause strings and connectors
        std::vector<std::string> clause_strs;
        std::string remaining = cond_str;
        while (!remaining.empty()) {
            const size_t and_pos = remaining.find("&&");
            const size_t or_pos = remaining.find("||");
            const size_t split = std::min(and_pos, or_pos);

            if (split == std::string::npos) {
                clause_strs.push_back(remaining);
                remaining.clear();
            } else {
                clause_strs.push_back(remaining.substr(0, split));
                cond.connectors.push_back(
                    (split == and_pos) ? CondLogic::AND : CondLogic::OR);
                remaining = remaining.substr(split + 2);
            }
        }

        for (auto& clause_str : clause_strs) {
            clause_str = trim(clause_str);
            if (clause_str.empty()) continue;

            // gen-5 cycle 3f: infix `in` operator as syntax sugar for
            // is_in(x, set). Scan for space-padded ` in ` to disambiguate
            // from `sin(x)`, `infinity`, etc. Lower to
            // FUNC_CALL("is_in", [lhs, rhs]) — identical AST to the
            // function-call form. Parse-time synthesis; AST equivalence
            // means backward compat is structural (existing tests asserting
            // on FUNC_CALL form pass unchanged).
            //
            // Edge cases (documented per visionary V3):
            //   - `x == 5 in int` → ` in ` matches first; parse_expr("x == 5")
            //     fails (parser.h has no comparison level). Fails loudly.
            //   - `x in y in z` → first ` in ` consumed; defensive check
            //     below produces clearer error than raw RHS parse failure.
            //   - `xin mass` (no space) → no IN token; lexer sees IDENT IDENT;
            //     comparison-op scan fails. Fails loudly.
            //   - `is_in(x, int)` → `find(" in ")` returns npos (no space-i-n-
            //     space substring); flows to existing predicate path. (Verified
            //     char-by-char in design proposal.)
            const size_t in_pos = clause_str.find(" in ");
            if (in_pos != std::string::npos) {
                std::string in_lhs_str = clause_str.substr(0, in_pos);
                std::string in_rhs_str = clause_str.substr(in_pos + 4); // skip " in "
                while (!in_lhs_str.empty()
                       && std::isspace(static_cast<unsigned char>(in_lhs_str.back())))
                    in_lhs_str.pop_back();
                size_t rhs_start = 0;
                while (rhs_start < in_rhs_str.size()
                       && std::isspace(static_cast<unsigned char>(in_rhs_str[rhs_start])))
                    rhs_start++;
                in_rhs_str = in_rhs_str.substr(rhs_start);
                if (in_lhs_str.empty() || in_rhs_str.empty())
                    throw std::runtime_error(
                        "Infix 'in': empty LHS or RHS in '" + clause_str + "'");
                ExprPtr in_lhs = Parser(Lexer(in_lhs_str).tokenize()).parse_expr();
                // Chained-`in` defensive check (cycle 3f critic): verify RHS
                // tokens contain no IN. Catches `x in y in z` with a clearer
                // error than the raw parser throw.
                auto in_rhs_tokens = Lexer(in_rhs_str).tokenize();
                if (std::any_of(in_rhs_tokens.begin(), in_rhs_tokens.end(),
                                [](const Token& t) { return t.type == TokenType::IN; }))
                    throw std::runtime_error(
                        "Infix 'in' does not chain: '" + clause_str
                        + "'. Use '(x in y) && (x in z)' for compound membership.");
                ExprPtr in_rhs = Parser(std::move(in_rhs_tokens)).parse_expr();
                auto func_call = Expr::Call("is_in", {in_lhs, in_rhs});
                cond.clauses.push_back({func_call, nullptr, CondOp::EQ});
                continue;
            }

            // Parse clause: expr op expr
            // Find comparison operator
            CondOp op = CondOp::EQ;
            size_t op_pos = std::string::npos;
            size_t op_len = 0;

            // Two-char operators first, then single-char
            for (auto& [s, o, l] : std::vector<std::tuple<std::string, CondOp, size_t>>{
                {"==", CondOp::EQ, 2}, {">=", CondOp::GE, 2}, {"<=", CondOp::LE, 2}, {"!=", CondOp::NE, 2},
                {">", CondOp::GT, 1}, {"<", CondOp::LT, 1}, {"=", CondOp::EQ, 1}
            }) {
                auto p = clause_str.find(s);
                if (p != std::string::npos && (op_pos == std::string::npos || p < op_pos)) {
                    // For single-char ops, skip if part of a two-char op
                    if (l == 1 && p + 1 < clause_str.size() && clause_str[p+1] == '=') continue;
                    if (l == 1 && s == "=" && p > 0 && (clause_str[p-1] == '>' || clause_str[p-1] == '<' || clause_str[p-1] == '!')) continue;
                    op_pos = p;
                    op_len = l;
                    op = o;
                }
            }

            // Predicate-clause form (Future #53 / gen-5 cycle 3a): no comparison
            // operator + parses as a FUNC_CALL whose head name is a recognised
            // predicate. Mirror `is_predicate_clause` in expr.h. Encoded as
            // `CondClause{lhs=FUNC_CALL(name,{args...}), rhs=nullptr,
            // op=CondOp::EQ}`.
            //
            // Cycle 3a D8 SIMPLIFY (2026-05-15): `is_int(n)` and
            // `is_in_dimension(n, m)` are rewritten to canonical
            // `is_in(n, int)` / `is_in(n, m)` here at parse time. The
            // dispatcher (expr.h::check_condition) and `is_predicate_clause`
            // only know `is_in` and `is_neg_num`. Cycle-2 .fw rules and tests
            // continue to use the legacy names; the rewrite preserves
            // semantics atomically for atomic-Var args.
            if (op_pos == std::string::npos) {
                auto tok = Lexer(clause_str).tokenize();
                Parser pp(tok);
                auto e = pp.parse_expr();
                if (e && e->type == ExprType::FUNC_CALL) {
                    if (e->name == "is_int" && e->args.size() == 1) {
                        // is_int(n) → is_in(n, int)
                        e->name = "is_in";
                        e->args.push_back(Expr::Var("int"));
                    } else if (e->name == "is_in_dimension" && e->args.size() == 2) {
                        // is_in_dimension(n, m) → is_in(n, m). Just rename.
                        e->name = "is_in";
                    }
                }
                if (e && e->type == ExprType::FUNC_CALL
                    && (e->name == "is_neg_num"
                        || e->name == "is_in")) {
                    cond.clauses.push_back({e, nullptr, CondOp::EQ});
                    continue;
                }
                continue; // malformed clause, skip
            }

            const std::string lhs_str = trim(clause_str.substr(0, op_pos));
            const std::string rhs_str = trim(clause_str.substr(op_pos + op_len));

            auto lhs_tok = Lexer(lhs_str).tokenize();
            auto rhs_tok = Lexer(rhs_str).tokenize();
            Parser lhs_p(lhs_tok), rhs_p(rhs_tok);

            cond.clauses.push_back({lhs_p.parse_expr(), rhs_p.parse_expr(), op});
        }

        return cond.clauses.empty() ? std::nullopt : std::optional<Condition>(cond);
    }

    void parse_line(const std::string& line) {
        // Split at condition keyword: "if", "iff", or ":" (legacy)
        // Not inside parentheses. Optional comma before if/iff.
        std::string eq_part = line;
        std::string cond_part;
        bool is_bidirectional = false;
        {
            int pd = 0;
            // justified: char-cursor with line[i+N] / line[i-1] / line.substr(0, i)
            for (size_t i = 0; i < line.size(); i++) {
                const char ch = line[i];
                if (ch == '(') { pd++; continue; }
                if (ch == ')') { pd--; continue; }
                if (pd != 0) continue;

                // "iff" keyword: followed by space or end-of-line; preceded by space or comma
                if (ch == 'i' && i + 2 < line.size()
                    && line[i+1] == 'f' && line[i+2] == 'f'
                    && (i + 3 == line.size() || line[i+3] == ' ')
                    && (i == 0 || line[i-1] == ' ' || line[i-1] == ',')) {
                    eq_part = line.substr(0, i);
                    while (!eq_part.empty() && (eq_part.back() == ' ' || eq_part.back() == ','))
                        eq_part.pop_back();
                    cond_part = (i + 3 == line.size()) ? std::string() : line.substr(i + 4);
                    is_bidirectional = true;
                    break;
                }

                // "if" keyword: followed by space or end-of-line; preceded by space or comma; not "iff"
                if (ch == 'i' && i + 1 < line.size()
                    && line[i+1] == 'f'
                    && (i + 2 == line.size() || line[i+2] == ' ')
                    && (i == 0 || line[i-1] == ' ' || line[i-1] == ',')) {
                    eq_part = line.substr(0, i);
                    while (!eq_part.empty() && (eq_part.back() == ' ' || eq_part.back() == ','))
                        eq_part.pop_back();
                    cond_part = (i + 2 == line.size()) ? std::string() : line.substr(i + 3);
                    break;
                }
            }
        }

        // Check for standalone global condition vs equation
        // An equation has "ident = expr" where = is not part of >=, <=, !=
        bool is_equation = false;
        // justified: char-cursor with eq_part[ci-1] lookahead
        for (size_t ci = 0; ci < eq_part.size(); ci++) {
            if (eq_part[ci] == '=') {
                const bool part_of_cmp = (ci > 0 && (eq_part[ci-1] == '>' || eq_part[ci-1] == '<' || eq_part[ci-1] == '!'));
                if (!part_of_cmp) { is_equation = true; break; }
            }
        }
        if (!is_equation) {
            // Global condition: "area >= 0", "side > 0"
            try {
                auto cond = parse_condition(eq_part);
                if (cond) global_conditions.push_back(std::move(*cond));
            // NOLINTNEXTLINE(bugprone-empty-catch) — malformed condition at load time → skip (best-effort parse)
            } catch (const std::runtime_error&) {}
            return;
        }

        auto tok = Lexer(eq_part).tokenize();
        if (tok.size() < 2) return;

        // Extract formula calls before expression parsing. Pass `this` so the
        // named-arg flavor (cycle 3i, Future #91 — `func(name=expr)` in
        // arithmetic position with no `?`) fires; CLI consumer at
        // parse_cli_query passes nullptr and gets `?`-only behavior.
        auto [mod_tok, calls] = extract_formula_calls(tok, this);
        // not std::transform: move-append into a different container; std::move_iterator is less readable here
        // cppcheck-suppress useStlAlgorithm
        for (auto& c : calls) formula_calls.push_back(std::move(c));

        // Standalone formula call: just "output_var END" after extraction
        if (mod_tok.size() <= 2) return;

        // Find the '=' token (not part of >=, <=, !=, ==)
        size_t eq_pos = 0;
        // justified: token-cursor — captures position of first EQUALS
        for (size_t i = 0; i < mod_tok.size(); i++) {
            if (mod_tok[i].type == TokenType::EQUALS) { eq_pos = i; break; }
        }
        if (eq_pos == 0) return;  // no '=' found

        // Annotation form (gen-3 cycle 2, 2026-05-14 / gen-5 cycle 3a,
        // 2026-05-15): `var:atom = expr` and `var:(atom1, atom2, ...) = expr`.
        // The annotation registers the binding's type in `type_map_` and
        // rewrites the LHS to a plain `var = expr` equation so the rest of
        // parse_line proceeds unchanged. ATOM-only inside parens; any
        // STAR/SLASH/CARET is a parse error per D10 grammar lock-in.
        if (mod_tok.size() >= 4
            && mod_tok[0].type == TokenType::IDENT
            && mod_tok[1].type == TokenType::COLON) {
            std::vector<std::string> atoms;
            size_t after = 0; // token index just past the annotation
            if (mod_tok[2].type == TokenType::IDENT
                && mod_tok[3].type == TokenType::EQUALS) {
                // Atomic form: var:atom = expr
                atoms.push_back(mod_tok[2].text);
                after = 3;
            } else if (mod_tok[2].type == TokenType::LPAREN) {
                // Intersection form: var:(atom1, atom2, ...) = expr.
                // Grammar lock-in (D10): IDENT/COMMA only inside parens —
                // operator tokens raise BindingAnnotationError (sibling
                // exception, propagates through load_lines per-line catch).
                size_t i = 3;
                while (i < mod_tok.size() && mod_tok[i].type != TokenType::RPAREN) {
                    if (mod_tok[i].type != TokenType::IDENT) {
                        throw BindingAnnotationError(
                            "binding annotation: only atom names allowed inside "
                            "'(...)' — got '" + mod_tok[i].text + "'");
                    }
                    atoms.push_back(mod_tok[i].text);
                    i++;
                    if (i < mod_tok.size() && mod_tok[i].type == TokenType::COMMA) i++;
                    else if (i < mod_tok.size() && mod_tok[i].type == TokenType::RPAREN) break;
                    else throw BindingAnnotationError(
                        "binding annotation: expected ',' or ')' in atom list");
                }
                if (i >= mod_tok.size() || mod_tok[i].type != TokenType::RPAREN)
                    throw BindingAnnotationError("binding annotation: missing closing ')'");
                i++;
                if (i >= mod_tok.size() || mod_tok[i].type != TokenType::EQUALS)
                    throw BindingAnnotationError("binding annotation: expected '=' after ')'");
                after = i;
            }
            if (!atoms.empty()) {
                const std::string& lhs_name = mod_tok[0].text;
                // M4 (gen-5 cycle 3a, 2026-05-15): structured classification
                // via set_definitions_. Each atom resolves to a SetDef Kind;
                // DIM_SECTION populates `.dim`, BUILTIN_PREDICATE populates
                // `.sets`. Unknown atoms raise BindingAnnotationError per D10
                // hard-error commitment. Cycle-2's `if (a == "int") continue`
                // hack retired — `int` now formally classifies as
                // BUILTIN_PREDICATE → goes into `.sets`.
                for (const auto& a : atoms) {
                    auto sit = set_definitions_.find(a);
                    if (sit == set_definitions_.end()) {
                        throw BindingAnnotationError(
                            "binding annotation: unknown set name '" + a
                            + "' — register a dim section like [" + a
                            + "], a predicate section like [" + a + "(n)] iff ..., "
                            + "a function section like [" + a + "(n) -> result] = ..., "
                            + "or use a built-in set: int, real, rational, imaginary");
                    }
                    switch (sit->second.kind) {
                        case SetDef::Kind::DIM_SECTION:
                            // Last DIM_SECTION atom wins on overwrite — multi-
                            // dim intersections are structurally ambiguous user
                            // input; cleanest semantics is "last one wins".
                            type_map_[lhs_name].dim = DimMap{{a, 1}};
                            break;
                        // BUILTIN_PREDICATE + USER_PREDICATE (cycle 3b) +
                        // FUNCTION_SECTION (cycle 3d) all share the same
                        // annotation behavior: insert atom into `.sets`. The
                        // dispatch difference lives in check_condition. Cases
                        // grouped via fall-through (closes cycle-3b R3
                        // deferral; documented at design D7).
                        case SetDef::Kind::BUILTIN_PREDICATE:
                        case SetDef::Kind::USER_PREDICATE:
                        case SetDef::Kind::FUNCTION_SECTION:
                            type_map_[lhs_name].sets.insert(a);
                            break;
                        case SetDef::Kind::COUNT_:
                            assert(false && "SetDef::Kind::COUNT_ unreachable");
                            break;
                    }
                }
                // Rewrite mod_tok so the rest of parse_line sees a plain
                // `var = expr` equation. Drop indices 1..after-1 (the
                // annotation), keep [0] (the lhs ident) + mod_tok[after..]
                // (the EQUALS and everything after).
                std::vector<Token> rebuilt;
                rebuilt.push_back(mod_tok[0]);
                rebuilt.insert(rebuilt.end(),
                    mod_tok.begin() + static_cast<std::ptrdiff_t>(after),
                    mod_tok.end());
                mod_tok = std::move(rebuilt);
                // Re-find the EQUALS position post-rewrite (it should be at 1).
                eq_pos = 1;
            }
        }

        // Simple equation: "var = expr" (IDENT followed by EQUALS)
        const bool simple_lhs = (eq_pos == 1 && mod_tok[0].type == TokenType::IDENT);

        if (!simple_lhs) {
            // Complex LHS: this is a rewrite rule (e.g., cos(-x) = cos(x))
            auto lhs_tok = std::vector<Token>(
                mod_tok.begin(),
                mod_tok.begin() + static_cast<std::ptrdiff_t>(eq_pos));
            lhs_tok.push_back(Token{TokenType::END, "", 0});
            Parser lp(lhs_tok);
            Parser rp(std::vector<Token>(
                mod_tok.begin() + static_cast<std::ptrdiff_t>(eq_pos + 1),
                mod_tok.end()));
            auto lhs_expr = lp.parse_expr();
            auto rhs_expr = rp.parse_expr();
            std::optional<Condition> cond_ast;
            bool cond_ok = true;
            if (is_bidirectional && !cond_part.empty()) {
                try { cond_ast = parse_condition(trim(cond_part)); }
                catch (const std::runtime_error& e) {
                    cond_ok = false;
                    std::cerr << "warning: dropping rewrite rule '" << eq_part
                              << "' — malformed condition: " << e.what() << '\n';
                }
            }
            if (cond_ok) {
                rewrite_rules.push_back({lhs_expr, rhs_expr, eq_part, std::move(cond_ast),
                                         is_undefined(rhs_expr), -1});
            }
            return;
        }

        const std::string& lhs = mod_tok[0].text;

        // Degenerate "x = x" from implied alias — skip
        if (mod_tok[2].type == TokenType::IDENT && mod_tok[2].text == lhs
            && mod_tok[3].type == TokenType::END)
            return;

        // Default: "x = 42" or "x = -42" (only if no condition)
        if (cond_part.empty()) {
            if (mod_tok[2].type == TokenType::NUMBER && mod_tok[3].type == TokenType::END) {
                defaults[lhs] = mod_tok[2].numval;
                return;
            }
            if (mod_tok[2].type == TokenType::MINUS
                && mod_tok[3].type == TokenType::NUMBER
                && mod_tok[4].type == TokenType::END) {
                defaults[lhs] = -mod_tok[3].numval;
                return;
            }
        }

        // Equation: parse RHS as expression
        Parser p(std::vector<Token>(mod_tok.begin() + 2, mod_tok.end()));
        std::optional<Condition> cond;
        // NOLINTNEXTLINE(bugprone-empty-catch) — malformed condition at load time → treat as unconditional
        try { cond = parse_condition(cond_part); } catch (const std::runtime_error&) {}
        equations.push_back({lhs, p.parse_expr(), std::move(cond), is_bidirectional});
    }

    // ────────────── Subsection: private solver ──────────────

    // --- Sub-system loading ---

    // ------------------------------------------------------------------------
    // Settings-propagation helper (gen-5 cycle 3h, 2026-05-16, closes Future #83).
    //
    // Mirrors the parent's solver-affecting state into a freshly constructed
    // sub-system: trace flag, numeric_mode, approximate_mode, custom_functions_
    // table, type_map_ (cycle 3a), and set_definitions_ (cycle 3a M5, extended
    // cycles 3b + 3d for USER_PREDICATE / FUNCTION_SECTION entries).
    //
    // Three call sites:
    //   1. `load_sub_system` normal path (right after `make_shared<FormulaSystem>`)
    //   2. `load_sub_system` auto-section path (re-constructs the sub when a
    //      single named section is auto-selected)
    //   3. `register_function_section` pre-cached sub (cycle 3h closes the gap
    //      where the pre-cached sub did NOT inherit parent settings — leaving
    //      `numeric_mode=false` on the sub even when the parent had it on,
    //      which suppressed Strategy 6 numeric scan during recursive
    //      FUNCTION_SECTION reverse-solve).
    //
    // USER_PREDICATE arena-lifetime invariant (cycle 3b) applies: the parent
    // owns the sub via `sub_systems` shared_ptr, so the parent arena outlives
    // any sub reference. See `load_sub_system`'s comment block for the
    // PARKED Future #87 reopen trigger.
    // ------------------------------------------------------------------------
    void copy_metadata_to_sub(FormulaSystem& sub) const {
        sub.trace = trace;
        sub.numeric_mode = numeric_mode;
        sub.approximate_mode = approximate_mode;
        sub.custom_functions_ = custom_functions_;
        sub.type_map_ = type_map_;
        sub.set_definitions_ = set_definitions_;
        sub.include_dirs = include_dirs;          // Future #80 M1: @include search path
        sub.included_files_ = included_files_;    // Future #80 M1: include allow-list
        sub.strict_includes_ = strict_includes_;  // Future #80 M2: strict-mode propagation
    }

    [[nodiscard]] const FormulaSystem& load_sub_system(const std::string& file_stem) const {
        // Split dotted names: "geometry.rectangle" → file="geometry", section="rectangle"
        std::string file_part = file_stem;
        std::string section;
        const size_t dot = file_stem.find('.');
        if (dot != std::string::npos) {
            file_part = file_stem.substr(0, dot);
            section = file_stem.substr(dot + 1);
        }

        // gen-5 cycle 3g: function-section self-reference short-circuit.
        // Recursive bodies (fibonacci(n-1) inside [fibonacci(n)->result] = ...)
        // resolve via this branch — no cache lookup, no filesystem fallback,
        // no shared_ptr cycle. Lazy timing: self_name_ is set by
        // register_function_section AFTER load_lines completes, so the body's
        // unresolved FUNC_CALLs at parse time become resolvable at solve time
        // without triggering the cycle-3d load-time stack overflow.
        // 10th location in the is_in dispatch comprehension-gate chain
        // (the 9 prior live across expr.h + system.h; see check_condition).
        if (!self_name_.empty() && file_part == self_name_) {
            return *this;
        }

        // Check custom and builtin function definitions
        auto& builtins = builtin_function_defs();
        auto blt = custom_function_defs_.find(file_part);
        const std::string* def_source = nullptr;
        if (blt != custom_function_defs_.end())
            def_source = &blt->second;
        else if (auto bit = builtins.find(file_part); bit != builtins.end())
            def_source = &bit->second;

        std::string abs_path;
        if (strict_includes_ && !def_source) {
            // Future #80 M2 (strict-includes mode): the base_dir filesystem
            // auto-probe is SKIPPED entirely. A cross-file call may resolve ONLY
            // via the @def: cache (handled above), the @include allow-list, or
            // the -I/FWIZ_PATH search path. Co-location alone is no longer
            // enough — the user must declare the dependency with @include.
            abs_path = resolve_from_included(file_part);
            if (abs_path.empty())
                abs_path = resolve_file_path(file_part, /*is_literal=*/false,
                                             /*searched=*/nullptr, /*exclude_base_dir=*/true);
            if (abs_path.empty())
                throw StrictIncludeError(build_strict_include_error(file_part));
        } else {
            // COEXIST / @def: path. base_dir probe (pre-existing behavior). The
            // canonicalized path is the primary cache key; it is used even if the
            // file does not exist (a missing file surfaces as "Cannot open file").
            std::string path = base_dir + "/" + file_part;
            if (path.find('.') == std::string::npos) path += ".fw";
            try { abs_path = std::filesystem::weakly_canonical(path).string(); }
            catch (const std::filesystem::filesystem_error&) { abs_path = path; }

            // Future #80 M1 (COEXIST): if the base_dir probe does not point at an
            // existing file, fall back to the @include search path so that a
            // formula call resolves a section file found via `-I`/FWIZ_PATH (or a
            // file previously pulled in by @include) WITHOUT requiring co-location.
            // This is additive — the base_dir path above is unchanged; only the
            // not-found case widens. Skipped for @def: cache entries (def_source).
            if (!def_source) {
                std::error_code ec;
                if (!std::filesystem::exists(abs_path, ec)) {
                    std::string search_hit = resolve_file_path(file_part, /*is_literal=*/false);
                    if (search_hit.empty())
                        search_hit = resolve_from_included(file_part);
                    if (!search_hit.empty()) abs_path = search_hit;
                }
            }
        }

        // Cache key: defined functions use name directly, files use abs path
        const std::string cache_key = def_source
            ? ("@def:" + file_part)
            : (abs_path + (section.empty() ? "" : "#" + section));
        auto it = sub_systems.find(cache_key);
        if (it != sub_systems.end()) return *it->second;

        // Future #69: cross-file resolution cycle detection. Each cross-file
        // load creates a NEW FormulaSystem with its own sub_systems cache, so
        // the cache check above never catches recursion (e.g. matmul.fw
        // containing `matmul(A, B)`). A thread-local set keyed on cache_key
        // closes that gap. The RAII guard erases on both success and
        // exception paths.
        static thread_local std::set<std::string> currently_loading;
        if (currently_loading.count(cache_key)) {
            throw CrossFileResolutionCycleError(
                "Cross-file resolution cycle: " + file_part
                + " recursively loads itself");
        }
        struct LoadGuard {
            std::set<std::string>& s;
            const std::string& k;
            ~LoadGuard() { s.erase(k); }
        };
        currently_loading.insert(cache_key);
        LoadGuard _guard{currently_loading, cache_key};

        auto sub = std::make_shared<FormulaSystem>();
        copy_metadata_to_sub(*sub);  // gen-5 cycle 3h (closes Future #83) — see helper docstring
        // Cross-file lifetime of USER_PREDICATE entries (cycle 3b):
        // USER_PREDICATE SetDef carries a Condition with ExprPtrs pointing
        // into THIS (parent) system's arena. Parent owns the sub via
        // `sub_systems` shared_ptr, so the parent arena outlives any sub
        // reference. `load_sub_system` returns `FormulaSystem&` (not
        // `shared_ptr<FormulaSystem>`) — the API does not let the caller
        // extend a sub beyond parent lifetime, so the borrow is safe.
        // PARKED Future #87: if the API ever exposes shared_ptr<FormulaSystem>
        // or sub-system cache eviction lands, USER_PREDICATE entries must
        // either be re-bound to sub's arena or deep-copied.

        // Try loading from file first; fall back to embedded definition
        if (!def_source) {
            sub->load_file(abs_path, section);
        } else {
            // Try file first (user can override definitions)
            bool loaded = false;
            try {
                std::ifstream f(abs_path);
                if (f.is_open()) {
                    sub->load_file(abs_path, section);
                    loaded = true;
                }
            // NOLINTNEXTLINE(bugprone-empty-catch) — user override file missing/malformed → fall back to builtin definition
            } catch (const std::runtime_error&) {}
            if (!loaded) {
                sub->load_string(*def_source, "@def:" + file_part);
            }
        }
        // Future #80 M2 (explicit-systems model): in strict mode a callable
        // cross-file system MUST be an explicit `[name(args) -> ret]` section.
        // A FLAT .fw file (bare equations, no header) that was @include'd merges
        // its equations but is NOT callable by stem — calling it errors with the
        // same helpful "add a header / @include" guidance. This is the second
        // layer Option C removes (the first being the base_dir auto-probe). The
        // gate is strict-mode-only and runs before the auto-select reload, so it
        // inspects the file's own declared sections. Skipped for @def: cache
        // entries (in-system / builtin sections are callable by construction)
        // and for dotted `file.section` calls (explicit section selection).
        if (strict_includes_ && !def_source && section.empty()) {
            const bool has_named_section = std::any_of(
                sub->sections_.begin(), sub->sections_.end(),
                [](const Section& s) { return !s.name.empty(); });
            if (!has_named_section)
                throw StrictIncludeError(build_strict_include_error(file_part));
        }

        // Auto-select section: if no equations loaded and file has exactly one
        // named section, load that section (common for single-function .fw files)
        if (sub->equations.empty() && section.empty()) {
            std::string auto_section;
            for (const auto& s : sub->sections_) {
                if (!s.name.empty()) {
                    if (!auto_section.empty()) { auto_section.clear(); break; } // multiple
                    auto_section = s.name;
                }
            }
            if (!auto_section.empty()) {
                sub = std::make_shared<FormulaSystem>();
                copy_metadata_to_sub(*sub);  // gen-5 cycle 3h (closes Future #83) — see helper docstring
                if (def_source)
                    sub->load_string(*def_source, "@def:" + file_part, auto_section);
                else
                    sub->load_file(abs_path, auto_section);
            }
        }
        sub_systems[cache_key] = sub;
        return *sub;
    }

    std::map<std::string, double> prepare_sub_bindings(
        const FormulaCall& call,
        std::map<std::string, double>& parent_bindings,
        std::set<std::string> visited = {}, int depth = 0,
        const std::string& skip_parent_var = "",
        bool resolve_unknowns = true,
        DeadEndSet* dead_ends = nullptr) const
    {
        // If caller didn't provide a dead-end set (verify / derive paths),
        // use a local one so we still thread a valid reference downward.
        DeadEndSet local_dead_ends;
        DeadEndSet& de = dead_ends ? *dead_ends : local_dead_ends;

        std::map<std::string, double> sub;

        // Evaluate a binding expression against parent bindings
        auto eval_binding = [&](const std::string& sub_var, ExprPtr expr) {
            // Substitute known parent bindings into the expression
            ExprPtr resolved = expr;
            std::set<std::string> vars;
            collect_vars(expr, vars);
            for (auto& v : vars) {
                if (auto it = parent_bindings.find(v); it != parent_bindings.end()) {
                    resolved = substitute(resolved, v, Expr::Num(it->second));
                } else if (resolve_unknowns) {
                    try {
                        const double val = solve_recursive(v, parent_bindings, visited, depth + 1, de);
                        resolved = substitute(resolved, v, Expr::Num(val));
                    } catch (const SolveBudgetExceededError&) { throw; }
                    catch (const std::runtime_error&) { return; }
                } else { return; }
            }
            if (auto val = evaluate(*simplify(resolved))) sub[sub_var] = val.value();
            else return;
        };

        for (auto& [sv, expr] : call.bindings) {
            // Check if we should skip this binding (for reverse formula call)
            // For simple Var bindings, check if the var name matches skip
            if (!skip_parent_var.empty() && is_var(expr) && expr->name == skip_parent_var) continue;
            eval_binding(sv, expr);
        }

        // Bridge: output_var -> query_var
        if (call.output_var != skip_parent_var) {
            if (auto it = parent_bindings.find(call.output_var); it != parent_bindings.end())
                sub[call.query_var] = it->second;
            else if (resolve_unknowns) {
                try {
                    const double val = solve_recursive(call.output_var, parent_bindings, visited, depth + 1, de);
                    sub[call.query_var] = val;
                } catch (const SolveBudgetExceededError&) { throw; }
                // NOLINTNEXTLINE(bugprone-empty-catch) — output_var unresolvable → leave unbound, sub-system may still solve
                catch (const std::runtime_error&) {}
            }
        }

        return sub;
    }

    // --- Strategy enumeration ---

    enum class CandidateType : uint8_t { EXPR, FORMULA_FWD, FORMULA_REV, NUMERIC, COUNT_ };
    struct Candidate {
        CandidateType type;
        ExprPtr expr;           // for EXPR candidates
        std::string desc;
        const FormulaCall* call;  // for formula candidates
        std::string sub_var;      // for FORMULA_REV: which sub-system var to solve
        const Condition* condition; // condition from the source equation (may be null)
        // Source-group id: candidates originating from the same source equation
        // share an id. Strategy 2's multiple roots (quadratic formula) all share
        // one id. `solve_all`'s first-successful policy stops the moment a NEW
        // source group arrives after the previous group produced >=1 result.
        int source_group = -1;
    };

    // True iff `expr` references a FormulaCall (by its output_var) whose bindings
    // mention `var`. This is how the iterator/unknown of a formula-bodied
    // aggregation hides from `contains_var(eq.rhs, target)`: after Step-C unroll,
    // `total = _fc0 + ... + _fcN` and the unknown (`def=k`) lives inside each
    // _fcN's bindings, not in the RHS tree itself. Strategy 6 consults this so a
    // reverse-solve target buried in a formula-call binding is still emitted as a
    // numeric candidate. ONE-LEVEL (non-transitive): inspects the directly-named
    // calls' bindings only; bindings that are themselves formula-call outputs are
    // not followed (nested/chained aggregation is a future step).
    [[nodiscard]] bool formula_call_bindings_contain(
            const ExprPtr& expr, const std::string& var) const {
        return std::any_of(formula_calls.begin(), formula_calls.end(),
            [&](const FormulaCall& fc) {
                return contains_var(expr, fc.output_var)
                    && std::any_of(fc.bindings.begin(), fc.bindings.end(),
                        [&](const auto& kv) { return contains_var(kv.second, var); });
            });
    }

    // Generates candidates for solving a target variable.
    // Calls handler(candidate) for each. Handler returns true to stop.
    // Optional bindings are used for Strategy 4 (equating) to substitute
    // known values before solving, preventing spurious results.
    template<typename Handler>
    void enumerate_candidates(const std::string& target, Handler&& handler,
                              const std::map<std::string, double>* sub_bindings = nullptr) const {
        int next_group = 0;

        // Strategy 1: target on LHS — direct from RHS
        for (auto& eq : equations)
            if (eq.lhs_var == target)
                if (handler(Candidate{CandidateType::EXPR, eq.rhs,
                    target + " = " + expr_to_string(eq.rhs), nullptr, "",
                    eq.condition ? &*eq.condition : nullptr, next_group++}))
                    return;

        // Strategy 2: target in RHS — algebraic inversion (may produce multiple solutions)
        for (auto& eq : equations) {
            if (!contains_var(eq.rhs, target)) continue;
            auto sols = solve_for_all(Expr::Var(eq.lhs_var), eq.rhs, target);
            const int eq_group = next_group++;  // one group per source equation
            for (auto& sol : sols)
                if (sol.expr)
                    if (handler(Candidate{CandidateType::EXPR, sol.expr,
                        target + " = " + expr_to_string(sol.expr)
                        + "  (from " + eq.lhs_var + " = " + expr_to_string(eq.rhs) + ")"
                        + (sol.cond_desc.empty() ? "" : "  [" + sol.cond_desc + "]"),
                        nullptr, "", eq.condition ? &*eq.condition : nullptr,
                        eq_group}))
                        return;
        }

        // Strategy 3: forward formula call
        for (auto& call : formula_calls)
            if (call.output_var == target)
                if (handler(Candidate{CandidateType::FORMULA_FWD, nullptr,
                    target + " via " + call.file_stem + "(" + call.query_var + "=?)",
                    &call, "", nullptr, next_group++}))
                    return;

        // Strategy 4: equate RHS of equations sharing a LHS variable
        // justified: outer of triangular pair (j = i+1) over equations
        for (size_t i = 0; i < equations.size(); i++)
            // justified: triangular pair (j = i+1) over equations
            for (size_t j = i + 1; j < equations.size(); j++) {
                if (equations[i].lhs_var != equations[j].lhs_var) continue;
                // Skip if both equations have different conditions — their domains
                // may not overlap (e.g., x>=0 and x<0 in piecewise abs)
                if (equations[i].condition && equations[j].condition) continue;
                // 12g perf guard: skip pairs where neither RHS contains the
                // target. Equating two RHS values where neither references
                // target yields no constraint on target — pure waste of an
                // O(tree) substitute + simplify + solve_for_all. Pre-existing
                // O(N^2) Strategy 4 was blowing up post-Periodicity M1 (4x
                // equation amplification on triangle --derive --cse).
                if (!contains_var(equations[i].rhs, target) &&
                    !contains_var(equations[j].rhs, target)) continue;
                // Optionally substitute known bindings to detect tautological equations
                auto maybe_sub = [&](const ExprPtr& e) -> ExprPtr {
                    if (!sub_bindings) return e;
                    ExprPtr r = e;
                    for (auto& [v, val] : *sub_bindings)
                        if (v != target) r = substitute(r, v, Expr::Num(val));
                    return simplify(r);
                };
                auto ei = maybe_sub(equations[i].rhs);
                auto ej = maybe_sub(equations[j].rhs);
                for (auto& [a, b] : {std::pair{ei, ej}, std::pair{ej, ei}}) {
                    auto sols = solve_for_all(a, b, target);
                    const int pair_group = next_group++; // one group per (i,j,direction)
                    for (auto& sol : sols) {
                        if (!sol.expr) continue;
                        // Verify: the solution must satisfy BOTH equations' conditions
                        // Pass the more restrictive condition (from equation i)
                        // The solver's condition checking will validate at solve time
                        const Condition* cond = nullptr;
                        if (equations[i].condition) cond = &*equations[i].condition;
                        else if (equations[j].condition) cond = &*equations[j].condition;
                        if (handler(Candidate{CandidateType::EXPR, sol.expr,
                            target + " = " + expr_to_string(sol.expr)
                            + "  (via " + equations[i].lhs_var + ")",
                            nullptr, "", cond, pair_group}))
                            return;
                    }
                }
            }

        // Strategy 5: reverse formula call (target appears in a binding)
        //
        // gen-5 cycle 3h (2026-05-16, Future #92): exclude the self-circular
        // case where `sub_var == target` AND the binding expression is
        // compound (not a pure `Var(target)`). The compound case — e.g.
        // `n = n-1` inside `[fibonacci(n)->result]` recursing on itself —
        // produces a FORMULA_REV candidate that re-enters the same sub with
        // the same target, blowing the formula-depth budget. The downstream
        // `prepare_sub_bindings` skip-logic only handles pure `Var(target)`
        // bindings (via `is_var(expr) && expr->name == skip_parent_var`);
        // anything compound slips through, so this structural pre-filter is
        // required. The pure-Var case `tpa_sq2(x)` (positional-arg sugar
        // expands to binding `x=x`) is preserved — that path is the standard
        // reverse-positional flow.
        for (auto& call : formula_calls)
            for (auto& [sub_var, expr] : call.bindings) {
                if (!contains_var(expr, target)) continue;
                // Self-circular guard: skip the compound `sub_var == target`
                // case (`n = n-1` inside a self-recursive section). The pure-Var
                // case (`x = x` from positional-arg sugar) is kept — the
                // downstream `prepare_sub_bindings` skip-logic absorbs it.
                const bool pure_var_match = is_var(expr) && expr->name == target;
                if (sub_var == target && !pure_var_match) continue;
                if (handler(Candidate{CandidateType::FORMULA_REV, nullptr,
                    target + " via " + call.file_stem + "(" + std::string(sub_var) + ")",
                    &call, sub_var, nullptr, next_group++}))
                    return;
            }

        // Strategy 7: cross-equation variable elimination
        // For target T in equation E1 with unknown U, find E2 that can express U.
        // Substitute U into E1, then solve for T. If the result still contains
        // another unknown V, try a second elimination from remaining equations.
        // justified: nested cross-pair search with `j == i` / `k == i` skip-self
        for (size_t i = 0; i < equations.size(); i++) {
            auto& e1 = equations[i];
            if (!contains_var(e1.rhs, target)) continue;
            std::set<std::string> e1_vars;
            collect_vars(e1.rhs, e1_vars);
            for (auto& u : e1_vars) {
                if (u == target) continue;
                if (sub_bindings && sub_bindings->count(u)) continue;
                if (is_active_builtin(u)) continue;
                // justified: cross-pair scan with j == i skip-self
                for (size_t j = 0; j < equations.size(); j++) {
                    if (j == i) continue;
                    auto& e2 = equations[j];
                    std::vector<ExprPtr> u_exprs;
                    if (e2.lhs_var == u) {
                        u_exprs.push_back(e2.rhs);
                    } else if (contains_var(e2.rhs, u)) {
                        auto sols = solve_for_all(Expr::Var(e2.lhs_var), e2.rhs, u);
                        for (const auto& s : sols) if (s.expr) u_exprs.push_back(s.expr);
                    } else continue;
                    for (auto& u_expr : u_exprs) {
                        if (contains_var(u_expr, u)) continue; // circular
                        auto subst_rhs = simplify(substitute(e1.rhs, u, u_expr));
                        // Collect remaining unknowns (exclude target, known, builtins)
                        std::set<std::string> remaining;
                        collect_vars(subst_rhs, remaining);
                        remaining.erase(target);
                        remaining.erase(e1.lhs_var);
                        if (sub_bindings) for (auto& [k,v] : *sub_bindings) remaining.erase(k);
                        for (auto it = remaining.begin(); it != remaining.end();)
                            if (is_active_builtin(*it)) it = remaining.erase(it); else ++it;
                        // Remove equation LHS vars — they have defining equations and will
                        // be resolved by try_resolve/try_derive; if resolution fails,
                        // the candidate is discarded naturally by the handler.
                        for (auto& eq : equations)
                            remaining.erase(eq.lhs_var);
                        // Try solving directly
                        auto try_solve_and_emit = [&](const ExprPtr& rhs, const std::string& desc) -> bool {
                            auto tsols = solve_for_all(Expr::Var(e1.lhs_var), rhs, target);
                            const int sub_group = next_group++; // one group per elim source
                            for (auto& ts : tsols) {
                                if (!ts.expr) continue;
                                const Condition* cond = e1.condition ? &*e1.condition : nullptr;
                                if (!cond && e2.condition) cond = &*e2.condition;
                                if (handler(Candidate{CandidateType::EXPR, ts.expr, desc,
                                    nullptr, "", cond, sub_group}))
                                    return true;
                            }
                            return false;
                        };
                        if (remaining.empty()) {
                            if (try_solve_and_emit(subst_rhs,
                                target + " = ...  (elim " + u + " via " + e2.lhs_var + ")"))
                                return;
                        }
                        // Second-level elimination for each remaining unknown V
                        for (auto& v : remaining) {
                            // justified: second-level cross-pair scan with k == i skip-self
                            for (size_t k = 0; k < equations.size(); k++) {
                                if (k == i) continue;
                                auto& e3 = equations[k];
                                std::vector<ExprPtr> v_exprs;
                                if (e3.lhs_var == v) {
                                    v_exprs.push_back(e3.rhs);
                                } else if (contains_var(e3.rhs, v)) {
                                    auto vs = solve_for_all(Expr::Var(e3.lhs_var), e3.rhs, v);
                                    for (const auto& s : vs) if (s.expr) v_exprs.push_back(s.expr);
                                } else continue;
                                for (auto& v_expr : v_exprs) {
                                    if (contains_var(v_expr, v)) continue;
                                    auto subst2 = simplify(substitute(subst_rhs, v, v_expr));
                                    if (try_solve_and_emit(subst2,
                                        target + " = ...  (elim " + u + "," + v + ")"))
                                        return;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Strategy 6: numeric root-finding (--numeric only)
        //
        // gen-5 cycle 3h (2026-05-16, Future #92): condition-aware emission.
        // An equation like `result = n if n <= 1` is structurally probeable
        // for `n` (the condition constrains n's domain), but the original
        // skip-predicate `lhs_var != target && !contains_var(rhs, target)`
        // suppressed emission because `n` does not appear in the RHS.
        // Extending the predicate to ALSO consult the condition closes the
        // gap and unblocks recursive FUNCTION_SECTION reverse-solve (e.g.
        // `is_in(N, fibonacci)` reaches the base case `result = n if n <= 1`
        // when scanning small integer n).
        if (numeric_mode) {
            for (auto& eq : equations) {
                const bool target_in_cond = eq.condition
                    && contains_var_in_condition(*eq.condition, target);
                if (eq.lhs_var != target
                        && !contains_var(eq.rhs, target)
                        && !target_in_cond
                        && !formula_call_bindings_contain(eq.rhs, target)) continue;
                auto combined = simplify(Expr::BinOpExpr(BinOp::SUB,
                    Expr::Var(eq.lhs_var), eq.rhs));
                if (handler(Candidate{CandidateType::NUMERIC, combined,
                    target + " ~= numeric  (from " + eq.lhs_var + " = " + expr_to_string(eq.rhs) + ")",
                    nullptr, "", eq.condition ? &*eq.condition : nullptr, next_group++}))
                    return;
            }
        }
    }

    // --- Shared helpers ---

    // Substitute all bindings into an expression. Works with both numeric and symbolic maps.
    template<typename MapType>
    [[nodiscard]] static ExprPtr substitute_bindings(ExprPtr expr, const MapType& bindings,
            const std::string& skip_var = "") {
        std::set<std::string> vars;
        collect_vars(expr, vars);
        for (auto& v : vars) {
            if (v == skip_var) continue;
            if (auto it = bindings.find(v); it != bindings.end()) {
                if constexpr (std::is_same_v<typename MapType::mapped_type, double>)
                    expr = substitute(expr, v, Expr::Num(it->second));
                else
                    expr = substitute(expr, v, it->second);
            }
        }
        return simplify(expr);
    }

    // --- Derive helpers ---

    // Build a mapping from sub-system variable names to parent-scope expressions,
    // substituting known bindings into the call's binding expressions.
    static std::map<std::string, ExprPtr> derive_unfold_bindings(
            const FormulaCall& call,
            const std::map<std::string, ExprPtr>& bindings) {
        std::map<std::string, ExprPtr> parent_map;
        for (auto& [sv, expr] : call.bindings)
            parent_map[sv] = substitute_bindings(expr, bindings);
        return parent_map;
    }

    // --- Derive (symbolic solver) ---

    [[nodiscard]] ExprPtr try_derive(const ExprPtr& expr, const std::string& target,
                       std::map<std::string, ExprPtr>& bindings,
                       std::set<std::string> visited, int depth, // NOLINT(performance-unnecessary-value-param) — intentional copy per branch
                       DeadEndSet& dead_ends) const {
        std::set<std::string> vars;
        collect_vars(expr, vars);

        bool has_target = false;
        ExprPtr resolved = expr;
        for (auto& v : vars) {
            if (v == target) { has_target = true; continue; }
            if (auto it = bindings.find(v); it != bindings.end()) {
                resolved = substitute(resolved, v, it->second);
            } else {
                auto sub_expr = derive_recursive(v, bindings, visited, depth + 1, dead_ends);
                if (sub_expr) {
                    resolved = substitute(resolved, v, sub_expr);
                    // After substitution, the target might have been introduced
                    if (contains_var(sub_expr, target)) has_target = true;
                } else {
                    return nullptr; // Can't resolve this variable — try next equation
                }
            }
        }

        simplify_clear_assumptions();
        auto result = simplify(resolved);
        for (const auto& a : simplify_get_assumptions())
            trace.step("  assuming: " + a.desc
                + (a.source == AssumptionSource::Inherent ? " (inherent)" : ""), depth + 1);

        // If the target appears in the resolved expression, we have:
        //   target = f(target, ...) — try to solve algebraically
        if (has_target) {
            auto sol = solve_for(Expr::Var(target), result, target);
            if (sol) return simplify(sol);
            return nullptr; // Non-linear in target — can't solve
        }

        // Try full evaluation — if it works, return a clean number
        if (auto val = evaluate(result)) {
            // Checked<double> already excludes NaN; only guard against infinity.
            if (!std::isinf(val.value())) return Expr::Num(val.value());
        }
        return result;
    }

    [[nodiscard]] ExprPtr derive_recursive(const std::string& target,
                             std::map<std::string, ExprPtr>& bindings,
                             std::set<std::string> visited, int depth,
                             DeadEndSet& dead_ends) const {
        if (auto it = bindings.find(target); it != bindings.end()) {
            return it->second;
        }
        if (is_active_builtin(target)) {
            return Expr::Var(target);
        }
        if (visited.count(target)) {
            return nullptr;
        }
        // Fix 1: pre-filter — skip if a sibling in this top-level derive
        // already discovered (target, current-bindings-keyset) is a dead-end.
        auto dead_key = std::make_pair(target, bindings_keyset(bindings));
        if (dead_ends.count(dead_key)) {
            return nullptr;
        }
        visited.insert(target);

        // Check condition using symbolic bindings (evaluate what we can)
        auto derive_check_condition = [&](const Condition* cond) -> bool {
            if (!cond) return true; // no condition = always valid
            std::map<std::string, double> numeric;
            for (auto& [k, v] : bindings) {
                if (auto nv = evaluate(*v)) numeric[k] = nv.value();
            }
            return check_condition(*cond, numeric);
        };

        ExprPtr found = nullptr;
        enumerate_candidates(target, [&](const Candidate& c) {
            // Check condition before trying the candidate
            if (!derive_check_condition(c.condition)) return false;

            if (c.type == CandidateType::EXPR) {
                auto result = try_derive(c.expr, target, bindings, visited, depth, dead_ends);
                if (result) { bindings[target] = result; found = result; return true; }
            } else if (c.type == CandidateType::FORMULA_FWD) {
                // Try unfolding: substitute the sub-system's equation body
                // into the parent scope as a symbolic expression
                try {
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    auto parent_map = derive_unfold_bindings(*c.call, bindings);
                    for (auto& eq : sub_sys.equations) {
                        if (eq.lhs_var != c.call->query_var) continue;
                        // Check sub-system equation condition (with mapped bindings)
                        if (eq.condition) {
                            std::map<std::string, double> cond_binds;
                            for (auto& [sv, pe] : parent_map) {
                                if (auto v = evaluate(*pe)) cond_binds[sv] = v.value();
                            }
                            if (!check_condition(*eq.condition, cond_binds))
                                continue;
                        }
                        // Substitute sub-system vars with parent expressions
                        ExprPtr unfolded = eq.rhs;
                        for (auto& [sv, pe] : parent_map)
                            unfolded = substitute(unfolded, sv, pe);
                        for (auto& [k, v] : sub_sys.defaults) {
                            if (parent_map.count(k)) continue;
                            if (k == c.call->query_var) continue;
                            unfolded = substitute(unfolded, k, Expr::Num(v));
                        }
                        unfolded = simplify(unfolded);
                        // Only use unfold if the result doesn't contain
                        // formula call outputs (which would need further
                        // resolution and may cause infinite expansion)
                        std::set<std::string> remaining;
                        collect_vars(unfolded, remaining);
                        const bool has_formula_output = std::any_of(
                            sub_sys.formula_calls.begin(), sub_sys.formula_calls.end(),
                            [&remaining](const FormulaCall& fc) {
                                return remaining.count(fc.output_var) > 0;
                            });
                        if (!has_formula_output) {
                            bindings[target] = unfolded;
                            found = unfolded;
                            return true;
                        }
                    }
                // NOLINTNEXTLINE(bugprone-empty-catch) — unfold/load failure → fall back to direct sub-system derivation below
                } catch (const std::runtime_error&) {}

                // Fallback: derive into sub-system directly (original approach)
                {
                    std::map<std::string, ExprPtr> sub_binds;
                    for (auto& [sv, expr] : c.call->bindings) {
                        ExprPtr resolved = expr;
                        std::set<std::string> vars;
                        collect_vars(expr, vars);
                        bool all_resolved = true;
                        for (auto& v : vars)
                            if (auto it = bindings.find(v); it != bindings.end())
                                resolved = substitute(resolved, v, it->second);
                            else all_resolved = false;
                        if (all_resolved) sub_binds[sv] = simplify(resolved);
                    }
                    if (auto it = bindings.find(c.call->output_var); it != bindings.end())
                        sub_binds[c.call->query_var] = it->second;
                    try {
                        auto& sub_sys = load_sub_system(c.call->file_stem);
                        for (auto& [k, v] : sub_sys.defaults)
                            if (!sub_binds.count(k) && k != c.call->query_var)
                                sub_binds[k] = Expr::Num(v);
                        // Fix 1: sub-systems get a fresh DeadEndSet (reset at
                        // formula-call entry) so sub-system failures don't
                        // poison the caller's sibling candidates.
                        DeadEndSet sub_dead_ends;
                        auto result = sub_sys.derive_recursive(c.call->query_var, sub_binds, {}, depth + 1, sub_dead_ends);
                        if (result) { bindings[target] = result; found = result; return true; }
                    } catch (const std::runtime_error&) { return false; }
                }
            } else if (c.type == CandidateType::FORMULA_REV) {
                // Unfold: substitute sub-system equation body into parent scope
                // then solve for target (which appears in a binding expression)
                try {
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    std::map<std::string, ExprPtr> parent_map;
                    for (auto& [sv, expr] : c.call->bindings)
                        parent_map[sv] = expr;
                    const std::string sub_target = c.sub_var;
                    ExprPtr binding_expr = parent_map[sub_target];
                    for (const auto& eq : sub_sys.equations) {
                        if (eq.lhs_var != c.call->query_var) continue;
                        // Check sub-system equation condition if possible
                        if (eq.condition) {
                            std::map<std::string, double> cond_binds;
                            for (auto& [sv, pe] : parent_map) {
                                if (auto v = evaluate(*pe)) cond_binds[sv] = v.value();
                            }
                            for (auto& [k, v] : bindings) {
                                if (auto nv = evaluate(*v)) cond_binds[k] = nv.value();
                            }
                            if (!check_condition(*eq.condition, cond_binds))
                                continue;
                        }
                        ExprPtr unfolded = eq.rhs;
                        for (auto& [sv, pe] : parent_map) {
                            if (sv == sub_target) continue;
                            unfolded = substitute(unfolded, sv, pe);
                        }
                        for (auto& [k, v] : sub_sys.defaults) {
                            if (parent_map.count(k)) continue;
                            if (k == c.call->query_var || k == sub_target) continue;
                            unfolded = substitute(unfolded, k, Expr::Num(v));
                        }
                        unfolded = simplify(unfolded);
                        // Use solve_for_all to get all solutions (e.g., abs → two)
                        auto sols = solve_for_all(Expr::Var(c.call->output_var), unfolded, sub_target);
                        for (auto& sol : sols) {
                            if (!sol.expr) continue;
                            ExprPtr final_expr = nullptr;
                            if (is_var(binding_expr) && binding_expr->name == target) {
                                auto b = bindings; // fresh copy per branch
                                final_expr = try_derive(sol.expr, target, b, visited, depth, dead_ends);
                            } else {
                                auto final_sols = solve_for_all(sol.expr, binding_expr, target);
                                for (auto& fs : final_sols) {
                                    if (!fs.expr) continue;
                                    auto b = bindings;
                                    final_expr = try_derive(fs.expr, target, b, visited, depth, dead_ends);
                                    if (final_expr) break;
                                }
                            }
                            if (final_expr) {
                                bindings[target] = final_expr;
                                found = final_expr;
                                return true; // derive_recursive returns first; derive_all iterates
                            }
                        }
                    }
                // NOLINTNEXTLINE(bugprone-empty-catch) — sub-system load or solve_for_all failure → skip reverse unfold
                } catch (const std::runtime_error&) {}
            }
            return false;
        });
        // Fix 1: post-fail — record dead-end before returning nullptr so
        // sibling candidates in the outer query don't redundantly re-explore.
        if (!found) dead_ends.insert(dead_key);
        return found;
    }

    // --- Numeric solver ---

    // Memoized resolve for numeric scanning — caches results to avoid
    // redundant recursive evaluations (critical for factorial, fibonacci).
    //
    // Fix 2: when called from within a top-level query (e.g. the system-probe
    // fallback in try_resolve_numeric), accept the caller's DeadEndSet so
    // probe iterations share dead-end knowledge across the 200+ samples.
    // When dead_ends is null (direct external call or a re-entrant test),
    // a fresh resolve() top-level call handles its own set and guards.
    [[nodiscard]] double resolve_memoized(const std::string& target,
                            std::map<std::string, double> bindings,
                            DeadEndSet* dead_ends = nullptr) const {
        // Build cache key: target + sorted bindings
        std::string key = target;
        // Heuristic reserve: each binding contributes ~20 chars (",name=12.345").
        key.reserve(target.size() + bindings.size() * 20);
        for (auto& [k, v] : bindings)
            key += "," + k + "=" + fmt_num(v);

        auto it = numeric_memo_.find(key);
        if (it != numeric_memo_.end()) return it->second;

        double result;
        if (dead_ends) {
            // Caller is already inside a top-level guarded context (rewrite
            // rules, func inverter, budget, arena scope). Reuse the caller's
            // DeadEndSet so sibling probe iterations benefit from each
            // iteration's recorded dead-ends.
            auto prepared = prepare_bindings(target, bindings);
            if (auto pit = prepared.find(target); pit != prepared.end())
                result = pit->second;
            else {
                std::set<std::string> visited;
                result = solve_recursive(target, prepared, visited, 0, *dead_ends);
            }
        } else {
            result = resolve(target, bindings);
        }
        numeric_memo_[key] = result;
        return result;
    }

    // Extract numeric bounds for a variable from conditions and global conditions
    std::pair<double, double> extract_bounds(
            const std::string& target,
            const std::map<std::string, double>& bindings,
            const Condition* eq_condition = nullptr) const {
        double lo = NUMERIC_DEFAULT_LO, hi = NUMERIC_DEFAULT_HI;

        auto apply_valueset = [&](const ValueSet& vs) {
            for (const auto& iv : vs.intervals()) {
                if (!std::isinf(iv.low) && iv.low > lo) lo = iv.low;
                if (!std::isinf(iv.high) && iv.high < hi) hi = iv.high;
            }
        };

        // Equation condition
        if (eq_condition) apply_valueset(eq_condition->to_valueset(target, bindings));

        // Global conditions
        for (const auto& gc : global_conditions)
            apply_valueset(gc.to_valueset(target, bindings));

        return {lo, hi};
    }

    // Try to solve for target numerically by finding roots of f(target) = 0
    [[nodiscard]] std::vector<double> try_resolve_numeric(
            const ExprPtr& combined, const std::string& target,
            std::map<std::string, double>& bindings,
            const std::set<std::string>& visited, int depth,
            const Condition* eq_condition,
            DeadEndSet& dead_ends) const {
        enforce_solve_budget(); // Part C: insurance

        // Re-entrance guard: prevent infinite recursion on coupled systems
        static thread_local std::set<std::string> numeric_active_;
        if (numeric_active_.count(target)) return {};
        numeric_active_.insert(target);
        struct NumericGuard { const std::string& t; std::set<std::string>& s;
            ~NumericGuard() { s.erase(t); } } const ng_{target, numeric_active_};

        // Build set of formula call output vars (may depend on target circularly)
        std::set<std::string> formula_outputs;
        for (auto& fc : formula_calls)
            formula_outputs.insert(fc.output_var);

        // Substitute all known bindings, resolve unknowns recursively
        // Skip formula call outputs — they may depend on the target
        ExprPtr expr = combined;
        std::set<std::string> vars;
        collect_vars(expr, vars);
        bool has_formula_vars = false;
        for (auto& v : vars) {
            if (v == target) continue;
            if (formula_outputs.count(v)) { has_formula_vars = true; continue; }
            if (auto it = bindings.find(v); it != bindings.end()) {
                expr = substitute(expr, v, Expr::Num(it->second));
            } else {
                // Part A pre-filter: don't recurse on known dead-end vars.
                if (dead_ends.count({v, bindings_keyset(bindings)})) {
                    has_formula_vars = true;
                    continue;
                }
                try {
                    // Deliberate copy (not a reference): this numeric-probe side
                    // channel needs visit-isolation from the parent path, so it must
                    // NOT share solve_recursive's by-ref visited set (cycle 3k).
                    std::set<std::string> visited_copy = visited;
                    const double val = solve_recursive(v, bindings, visited_copy, depth + 1, dead_ends);
                    expr = substitute(expr, v, Expr::Num(val));
                } catch (const SolveBudgetExceededError&) { throw; }
                catch (const std::runtime_error&) { has_formula_vars = true; }
            }
        }
        expr = simplify(expr);

        // Extract bounds from conditions
        auto [lo, hi] = extract_bounds(target, bindings, eq_condition);
        if (lo >= hi) return {};

        // Check if target still appears after substitution
        const bool has_target = contains_var(expr, target);

        // Heuristic: try integer mode if bounds are reasonable integers
        const bool try_integer = (lo >= -10000 && hi <= 10000
            && std::floor(lo) == lo && std::floor(hi) == hi
            && (hi - lo) <= 20000);

        std::vector<double> roots;

        if (has_target && !has_formula_vars) {
            // Equation-based: f(target) = combined_expr(target) = 0
            auto f = [&, expr](double x) -> double {
                ExprPtr subst = substitute(expr, target, Expr::Num(x));
                return evaluate(*simplify(subst)).value_or_nan();
            };

            // M4: compute symbolic derivative once and pass to newton_solve.
            // If symbolic_diff_simplified returns null (unknown function, etc.),
            // fp_ptr stays null and Newton uses central finite differences.
            ExprPtr d_expr = symbolic_diff_simplified(*expr, target);
            std::function<double(double)> fp_fn;  // std::function: storage for optional derivative; pointer is taken below and passed to newton_solve
            const std::function<double(double)>* fp_ptr = nullptr;  // std::function: pointer-to-optional pattern paired with fp_fn above
            if (d_expr) {
                fp_fn = [&, d_expr](double x) -> double {
                    ExprPtr subst = substitute(d_expr, target, Expr::Num(x));
                    return evaluate(*simplify(subst)).value_or_nan();
                };
                fp_ptr = &fp_fn;
            }

            if (try_integer) {
                roots = find_numeric_roots(f, lo, hi, true, numeric_samples, fp_ptr);
                if (!roots.empty()) goto filter;
            }
            roots = find_numeric_roots(f, lo, hi, false, numeric_samples, fp_ptr);
            if (!roots.empty()) goto filter;
        }

        // System-probe fallback: for each candidate target value,
        // evaluate the system forward and check if known bindings match.
        // This handles recursive calls where the equation can't be evaluated in isolation.
        {
            // Find variables that are both known (in bindings) and computable from target
            // e.g., for factorial: result=120 is known, result can be computed from n
            std::vector<std::string> probe_vars;
            for (const auto& eq : equations) {
                if (eq.lhs_var == target) continue; // target on LHS = normal direction
                if (contains_var(eq.rhs, target)) continue; // target in RHS = equation-based (tried above)
                // eq.lhs_var is defined by equations — if it's in bindings, we can probe
                if (bindings.count(eq.lhs_var))
                    probe_vars.push_back(eq.lhs_var);
            }
            // Also check: any variable in bindings that could be computed from target
            for (const auto& binding : bindings) {
                const std::string& bvar = binding.first;
                if (bvar == target) continue;
                const bool found = std::any_of(probe_vars.begin(), probe_vars.end(),
                    [&bvar](const std::string& pv) { return pv == bvar; });
                if (!found) probe_vars.push_back(bvar);
            }

            // Suppress trace during probe scans — each probe point calls
            // resolve_memoized which triggers full solve_recursive traces.
            // With 200+ scan points this produces enormous --steps output.
            auto saved_trace = trace.level;
            trace.level = TraceLevel::NONE;
            struct TraceGuard { Trace& t; TraceLevel l; ~TraceGuard() { t.level = l; } } const tg_{trace, saved_trace};

            for (auto& probe_var : probe_vars) {
                if (!bindings.count(probe_var)) continue;
                double expected = bindings.at(probe_var);

                auto f = [&](double x) -> double {
                    try {
                        auto test_binds = bindings;
                        test_binds[target] = x;
                        test_binds.erase(probe_var); // remove so it gets recomputed
                        // Fix 2: share the outer DeadEndSet across probe
                        // iterations — first iteration populates, later
                        // iterations benefit from pre-filter short-circuits.
                        const double computed = resolve_memoized(probe_var, test_binds, &dead_ends);
                        return computed - expected;
                    } catch (const std::runtime_error&) { return std::numeric_limits<double>::quiet_NaN(); }
                };

                if (try_integer) {
                    roots = find_numeric_roots(f, lo, hi, true, numeric_samples);
                    if (!roots.empty()) goto filter;
                }
                roots = find_numeric_roots(f, lo, hi, false, numeric_samples);
                if (!roots.empty()) goto filter;
            }
        }

        return {};

        filter:
        // Filter by equation condition and global conditions
        std::vector<double> filtered;
        for (const double r : roots) {
            auto test_binds = bindings;
            test_binds[target] = r;
            bool ok = true;
            if (eq_condition && !check_condition(*eq_condition, test_binds)) ok = false;
            for (const auto& gc : global_conditions)
                if (!check_condition(gc, test_binds)) ok = false;
            if (ok) filtered.push_back(r);
        }
        return filtered;
    }

    // --- Solver ---

    [[nodiscard]] double solve_recursive(const std::string& target,
                           std::map<std::string, double>& bindings,
                           std::set<std::string>& visited, int depth,
                           DeadEndSet& dead_ends) const {
        if (auto it = bindings.find(target); it != bindings.end()) {
            trace.calc("known: " + target + " = " + fmt_trace(it->second, nullptr, target), depth + 1);
            return it->second;
        }
        if (is_active_builtin(target)) {
            const double val = builtin_constants().at(target);
            bindings[target] = val;
            return val;
        }
        if (visited.count(target))
            throw std::runtime_error(
                "Circular dependency: '" + target + "' depends on itself through a chain of equations");
        // Part A: short-circuit if we've already discovered target is unreachable
        // with this exact set of bindings (a sibling candidate earlier in the
        // top-level query exhausted all paths to it).
        auto dead_key = std::make_pair(target, bindings_keyset(bindings));
        if (dead_ends.count(dead_key))
            throw std::runtime_error("Cannot solve for '" + target + "'");
        visited.insert(target);
        // RAII: erase `target` on BOTH normal return and exception unwind so the
        // by-reference `visited` always reflects the path-from-root, matching the
        // prior by-value isolation. Load-bearing for the catch-and-retry path in
        // try_resolve (see Future #98 sibling-leak regression test). Placed after
        // the early-throws above so cycle-detected / dead-end pre-insert exits are
        // not covered (target was not inserted on those paths).
        struct VisitedGuard {
            std::set<std::string>& s; const std::string& t;
            ~VisitedGuard() { s.erase(t); }
        } const visited_guard{visited, target};

        bool found_eq = false;
        bool had_nan_inf = false;
        std::set<std::string> missing;

        auto try_expr = [&](const ExprPtr& expr, const std::string& label) -> bool {
            found_eq = true;
            trace.step(label, depth + 1);
            return try_resolve(expr, target, bindings, visited, depth, had_nan_inf, missing, dead_ends);
        };

        auto try_formula = [&](const FormulaCall& call, const std::string& resolve_var,
                               const std::string& skip_var = "") -> bool {
            found_eq = true;
            if (formula_depth_ >= max_formula_depth)
                throw FormulaDepthExceededError();
            trace.step("formula call: " + call.file_stem + "(" + resolve_var + ")", depth + 1);
            try {
                formula_depth_++;
                struct DepthGuard { ~DepthGuard() { formula_depth_--; } } const guard;
                auto sub_binds = prepare_sub_bindings(call, bindings, visited, depth, skip_var,
                                                      true, &dead_ends);
                auto& sub_sys = load_sub_system(call.file_stem);
                sub_sys.max_formula_depth = max_formula_depth;
                for (auto& [sv, val] : sub_binds)
                    trace.calc("  binding: " + sv + " = " + fmt_trace(val), depth + 2);

                // @extern fast path: if sub-system has extern_func and we're
                // resolving the return var with all inputs known, call C++ directly
                double result;
                bool used_extern = false;
                for (auto& sec : sub_sys.sections_) {
                    if (!sec.extern_func.empty() && resolve_var == sec.return_var) {
                        auto& registry = builtin_functions();
                        auto fit = registry.find(sec.extern_func);
                        if (fit != registry.end() && sec.positional_args.size() == 1) {
                            auto ait = sub_binds.find(sec.positional_args[0]);
                            if (ait != sub_binds.end()) {
                                result = fit->second(ait->second);
                                // T6: @extern arg may be a known parent var (look up by name);
                                // result is C++-computed — alias-table fallback only.
                                trace.step("  @extern " + sec.extern_func
                                    + "(" + fmt_trace(ait->second, nullptr, ait->first) + ") = "
                                    + fmt_trace(result), depth + 2);
                                used_extern = true;
                                break;
                            }
                        }
                    }
                }
                if (!used_extern) {
                    // gen-5 cycle 3g (2026-05-16): use resolve_memoized so
                    // recursive bodies share the sub's numeric_memo_ — collapses
                    // O(2^n) Fibonacci-style recursion to O(n). Passing
                    // `&dead_ends` from the enclosing solve_recursive parameter
                    // keeps the lighter "non-outermost" code path (no fresh
                    // BudgetGuard / solved_symbolic_.clear() / Guard installs),
                    // which also shrinks the per-call stack frame enough that
                    // a runaway recursion like cycle-3d's `[bad(n)]=bad(n+1)`
                    // hits formula_depth_'s 1000 limit before blowing the
                    // 8MB stack. T7 bridge below still reads solved_symbolic_
                    // (stale on memo hit but numerically correct — see design D4).
                    result = sub_sys.resolve_memoized(resolve_var, sub_binds, &dead_ends);
                }
                if (std::isnan(result) || std::isinf(result)) { had_nan_inf = true; return false; }
                // T7 sub-system bridge: borrow the sub-system's recognized
                // symbolic form (populated at its T10) so the parent's trace
                // and final share the same ExprPtr. Sub-systems are kept
                // alive by `sub_systems` for the parent's lifetime — the
                // ExprPtr is arena-allocated in the sub-system's arena;
                // safe to read here. Replace with typed FORMULA_CALL nodes
                // when Future #20 lands (then this 5-line bridge deletes).
                ExprPtr sub_sym = nullptr;
                if (!used_extern) {
                    if (auto sit = sub_sys.solved_symbolic_.find(resolve_var);
                            sit != sub_sys.solved_symbolic_.end())
                        sub_sym = sit->second;
                    if (sub_sym) solved_symbolic_[target] = sub_sym;
                }
                trace.step("  result: " + target + " = " + fmt_trace(result, sub_sym), depth + 1);
                bindings[target] = result;
                return true;
            } catch (const FormulaDepthExceededError&) {
                throw; // propagate depth guard (was stringly-typed `msg.find("depth")` pre-cycle-3j)
            } catch (const std::runtime_error& e) {
                trace.step("  failed: " + std::string(e.what()), depth + 2);
                return false;
            }
        };

        bool solved = false;
        enumerate_candidates(target, [&](const Candidate& c) {
            enforce_solve_budget(); // Part C: insurance — per-candidate-evaluation
            // Check condition BEFORE solving if all vars are known
            if (c.condition && !check_condition(*c.condition, bindings)) {
                trace.step("  condition failed (pre-check), skipping", depth + 1);
                found_eq = true;
                return false; // skip this candidate
            }

            bool ok = false;
            switch (c.type) {
                case CandidateType::EXPR:
                    ok = try_expr(c.expr, c.desc); break;
                case CandidateType::FORMULA_FWD:
                    ok = try_formula(*c.call, c.call->query_var); break;
                case CandidateType::FORMULA_REV:
                    ok = try_formula(*c.call, c.sub_var, target); break;
                case CandidateType::NUMERIC: {
                    // Numeric probing is for the user's top-level intent
                    // (transcendental equations, direct numerical inversion).
                    // At nested depth, we're resolving a free variable inside
                    // another candidate's evaluation — numeric probing at that
                    // level is almost always a dead end and drives exponential
                    // fan-out in densely-interconnected systems (triangle.fw).
                    if (depth > 0) { found_eq = true; break; }
                    found_eq = true;
                    trace.step(c.desc, depth + 1);
                    auto roots = try_resolve_numeric(c.expr, target, bindings,
                        visited, depth, c.condition, dead_ends);
                    if (!roots.empty()) {
                        bindings[target] = roots[0];
                        numeric_results_[target] = false;
                        ok = true;
                    }
                    break;
                }
                case CandidateType::COUNT_: assert(false); break;
            }

            if (ok) {
                // Check equation condition AFTER solving
                if (c.condition && !check_condition(*c.condition, bindings)) {
                    trace.step("  condition failed (post-check), trying next", depth + 1);
                    bindings.erase(target);
                    return false;
                }
                // Check global conditions
                // not std::any_of: body emits trace.step + bindings.erase before returning; multi-action
                for (const auto& gc : global_conditions) {
                    // cppcheck-suppress useStlAlgorithm
                    if (!check_condition(gc, bindings)) {
                        trace.step("  global condition failed, trying next", depth + 1);
                        bindings.erase(target);
                        return false;
                    }
                }
                solved = true;
                return true;
            }
            return false;
        }, &bindings);
        if (solved) {
            return bindings.at(target);
        }

        // Part A: record dead-end before propagating the failure — sibling
        // candidates in the outer query won't redundantly re-try the same
        // free vars with the same bindings.
        dead_ends.insert(dead_key);

        // Error reporting
        if (!found_eq)
            throw std::runtime_error("No equation found for '" + target + "'");
        if (had_nan_inf && missing.empty())
            throw std::runtime_error("Cannot solve for '" + target
                + "': all equations produced invalid results (NaN or infinity)");
        if (!missing.empty()) {
            std::string list;
            for (const auto& v : missing) list += (list.empty() ? "" : ", ") + ("'" + v + "'");
            throw std::runtime_error("Cannot solve for '" + target + "': no value for " + list);
        }
        throw std::runtime_error("Cannot solve for '" + target + "'");
    }

    [[nodiscard]] bool try_resolve(const ExprPtr& expr, const std::string& target,
                     std::map<std::string, double>& bindings,
                     std::set<std::string>& visited, int depth,
                     bool& had_nan_inf, std::set<std::string>& missing,
                     DeadEndSet& dead_ends) const {
        enforce_solve_budget(); // Part C: insurance — should never trip given Part A
        // Resolve all free variables in the expression
        std::set<std::string> vars;
        collect_vars(expr, vars);

        ExprPtr resolved = expr;
        for (auto& v : vars) {
            if (v == target) return false;
            if (auto it = bindings.find(v); it != bindings.end()) {
                trace.calc("substitute " + v + " = " + fmt_trace(it->second, nullptr, v), depth + 2);
                resolved = substitute(resolved, v, Expr::Num(it->second));
            } else {
                // Part A: pre-filter — skip this candidate without descending
                // if a sibling already discovered (v, current-bindings-keyset)
                // is a dead-end.
                if (dead_ends.count({v, bindings_keyset(bindings)})) {
                    missing.insert(v);
                    return false;
                }
                trace.step("need: " + v, depth + 2);
                try {
                    const double val = solve_recursive(v, bindings, visited, depth + 1, dead_ends);
                    trace.calc("substitute " + v + " = " + fmt_trace(val, nullptr, v), depth + 2);
                    resolved = substitute(resolved, v, Expr::Num(val));
                } catch (const SolveBudgetExceededError&) { throw; }
                catch (const FormulaDepthExceededError&) {
                    throw; // propagate depth guard (was stringly-typed `msg.find("depth")` pre-cycle-3j)
                }
                catch (const std::runtime_error&) {
                    missing.insert(v);
                    return false;
                }
            }
        }

        trace.calc("evaluate: " + expr_to_string(resolved), depth + 2);
        simplify_clear_assumptions();
        const auto* simplified = simplify(resolved);
        auto assumptions = simplify_get_assumptions();
        for (const auto& a : assumptions)
            trace.step("  assuming: " + a.desc
                + (a.source == AssumptionSource::Inherent ? " (inherent)" : ""), depth + 2);
        auto result_opt = evaluate(simplified);
        if (!result_opt) {
            // Empty can mean either (a) an unresolved variable / unknown function
            // (structural failure — fall through silently) or (b) a NaN propagated
            // from eval_div / sqrt(-1) / log(-1) (numeric failure — flag as
            // "all equations produced invalid results" for the user).
            // value_or_nan() returns NaN in both cases, so we distinguish by
            // re-evaluating: structural failures leave behind free variables.
            std::set<std::string> free_vars;
            collect_vars(simplified, free_vars);
            for (auto& [k, _] : bindings) free_vars.erase(k);
            if (free_vars.empty()) {
                trace.step("result is NaN, trying alternatives", depth + 1);
                had_nan_inf = true;
            }
            return false;
        }
        const double result = result_opt.value();
        if (std::isinf(result)) {
            trace.step("result is inf, trying alternatives", depth + 1);
            had_nan_inf = true;
            return false;
        }
        // T10: write the recognized symbolic form to the provenance carrier
        // BEFORE rendering — trace and final share this exact ExprPtr.
        // expr_recognize_constants takes `const Expr*` and handles the bridge
        // to tree_map_leaf internally.
        ExprPtr recognized = expr_recognize_constants(simplified, aliases_);
        solved_symbolic_[target] = recognized;
        trace.step("result: " + target + " = " + fmt_trace(result, recognized), depth + 1);
        bindings[target] = result;
        return true;
    }
};

// ============================================================================
//  CLI query parsing
// ============================================================================

struct CLIQueryVar {
    std::string variable;   // formula variable name
    std::string alias;      // output name
    bool strict = false;    // ?! mode — error if multiple results
};

struct CLIQuery {
    std::string filename;
    std::string section;        // section name (from file.section syntax)
    std::string inline_source;  // inline equations (query-first format)
    std::vector<CLIQueryVar> queries;
    std::map<std::string, double> bindings;
    // Future #5: table-mode range bindings — CLI-order-preserving (parallel to
    // `queries`). Each entry is (var_name, expanded values). Populated only
    // when a `..`-bearing bracketed value (e.g. `a=[1..10]`) is parsed; empty
    // for all existing CLI paths.
    std::vector<std::pair<std::string, std::vector<double>>> range_bindings;
    std::map<std::string, std::string> symbolic; // formula_var -> output_name (derive mode)
    // Future #21 (nested form): top-level args that are themselves formula
    // calls — `outer(result=?, inner(z=?x, p=3))`. Parsed via the same
    // `extract_formula_calls` token-level primitive `.fw` files use, then
    // injected into `sys.formula_calls` by main.cpp before the first solve
    // dispatch. The synthetic alias (`x` above) routes the inner call's
    // result into the parent scope as a regular variable.
    std::vector<FormulaCall> nested_calls;
    // Future #67: post-load synthetic equations from CLI sugar.
    // Populated by parse_cli_query when an arg is recognised as a resolve-at-
    // load FUNC_CALL (currently `integral(...)`/`diff(...)` — same set as the
    // load_with_sections post-load passes). Loaded by main.cpp via
    // `sys.load_string(q.synthetic_equations, "<cli-resolve-at-load>")`
    // AFTER the file / inline source, BEFORE the standard query dispatch
    // loop. Empty for all existing non-resolve-at-load CLI paths.
    // `synthetic_aliases` records the alias names emitted alongside (a subset
    // of `queries[i].alias`). Pass 2's resolve-failure fallback uses this set
    // to print the symbolic RHS for free-variable cases (e.g.
    // `diff(distance, time)=?slope` with no `velocity=` binding → `slope =
    // velocity`), matching the pre-unification Pass 1.5 / 1.6 behaviour.
    std::string synthetic_equations;
    std::set<std::string> synthetic_aliases;
};

// Future #5: parse a bracketed range value like `[1..10]`, `[1..10 @ 0.5]`, or
// the compound form `[1..5, 6..10]`. Returns the expanded numeric sequence.
// Sub-range grammar: `start..stop[ @ step]`. Defaults: step=1 when ascending
// and unspecified; descending REQUIRES explicit negative step (refuses silent
// direction-swap). Endpoints both inclusive; count is `round((stop-start)/
// step)+1`, then values are generated as `start + i*step` (count-based, NOT
// repeated addition — avoids IEEE 754 drift). Bounds may be expressions
// (`pi/4`, `2*pi`) — reuses the same `Parser + evaluate` idiom that scalar
// CLI values use below. Throws `std::runtime_error` on malformed input, empty
// range, zero step, or unevaluable bound expressions.
[[nodiscard]] inline std::vector<double> parse_range(const std::string& val) {
    if (val.size() < 2 || val.front() != '[' || val.back() != ']')
        throw std::runtime_error("parse_range: expected '[...]' got '" + val + "'");
    const std::string inner = val.substr(1, val.size() - 2);

    // Split on top-level commas (track paren AND bracket depth for nested
    // expression bounds like `f(a, b)` inside a bound).
    std::vector<std::string> sub_ranges;
    { int depth = 0; size_t start = 0;
      for (size_t i = 0; i < inner.size(); i++) {
          const char c = inner[i];
          if      (c == '(' || c == '[') depth++;
          else if (c == ')' || c == ']') depth--;
          else if (c == ',' && depth == 0) {
              sub_ranges.push_back(trim(inner.substr(start, i - start)));
              start = i + 1;
          }
      }
      sub_ranges.push_back(trim(inner.substr(start)));
    }

    // Bound parser: stod fast-path, then Parser+evaluate fallback for expressions.
    // Same idiom `parse_cli_query` uses for scalar args, just hoisted into a lambda.
    auto parse_bound = [](const std::string& s) -> double {
        if (s.empty())
            throw std::runtime_error("parse_range: missing bound");
        double v = 0; size_t pos = 0;
        try { v = std::stod(s, &pos); }
        // NOLINTNEXTLINE(bugprone-empty-catch) — fall through to expression path
        catch (const std::invalid_argument&) { pos = 0; }
        // NOLINTNEXTLINE(bugprone-empty-catch) — fall through to expression path
        catch (const std::out_of_range&) { pos = 0; }
        if (pos == s.size()) return v;
        try {
            ExprArena temp_arena;
            const ExprArena::Scope scope(temp_arena);
            auto expr = Parser(Lexer(s).tokenize()).parse_expr();
            if (auto val_opt = evaluate(*simplify(expr)))
                return val_opt.value();
        // NOLINTNEXTLINE(bugprone-empty-catch) — failure handled by throw below
        } catch (const std::runtime_error&) {}
        throw std::runtime_error("parse_range: cannot evaluate bound '" + s + "'");
    };

    std::vector<double> values;
    for (const auto& sr : sub_ranges) {
        const size_t dotdot = sr.find("..");
        if (dotdot == std::string::npos)
            throw std::runtime_error("parse_range: missing '..' in sub-range '" + sr + "'");
        const std::string start_str = trim(sr.substr(0, dotdot));
        std::string rest = trim(sr.substr(dotdot + 2));
        if (start_str.empty() || rest.empty())
            throw std::runtime_error("parse_range: malformed sub-range '" + sr + "'");

        std::string stop_str, step_str;
        const size_t at_pos = rest.find('@');
        if (at_pos == std::string::npos) {
            stop_str = rest;
            // step_str stays empty → resolved after start/stop
        } else {
            stop_str = trim(rest.substr(0, at_pos));
            step_str = trim(rest.substr(at_pos + 1));
            if (stop_str.empty() || step_str.empty())
                throw std::runtime_error("parse_range: malformed '@ step' in '" + sr + "'");
        }
        if (stop_str.empty())
            throw std::runtime_error("parse_range: missing stop value in '" + sr + "'");

        const double start_v = parse_bound(start_str);
        const double stop_v  = parse_bound(stop_str);
        double step_v = 0.0;
        if (step_str.empty()) {
            if (start_v > stop_v)
                throw std::runtime_error("parse_range: '" + sr +
                    "' descending without explicit step (use '@ -1' to confirm direction)");
            step_v = 1.0;
        } else {
            step_v = parse_bound(step_str);
        }

        if (step_v == 0.0)
            throw std::runtime_error("parse_range: zero step in '" + sr + "'");
        if (step_v > 0.0 && start_v > stop_v)
            throw std::runtime_error("parse_range: empty range '" + sr +
                "' (start > stop with positive step)");
        if (step_v < 0.0 && start_v < stop_v)
            throw std::runtime_error("parse_range: empty range '" + sr +
                "' (start < stop with negative step)");

        // Shared count-based generation (avoids float drift). The direction/step
        // validity is already enforced above with specific error messages; the
        // generator's own guards are a no-op here. See gen_range_values (expr.h).
        const auto sub_values = gen_range_values(start_v, stop_v, step_v);
        values.insert(values.end(), sub_values.begin(), sub_values.end());
    }
    return values;
}

[[nodiscard]] inline CLIQuery parse_cli_query(const std::string& input,
                                bool allow_no_queries = false,
                                bool allow_symbolic = false) {
    CLIQuery q;

    const size_t lparen = input.find('(');
    if (lparen == std::string::npos)
        throw std::runtime_error("Expected format: filename(var=?, var=value, ...)");

    q.filename = input.substr(0, lparen);
    // Query-first format: "(args) inline equations..." — empty filename
    if (q.filename.empty()) {
        // filename stays empty — caller detects this and uses inline/stdin
    } else {
        // Split file.section: "geometry.triangle" → file="geometry.fw", section="triangle"
        // If it ends with ".fw", it's a direct file path (no section)
        // Otherwise, first dot separates file stem from section path
        const size_t dot = q.filename.find('.');
        if (dot == std::string::npos) {
            q.filename += ".fw";
        } else {
            const std::string after_dot = q.filename.substr(dot + 1);
            if (after_dot == "fw" || after_dot.find('/') != std::string::npos
                || after_dot.find('\\') != std::string::npos) {
                // It's a file extension or path — keep as-is
            } else {
                // file.section format
                q.section = after_dot;
                q.filename = q.filename.substr(0, dot) + ".fw";
            }
        }
    }

    // Find matching closing paren (respecting nesting)
    size_t rparen = std::string::npos;
    { int depth = 1;
      // justified: char-cursor — captures matching offset
      for (size_t i = lparen + 1; i < input.size(); i++) {
          if (input[i] == '(') depth++;
          else if (input[i] == ')') { if (--depth == 0) { rparen = i; break; } }
      }
    }
    if (rparen == std::string::npos)
        throw std::runtime_error("Missing closing parenthesis");

    // Capture inline source (text after closing paren)
    if (rparen + 1 < input.size()) {
        const std::string after = trim(input.substr(rparen + 1));
        if (!after.empty()) {
            q.inline_source = after;
            // If there was a "name" before (, it's a section selector, not a filename
            if (!q.filename.empty()) {
                // Strip .fw suffix if it was auto-added
                const std::string raw = input.substr(0, lparen);
                q.section = raw;
                q.filename.clear();
            }
        }
    }

    // Split arguments by comma (respecting nested parens AND brackets).
    // Future #5: bracket-depth fix — vec/mat literals and table ranges embed
    // commas inside `[...]`; without bracket tracking, `a=[1..5, 6..10]` would
    // split incorrectly at the inner comma. Mirrors the integral inner scanner
    // below at the `integral(...)` 4-arg piece split.
    std::vector<std::string> args;
    { int depth = 0; size_t start = lparen + 1;
      // justified: char-cursor with substr(start, i - start) and start = i+1
      for (size_t i = start; i <= rparen; i++) {
          if      (input[i] == '(' || input[i] == '[') depth++;
          else if (input[i] == ')' || input[i] == ']') depth--;
          if ((input[i] == ',' && depth == 0) || i == rparen) {
              auto a = trim(input.substr(start, i - start));
              if (!a.empty()) args.push_back(a);
              start = i + 1;
          }
      }
    }

    for (auto& arg : args) {
        arg = trim(arg);
        if (arg.empty()) continue;

        // Future #21 (nested form): a top-level arg may itself be a formula
        // call, e.g. `nc_inner(z=?x, p=3)`. The cheap shape check is "starts
        // with bare-IDENT immediately followed by `(`". If it matches, lex
        // the arg and run `extract_formula_calls` — the same token-level
        // primitive `.fw`-file parsing uses — to harvest a `FormulaCall`.
        // Failures fall through to the regular per-arg dispatch (preserves
        // backward compat for legitimate `var=expr` args whose RHS happens
        // to look call-like).
        {
            // Inline shape pre-check: /^[A-Za-z_][A-Za-z0-9_]*\(/
            // arg.empty() guarded by `continue` above, so arg[0] is safe here.
            bool looks_call = false;
            if (std::isalpha(static_cast<unsigned char>(arg[0])) || arg[0] == '_') {
                size_t k = 1;
                while (k < arg.size()
                       && (std::isalnum(static_cast<unsigned char>(arg[k])) || arg[k] == '_'))
                    k++;
                if (k < arg.size() && arg[k] == '(') looks_call = true;
            }
            if (looks_call) {
                auto tok = Lexer(arg).tokenize();
                auto [mod_tok, calls] = FormulaSystem::extract_formula_calls(tok);
                if (!calls.empty()) {
                    for (auto& fc : calls) {
                        // Parse-time alias collision check: `output_var` must
                        // not already be claimed by an outer query alias or
                        // a previously-injected nested call.
                        const std::string& alias = fc.output_var;
                        const bool collides_outer = std::any_of(
                            q.queries.begin(), q.queries.end(),
                            [&alias](const auto& outer) { return outer.alias == alias; });
                        if (collides_outer)
                            throw std::runtime_error(
                                "Nested-call alias collision: '" + alias
                                + "' is already an outer query alias");
                        const bool collides_prev = std::any_of(
                            q.nested_calls.begin(), q.nested_calls.end(),
                            [&alias](const auto& prev) { return prev.output_var == alias; });
                        if (collides_prev)
                            throw std::runtime_error(
                                "Nested-call alias collision: '" + alias
                                + "' is already used by another nested call");
                        q.nested_calls.push_back(std::move(fc));
                    }
                    continue;
                }
            }
        }

        const size_t eq = arg.find('=');
        if (eq == std::string::npos) {
            // Bare variable name (no '='). In symbolic modes (--derive, --fit)
            // treat as a symbolic placeholder equivalent to "name=name" —
            // matches the user's workaround of writing "b=b" to keep a variable
            // free. In numeric modes, bare names have no useful interpretation.
            if (allow_symbolic) {
                q.symbolic[arg] = arg;
                continue;
            }
            throw std::runtime_error(
                "Bare variable name '" + arg + "' is only valid with --derive or --fit; "
                "use '" + arg + "=<value>', '" + arg + "=?', or '" + arg + "=?alias' instead");
        }

        std::string name = trim(arg.substr(0, eq));
        std::string val  = trim(arg.substr(eq + 1));

        if (name.empty())
            throw std::runtime_error("Missing variable name in '" + arg + "'");

        // Future #6 + #67: `diff(target_expr, var)=?[alias]` — synthesised
        // into a regular equation `<alias> = diff(target, var)` and a regular
        // `CLIQueryVar` so the standard Pass 2 query loop handles dispatch.
        // The post-load `resolve_diff_in_equations` pass rewrites it after
        // the system's source loads.
        if (val.size() >= 1 && val[0] == '?'
            && name.size() > 5 && name.compare(0, 5, "diff(") == 0
            && name.back() == ')') {
            std::string inner = name.substr(5, name.size() - 6);
            int pd = 0; size_t comma_pos = std::string::npos;
            // justified: char-cursor — captures comma offset for substr split
            for (size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '(') pd++;
                else if (inner[i] == ')') pd--;
                else if (inner[i] == ',' && pd == 0) { comma_pos = i; break; }
            }
            if (comma_pos == std::string::npos)
                throw std::runtime_error("diff(): expected target and variable separated by ','");
            const std::string target = trim(inner.substr(0, comma_pos));
            const std::string dvar   = trim(inner.substr(comma_pos + 1));
            if (target.empty() || dvar.empty())
                throw std::runtime_error("diff(): missing target or variable");
            std::string rest = trim(val.substr(1));
            if (!rest.empty() && rest[0] == '!') rest = trim(rest.substr(1));  // ?! ignored — diff is single-result
            const std::string alias = rest.empty() ? ("diff_" + dvar) : rest;
            q.synthetic_equations += alias + " = diff(" + target + ", " + dvar + ")\n";
            q.queries.push_back({alias, alias, false});
            q.synthetic_aliases.insert(alias);
            continue;
        }

        // Future #16 (M1+M2) + #67: `integral(target_expr, var)=?[alias]` or
        // 4-arg `integral(target_expr, var, lo, hi)=?[alias]`. Synthesised
        // into a regular equation + a regular query alias, the same way diff
        // above is. The split harvests ALL top-level commas (paren AND bracket
        // depth == 0 — vec/mat literals embed COMMAs inside [...] just like
        // `parse_call_args`). 2-arg / 4-arg forms dispatch by piece count;
        // any other count is an error.
        if (val.size() >= 1 && val[0] == '?'
            && name.size() > 9 && name.compare(0, 9, "integral(") == 0
            && name.back() == ')') {
            std::string inner = name.substr(9, name.size() - 10);
            std::vector<std::string> pieces;
            { int pd = 0; size_t start = 0;
              // justified: char-cursor; captures piece boundaries via substr(start, i - start)
              for (size_t i = 0; i < inner.size(); i++) {
                  const char c = inner[i];
                  if (c == '(' || c == '[') pd++;
                  else if (c == ')' || c == ']') pd--;
                  else if (c == ',' && pd == 0) {
                      pieces.push_back(trim(inner.substr(start, i - start)));
                      start = i + 1;
                  }
              }
              pieces.push_back(trim(inner.substr(start)));
            }
            if (pieces.size() != 2 && pieces.size() != 4)
                throw std::runtime_error("integral(): expected 2-arg `integral(f, x)` or 4-arg `integral(f, x, a, b)`");
            const std::string target = pieces[0];
            const std::string ivar   = pieces[1];
            const std::string lo_text = (pieces.size() == 4) ? pieces[2] : std::string();
            const std::string hi_text = (pieces.size() == 4) ? pieces[3] : std::string();
            if (target.empty() || ivar.empty())
                throw std::runtime_error("integral(): missing target or variable");
            if (pieces.size() == 4 && (lo_text.empty() || hi_text.empty()))
                throw std::runtime_error("integral(): missing definite-integral bound");
            std::string rest = trim(val.substr(1));
            if (!rest.empty() && rest[0] == '!') rest = trim(rest.substr(1));  // ?! ignored — integral is single-result
            const std::string alias = rest.empty() ? ("integral_" + ivar) : rest;
            if (pieces.size() == 2) {
                q.synthetic_equations += alias + " = integral(" + target + ", " + ivar + ")\n";
            } else {
                q.synthetic_equations += alias + " = integral(" + target + ", " + ivar
                                       + ", " + lo_text + ", " + hi_text + ")\n";
            }
            q.queries.push_back({alias, alias, false});
            q.synthetic_aliases.insert(alias);
            continue;
        }

        if (val.size() >= 1 && val[0] == '?') {
            // Query: "x=?" or "x=?!" or "x=?alias" or "x=?!alias"
            bool strict = false;
            std::string rest = val.substr(1);
            if (!rest.empty() && rest[0] == '!') {
                strict = true;
                rest = rest.substr(1);
            }
            const std::string alias = rest.empty() ? name : trim(rest);
            q.queries.push_back({name, alias, strict});
        } else if (val.empty()) {
            throw std::runtime_error("Missing value for '" + name + "'");
        } else if (val.front() == '[' && val.find("..") != std::string::npos) {
            // Future #5: bracketed value containing '..' → range binding.
            // The `..` token is invalid Lexer input; route to `parse_range`
            // BEFORE any Lexer call. Vec literals `[1,2,3]` lack `..` and
            // fall through to the existing expression-parse path below.
            q.range_bindings.emplace_back(name, parse_range(val));
            continue;
        } else {
            // Future #67: `<name>=integral(...)` / `<name>=diff(...)` binding
            // RHS. The value side carries a resolve-at-load FUNC_CALL — emit
            // a synthetic equation so the post-load passes rewrite it just
            // like an in-file `<name> = integral(...)`. The syntactic check
            // (starts_with `integral(`/`diff(` AND ends with `)`) is the same
            // set the resolve-at-load passes currently rewrite — wider forms
            // like `2 * integral(...)` still fall through to the existing
            // `Parser+evaluate` error path.
            if (val.size() > 9 && val.compare(0, 9, "integral(") == 0 && val.back() == ')') {
                q.synthetic_equations += name + " = " + val + "\n";
                q.synthetic_aliases.insert(name);
                continue;
            }
            if (val.size() > 5 && val.compare(0, 5, "diff(") == 0 && val.back() == ')') {
                q.synthetic_equations += name + " = " + val + "\n";
                q.synthetic_aliases.insert(name);
                continue;
            }
            double v = 0;
            size_t pos = 0;
            try { v = std::stod(val, &pos); }
            catch (const std::invalid_argument&) { pos = 0; }
            catch (const std::out_of_range&) { pos = 0; }
            if (pos != val.size()) {
                // Try parsing as expression (e.g. "10*2^3", "sqrt(2)")
                bool ok = false;
                bool parsed_ok = false;
                try {
                    ExprArena temp_arena;
                    const ExprArena::Scope scope(temp_arena);
                    auto expr = Parser(Lexer(val).tokenize()).parse_expr();
                    parsed_ok = true;
                    if (auto val_opt = evaluate(*simplify(expr))) {
                        v = val_opt.value();
                        ok = true;
                    }
                // NOLINTNEXTLINE(bugprone-empty-catch) — parser failure (malformed expression) handled by the !ok branch below
                } catch (const std::runtime_error&) {}
                if (!ok) {
                    // Symbolic mode (derive / fit) opts out of post-load
                    // deferral: a non-numeric RHS like `a=side` is meant to
                    // STAY symbolic for the derive-rewrite path. Check this
                    // BEFORE the #73 synthetic-equation branch.
                    if (allow_symbolic) {
                        q.symbolic[name] = val;
                        continue;
                    }
                    // Future #73: parser succeeded but `evaluate` returned empty.
                    // The RHS references Vars that will bind after the file
                    // loads — typical case is unit suffixes like `100kg` where
                    // `kg` resolves from `stdlib/units/si-minimal.fw`. Emit as
                    // a synthetic equation so the post-load resolution handles
                    // it like an in-file binding. Same channel #67 uses for
                    // `integral(...)` / `diff(...)` resolve-at-load.
                    if (parsed_ok) {
                        q.synthetic_equations += name + " = " + val + "\n";
                        q.synthetic_aliases.insert(name);
                        continue;
                    }
                    // Reaching here means parser threw (`parsed_ok == false`):
                    // the RHS is malformed syntax, not deferred-resolution.
                    throw std::runtime_error("Invalid value '" + val + "' for variable '" + name + "'");
                }
            }
            if (std::isnan(v))
                throw std::runtime_error("NaN is not a valid value for '" + name + "'");
            if (std::isinf(v))
                throw std::runtime_error("Infinity is not a valid value for '" + name + "'");
            q.bindings[name] = v;
        }
    }

    if (q.queries.empty() && !allow_no_queries)
        throw std::runtime_error("No query variable (use var=?)");
    return q;
}
