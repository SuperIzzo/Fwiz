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

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// ============================================================================
//  Formula system
// ============================================================================

enum class CondOp : uint8_t { GT, GE, LT, LE, EQ, NE, COUNT_ };
enum class CondLogic : uint8_t { AND, OR };

struct CondClause {
    ExprPtr lhs;
    ExprPtr rhs;
    CondOp op;
};

struct Condition {
    std::vector<CondClause> clauses;
    std::vector<CondLogic> connectors; // size = clauses.size() - 1

    // Convert condition to a ValueSet for a specific variable
    // Only works for simple conditions like "x > 0", "x <= 10"
    ValueSet to_valueset(const std::string& var,
                         const std::map<std::string, double>& bindings = {}) const {
        ValueSet result = ValueSet::all();
        for (size_t i = 0; i < clauses.size(); i++) {
            const auto& c = clauses[i];
            // Check if this clause constrains `var`
            bool lhs_is_var = is_var(c.lhs) && c.lhs->name == var;
            bool rhs_is_var = is_var(c.rhs) && c.rhs->name == var;
            if (!lhs_is_var && !rhs_is_var) continue;

            // Try to evaluate the other side
            ExprPtr other = lhs_is_var ? c.rhs : c.lhs;
            ExprPtr resolved = other;
            std::set<std::string> vars;
            collect_vars(other, vars);
            for (auto& v : vars) {
                if (auto it = bindings.find(v); it != bindings.end())
                    resolved = substitute(resolved, v, Expr::Num(it->second));
                else return ValueSet::all(); // can't evaluate — return unconstrained
            }
            auto val_opt = evaluate(*simplify(resolved));
            if (!val_opt) return ValueSet::all();
            double val = val_opt.value();

            // Build ValueSet from operator (flip if var is on RHS)
            CondOp op = c.op;
            if (rhs_is_var) {
                // Flip: "5 > x" becomes "x < 5"
                switch (op) {
                    case CondOp::GT: op = CondOp::LT; break;
                    case CondOp::GE: op = CondOp::LE; break;
                    case CondOp::LT: op = CondOp::GT; break;
                    case CondOp::LE: op = CondOp::GE; break;
                    default: break;
                }
            }

            ValueSet clause_set;
            switch (op) {
                case CondOp::GT: clause_set = ValueSet::gt(val); break;
                case CondOp::GE: clause_set = ValueSet::ge(val); break;
                case CondOp::LT: clause_set = ValueSet::lt(val); break;
                case CondOp::LE: clause_set = ValueSet::le(val); break;
                case CondOp::EQ: clause_set = ValueSet::eq(val); break;
                case CondOp::NE: clause_set = ValueSet::ne(val); break;
                case CondOp::COUNT_: break;
            }

            // i == 0 always intersects (no prior connector); otherwise the
            // connector at i-1 decides intersect vs unite.
            if (i == 0 || connectors[i-1] == CondLogic::AND)
                result = result.intersect(clause_set);
            else
                result = result.unite(clause_set);
        }
        return result;
    }
};

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

// Diagnostic helper: dump dead-end set in readable form.
// Format: "(size=N) [(var1, {b1,b2}), (var2, {}), ...]"
inline void dump_dead_ends(std::ostream& os, const DeadEndSet& de) {
    os << "(size=" << de.size() << ")";
    if (de.empty()) return;
    os << " [";
    bool first = true;
    for (auto& [var, keys] : de) {
        if (!first) os << ", ";
        os << "(" << var << ", {";
        bool first_k = true;
        for (const auto& k : keys) {
            if (!first_k) os << ",";
            os << k;
            first_k = false;
        }
        os << "})";
        first = false;
    }
    os << "]";
}

// Thrown when the per-query solve budget is exhausted. Signals a critical
// bug (should never fire in practice given dead-end sharing); distinct from
// regular solve failures so CLI can return a dedicated exit code.
// Intentionally NOT derived from std::runtime_error so the many
// `catch (const std::runtime_error&)` sites in the solver don't swallow it —
// a budget breach must propagate to the top-level caller to signal the bug.
struct SolveBudgetExceededError : std::exception {
    const char* what() const noexcept override { return "TIMEOUT: solve budget exceeded"; }
};

// ============================================================================
//  Diagnostic logging (gated by FWIZ_TRACE_SOLVER env var)
// ============================================================================
// Temporary instrumentation for diagnosing triangle-hang. When FWIZ_TRACE_SOLVER
// is set in the environment, key solver entry / exit sites stream a concise
// structured line to std::cerr so we can see depth, bindings, dead-end and
// budget state. The env var is read exactly once (static-local lazy init).

inline bool fwiz_trace_solver() {
    static bool enabled = std::getenv("FWIZ_TRACE_SOLVER") != nullptr;
    return enabled;
}

constexpr int MAX_DIAGNOSTIC_SOLVE_DEPTH = 100; // hard cap while diagnosing

inline std::string diag_keyset_str(const std::map<std::string, double>& m) {
    std::string out = "{";
    bool first = true;
    for (auto& [k, _] : m) {
        if (!first) out += ",";
        out += k;
        first = false;
    }
    out += "}";
    return out;
}

inline std::string diag_set_str(const std::set<std::string>& s) {
    std::string out = "{";
    bool first = true;
    for (const auto& v : s) {
        if (!first) out += ",";
        out += v;
        first = false;
    }
    out += "}";
    return out;
}

inline std::string diag_expr_preview(const ExprPtr& e, size_t limit = 60) {
    if (!e) return "<null>";
    std::string s = expr_to_string(e);
    if (s.size() > limit) { s.resize(limit); s += "..."; }
    return s;
}

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
    mutable Trace trace;
    mutable int max_formula_depth = 1000;
    mutable bool numeric_mode = false;
    mutable bool approximate_mode = false;  // --approximate: collapse symbolic to floats in derive output
    int numeric_samples = NUMERIC_DEFAULT_SAMPLES;
    int fit_depth = FIT_DEFAULT_DEPTH;
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

    static void charge_budget() {
        if (solve_budget_depth_ == 0) return; // uninitialized (direct test calls)
        if (--solve_budget_remaining_ < 0) throw SolveBudgetExceededError();
    }

    // Collect the names (keys) of a bindings map as a set. Used to key
    // dead-end entries by available-binding context rather than specific values.
    template <typename Value>
    static std::set<std::string> bindings_keyset(
            const std::map<std::string, Value>& bindings) {
        std::set<std::string> keys;
        for (auto& [k, _] : bindings) keys.insert(k);
        return keys;
    }

    mutable std::map<std::string, std::shared_ptr<FormulaSystem>> sub_systems;
    mutable std::unordered_map<std::string, double> numeric_memo_;
    mutable std::map<std::string, bool> numeric_results_; // var → true if exact (verified)

    std::set<std::string> all_variables() const {
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
    static Section parse_section_header(const std::string& header) {
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
                std::string args_str = inner.substr(lparen + 1, rparen - lparen - 1);
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

    static std::vector<Section> split_sections(const std::vector<std::string>& all_lines) {
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
                std::string tag = trimmed.substr(1, space == std::string::npos ? std::string::npos : space - 1);
                std::string val = (space != std::string::npos) ? trim(trimmed.substr(space + 1)) : "";
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
              for (size_t ci = 0; ci < line.size(); ci++) {
                  if (line[ci] == '(') pd++;
                  else if (line[ci] == ')') pd--;
                  else if (line[ci] == '#' && pd == 0) { line = trim(line.substr(0, ci)); break; }
              }
              if (line.empty()) continue;
            }
            try { parse_line(line); }
            catch (const std::exception& e) {
                trace.step("  warning: skipping line " + std::to_string(line_num) + ": " + e.what());
            }
        }
    }

    // Load a specific section with cascading inheritance.
    // "triangle.right" loads: top-level → [triangle] → [triangle.right]
    void load_section(const std::string& section) {
        // Always load top-level (unnamed section)
        for (const auto& s : sections_)
            if (s.name.empty()) { load_lines(s.lines); break; }

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
            for (const auto& s : sections_) {
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
    bool is_active_builtin(const std::string& name) const {
        auto& consts = builtin_constants();
        if (!consts.count(name)) return false;
        if (defaults.count(name)) return false;
        for (const auto& eq : equations)
            if (eq.lhs_var == name) return false;
        return true;
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
    static std::vector<std::string> read_all_lines(std::istream& in) {
        std::vector<std::string> lines;
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first) { first = false; strip_bom(line); }
            // Split on semicolons (as line separator)
            size_t pos = 0;
            while (pos < line.size()) {
                size_t semi = line.find(';', pos);
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
log(e^x) = x
e^log(x) = x
log(x^n) = n * log(x) iff x != 0
x/x = 1 iff x != 0
x/x = undefined iff x = 0
x^0 = 1
x^1 = x
x^(1/2) = sqrt(x)
(x^a)^b = x^(a*b)
)";

    // Built-in function definitions — loaded as sub-systems when called.
    // Each maps a function name to its .fw section content.
    static const std::map<std::string, std::string>& builtin_function_defs() {
        static const std::map<std::string, std::string> defs = {
            {"sin",  "[sin(x) -> result] @extern sin; x = asin(result)"},
            {"cos",  "[cos(x) -> result] @extern cos; x = acos(result)"},
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
    }

    // Group rewrite rules by LHS pattern and check exhaustiveness.
    // Rules with the same pattern string are grouped (e.g., "x / x").
    // A group is exhaustive if its conditions' union covers all reals for every
    // constrained variable.
    void compute_rewrite_groups() {
        rewrite_rule_groups_.clear();
        std::map<std::string, size_t> key_to_group;

        for (size_t i = 0; i < rewrite_rules.size(); i++) {
            auto key = expr_to_string(rewrite_rules[i].pattern);
            auto it = key_to_group.find(key);
            if (it == key_to_group.end()) {
                key_to_group[key] = rewrite_rule_groups_.size();
                rewrite_rules[i].group_index = static_cast<int>(rewrite_rule_groups_.size());
                rewrite_rule_groups_.push_back({key, {i}, false});
            } else {
                rewrite_rules[i].group_index = static_cast<int>(it->second);
                rewrite_rule_groups_[it->second].rule_indices.push_back(i);
            }
        }

        // Check exhaustiveness for groups with multiple rules
        for (auto& group : rewrite_rule_groups_) {
            if (group.rule_indices.size() < 2) continue;

            // Collect all condition variables and their ValueSets
            std::map<std::string, ValueSet> var_coverage;
            bool all_have_conditions = true;

            for (size_t idx : group.rule_indices) {
                const auto& rule = rewrite_rules[idx];
                if (rule.condition.empty()) {
                    // Unconditional rule → covers everything
                    group.exhaustive = true;
                    break;
                }
                try {
                    auto cond = parse_condition(rule.condition);
                    if (!cond) { all_have_conditions = false; continue; }

                    // Extract constrained variables from condition
                    for (auto& clause : cond->clauses) {
                        std::string var;
                        if (is_var(clause.lhs)) var = clause.lhs->name;
                        else if (is_var(clause.rhs)) var = clause.rhs->name;
                        if (var.empty()) continue;

                        auto vs = cond->to_valueset(var);
                        if (var_coverage.count(var))
                            var_coverage[var] = var_coverage[var].unite(vs);
                        else
                            var_coverage[var] = vs;
                    }
                } catch (const std::runtime_error&) { all_have_conditions = false; }
            }

            if (group.exhaustive) continue;  // already set by unconditional rule
            if (!all_have_conditions || var_coverage.empty()) continue;

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
        for (size_t i = 0; i < rewrite_rule_groups_.size(); i++)
            rewrite_exhaustive_flags_[i] = rewrite_rule_groups_[i].exhaustive;
    }

    // Walk an expression, find FUNC_CALL nodes that aren't builtins, and convert them
    // to formula calls using positional arg metadata from the sub-system's section header.
    // Returns the expression with formula calls replaced by their output variables.
    ExprPtr extract_positional_calls(const ExprPtr& e, const std::string& eq_lhs,
                                     std::vector<FormulaCall>& calls) {
        if (!e) return e;
        if (e->type == ExprType::FUNC_CALL
            && !builtin_functions().count(e->name)
            && !custom_functions_.count(e->name)) {
            // Not a builtin — try loading as sub-system formula
            std::string file_stem = e->name;
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
                    for (const auto& sec : sub.sections_) {
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

                // Generate unique output variable name
                static int call_counter = 0;
                call.output_var = "_fc" + std::to_string(call_counter++);

                // Map positional args
                for (size_t i = 0; i < e->args.size() && i < pos_args.size(); i++) {
                    // Recursively process nested calls in the argument
                    auto arg = extract_positional_calls(e->args[i], eq_lhs, calls);
                    call.bindings[pos_args[i]] = arg;
                }

                calls.push_back(std::move(call));
                return Expr::Var(calls.back().output_var);
            } catch (const std::runtime_error&) {
                // Sub-system not found — leave as FUNC_CALL
                return e;
            }
        }

        // Recurse into sub-expressions
        if (e->type == ExprType::UNARY_NEG)
            return Expr::Neg(extract_positional_calls(e->child, eq_lhs, calls));
        if (e->type == ExprType::BINOP)
            return Expr::BinOpExpr(e->op,
                extract_positional_calls(e->left, eq_lhs, calls),
                extract_positional_calls(e->right, eq_lhs, calls));
        if (e->type == ExprType::FUNC_CALL) {
            std::vector<ExprPtr> args;
            args.reserve(e->args.size());
            for (auto& a : e->args)
                args.push_back(extract_positional_calls(a, eq_lhs, calls));
            return Expr::Call(e->name, args);
        }
        return e;
    }

    // Post-load: convert FUNC_CALL nodes that match sub-systems into formula calls
    void resolve_positional_calls() {
        for (auto& eq : equations) {
            std::vector<FormulaCall> new_calls;
            eq.rhs = extract_positional_calls(eq.rhs, eq.lhs_var, new_calls);
            for (auto& c : new_calls)
                formula_calls.push_back(std::move(c));
        }
    }

    void load_with_sections(const std::vector<std::string>& all_lines, const std::string& section) {
        sections_ = split_sections(all_lines);
        if (sections_.size() <= 1 && section.empty())
            load_lines(all_lines);
        else
            load_section(section);
        resolve_positional_calls();
        compute_rewrite_groups();  // regroup after user rules loaded
        trace_loaded();
    }

    void load_string(const std::string& source, const std::string& label = "<inline>",
                     const std::string& section = "") {
        ExprArena::Scope scope(arena);
        if (base_dir.empty()) base_dir = ".";
        if (rewrite_rules.empty()) load_builtins();
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
        ExprArena::Scope scope(arena);
        if (path.empty())
            throw std::runtime_error("No file path provided");
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
            throw std::runtime_error("Path is a directory, not a file: " + path);

        base_dir = std::filesystem::path(path).parent_path().string();
        if (base_dir.empty()) base_dir = ".";
        if (rewrite_rules.empty()) load_builtins();

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

    static bool approx_equal(double a, double b) {
        if (std::isnan(a) || std::isnan(b)) return false;
        if (std::isinf(a) || std::isinf(b)) return a == b;
        double eps = std::max(EPSILON_REL, EPSILON_REL * std::max(std::abs(a), std::abs(b)));
        return std::abs(a - b) < eps;
    }

    std::vector<VerifyResult> verify_variable(
        const std::string& target, double known_value,
        std::map<std::string, double> bindings) const
    {
        ExprArena::Scope scope(arena);
        BudgetGuard budget_guard; // Part C
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
                        double val = solve_recursive(v, b2, {target}, 0, de);
                        resolved = substitute(resolved, v, Expr::Num(val));
                    } catch (const std::runtime_error&) { return; }
                }
            }
            auto computed_opt = evaluate(simplify(resolved));
            if (!computed_opt) return;
            double computed = computed_opt.value();
            if (std::isnan(computed) || std::isinf(computed)) return;
            results.push_back({desc, computed, approx_equal(computed, known_value)});
        };

        auto try_verify_formula = [&](const FormulaCall& call, const std::string& resolve_var,
                                      const std::string& desc) {
            try {
                auto sub_binds = prepare_sub_bindings(call, bindings, {}, 0, target, false);
                auto& sub_sys = load_sub_system(call.file_stem);
                double computed = sub_sys.resolve(resolve_var, sub_binds);
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
    std::map<std::string, ExprPtr> prepare_derive_bindings(
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
    std::string format_derived(const ExprPtr& result) const {
        // format_derived allocates via the arena (fmt_exact_double builds
        // Num nodes; substitute_builtin_constants rewrites the tree). Open
        // our own scope so callers don't have to — scopes nest, and the
        // cost of an extra stack frame is negligible for a one-shot format.
        ExprArena::Scope scope(arena);
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
            if (!std::isinf(val.value())) return fmt_exact_double(val.value());
        } else {
            trace.calc("derive: symbolic result (cannot evaluate)");
        }
        // Recognize constants and fractions in the expression tree
        const auto* recognized = expr_recognize_constants(distributed);
        return expr_to_string(recognized);
    }

    // Derive single result (backwards compatible)
    std::string derive(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings) const {
        ExprArena::Scope scope(arena);
        auto bindings = prepare_derive_bindings(target, numeric_bindings, symbolic_bindings);
        DeadEndSet dead_ends; // Fix 1: per-top-level-query dead-end set
        auto result = derive_recursive(target, bindings, {}, 0, dead_ends);
        if (!result) throw std::runtime_error("Cannot derive equation for '" + target + "'");
        return format_derived(result);
    }

    // Derive ALL results (for multi-valued inversions: abs, quadratic, etc.)
    std::vector<std::string> derive_all(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings) const {
        ExprArena::Scope scope(arena);
        RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &numeric_bindings, &custom_functions_);
        FuncInverterGuard fi_guard(make_func_inverter());
        auto bindings = prepare_derive_bindings(target, numeric_bindings, symbolic_bindings);

        // Collect ALL results from enumerate_candidates
        std::vector<std::string> results;
        std::set<std::string> seen;

        auto add_result = [&](const ExprPtr& result) {
            if (!result) return;
            auto s = format_derived(result);
            if (seen.insert(s).second) results.push_back(s);
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
                add_result(try_derive(c.expr, target, b, {}, 0, dead_ends));
            } else if (c.type == CandidateType::FORMULA_REV) {
                // Unfold sub-system equations and collect all solutions
                // (sub-system load failure → no reverse solutions from this candidate)
                try {
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    std::map<std::string, ExprPtr> parent_map;
                    for (auto& [sv, expr] : c.call->bindings)
                        parent_map[sv] = expr;
                    std::string sub_target = c.sub_var;
                    ExprPtr binding_expr = parent_map[sub_target];

                    for (const auto& eq : sub_sys.equations) {
                        if (eq.lhs_var != c.call->query_var) continue;
                        if (eq.condition && !sub_sys.check_condition(*eq.condition, numeric))
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
                                add_result(try_derive(sol.expr, target, b, {}, 0, dead_ends));
                            } else {
                                auto final_sols = solve_for_all(sol.expr, binding_expr, target);
                                for (auto& fs : final_sols) {
                                    if (!fs.expr) continue;
                                    auto b = bindings;
                                    add_result(try_derive(fs.expr, target, b, {}, 0, dead_ends));
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
                add_result(result);
                return result != nullptr; // stop if found (forward is single-valued)
            }
            return false; // don't stop — collect all
        });

        // If no equation-based results, check iff conditions for constraint inversion.
        // For piecewise functions: "result = 1 iff x > 0" → "x > 0 if result = 1"
        if (results.empty()) {
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

                std::string cond_str = eq.condition
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

                std::string link = exclusive ? " iff " : " if ";
                std::string eq_str = eq.lhs_var + " = " + expr_to_string(rhs_val);
                std::string inverted = body_is_known
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
        ExprArena::Scope scope(arena);

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

    // Build a function inverter that resolves via .fw sub-system definitions.
    // Given f(inner) = rhs, loads f's sub-system and solves for the input variable.
    FuncInverter make_func_inverter() const {
        return [this](const std::string& func_name, const ExprPtr& rhs) -> ExprPtr {
            try {
                auto& sub = load_sub_system(func_name);
                // Find the section with positional args (the function definition)
                for (const auto& sec : sub.sections_) {
                    if (sec.positional_args.empty()) continue;
                    // The input variable is the first positional arg
                    // The return variable is sec.return_var (or "result")
                    std::string input_var = sec.positional_args[0];
                    std::string return_var = sec.return_var.empty() ? "result" : sec.return_var;
                    // Solve: given return_var = rhs, find input_var
                    // Look through the sub-system's equations for one that has input_var on the LHS
                    for (auto& eq : sub.equations) {
                        if (eq.lhs_var == input_var) {
                            // eq: input_var = f(return_var)
                            // Substitute return_var → rhs
                            return simplify(substitute(eq.rhs, return_var, rhs));
                        }
                    }
                    // Try solving algebraically: return_var = g(input_var) → input_var = g⁻¹(rhs)
                    for (auto& eq : sub.equations) {
                        if (eq.lhs_var == return_var && contains_var(eq.rhs, input_var)) {
                            auto result = solve_for(Expr::Var(return_var), eq.rhs, input_var);
                            if (result)
                                return simplify(substitute(result, return_var, rhs));
                        }
                    }
                    break;  // only check first section with positional args
                }
            // NOLINTNEXTLINE(bugprone-empty-catch) — sub-system load or solve failure → no inverse available
            } catch (const std::runtime_error&) {}
            return nullptr;  // no inverse found
        };
    }

    // RAII guard for function inverter thread-local
    struct FuncInverterGuard {
        explicit FuncInverterGuard(FuncInverter fn) { solve_set_func_inverter(std::move(fn)); }
        ~FuncInverterGuard() { solve_set_func_inverter(nullptr); }
    };

    double resolve(const std::string& target,
                   std::map<std::string, double> bindings) const {
        ExprArena::Scope scope(arena);
        BudgetGuard budget_guard; // Part C: initialize budget at top-level entry
        auto prepared = prepare_bindings(target, bindings);
        RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &prepared, &custom_functions_);
        FuncInverterGuard fi_guard(make_func_inverter());
        if (auto it = prepared.find(target); it != prepared.end()) return it->second;
        DeadEndSet dead_ends; // Part A: per-top-level-query dead-end set
        return solve_recursive(target, prepared, {}, 0, dead_ends);
    }

    ValueSet resolve_all(const std::string& target,
                          std::map<std::string, double> bindings) const {
        ExprArena::Scope scope(arena);
        BudgetGuard budget_guard; // Part C: initialize budget at top-level entry
        auto prepared = prepare_bindings(target, bindings);
        RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &prepared, &custom_functions_);
        FuncInverterGuard fi_guard(make_func_inverter());
        if (auto it = prepared.find(target); it != prepared.end())
            return ValueSet::eq(it->second);

        // Try solving for exact values
        std::vector<double> exact_results;
        DeadEndSet dead_ends; // Part A: per-top-level-query dead-end set
        try {
            exact_results = solve_all(target, prepared, {}, 0, dead_ends);

            // Cross-equation validation: verify each candidate against ALL equations
            // For each equation, substitute all known values + candidate,
            // then check LHS == evaluated RHS
            // Only cross-validate when there are multiple equations with known LHS values
            // (single-equation multiple roots are already valid by construction)
            int known_lhs_count = 0;
            for (const auto& eq : equations)
                if (prepared.count(eq.lhs_var)) known_lhs_count++;
            if (exact_results.size() > 1 && known_lhs_count > 1) {
                std::vector<double> validated;
                for (double r : exact_results) {
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
                bool all_exact = true;
                for (double r : exact_results)
                    if (std::abs(r - std::round(r)) > EPSILON_ZERO)
                        { all_exact = false; break; }
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
        for (const auto& gc : global_conditions)
            constraints = constraints.intersect(gc.to_valueset(target, prepared));

        bool has_constraints = !constraints.empty()
            && constraints.to_string() != ValueSet::all().to_string();

        // Combine results: only unite algebraic + ranges when iff constraints contributed
        if (!exact_results.empty() && has_iff_constraints && has_constraints) {
            auto combined = constraints;
            for (double r : exact_results)
                combined = combined.unite(ValueSet::eq(r));
            return combined;
        }
        if (!exact_results.empty())
            return ValueSet::discrete(exact_results);
        if (has_constraints)
            return constraints;

        throw std::runtime_error("Cannot solve for '" + target + "'");
    }

    double resolve_one(const std::string& target,
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
    std::map<std::string, double> prepare_bindings(const std::string& target,
                                                    std::map<std::string, double>& bindings) const {
        trace.step("\nsolving for: " + target);
        for (auto& [k, v] : defaults) {
            if (k != target && !bindings.count(k)) {
                bindings[k] = v;
                trace.step("  using default: " + k + " = " + fmt_num(v));
            }
        }
        if (trace.show_steps() && !bindings.empty()) {
            trace.step("  given:");
            for (auto& [k, v] : bindings)
                trace.step("    " + k + " = " + fmt_num(v));
        }
        return bindings;
    }

    // Like solve_recursive but collects ALL valid results instead of stopping at first
    std::vector<double> solve_all(const std::string& target,
                                   std::map<std::string, double>& bindings,
                                   std::set<std::string> visited, int depth,
                                   DeadEndSet& dead_ends) const {
        if (fwiz_trace_solver() && depth <= MAX_DIAGNOSTIC_SOLVE_DEPTH) {
            std::cerr << "[depth=" << depth << " fn=solve_all target=" << target << "]\n"
                      << "  bindings: " << diag_keyset_str(bindings) << "\n"
                      << "  visited: " << diag_set_str(visited) << "\n"
                      << "  dead_ends: ";
            dump_dead_ends(std::cerr, dead_ends);
            std::cerr << "\n  budget: " << solve_budget_remaining_ << "\n";
        }
        if (auto it = bindings.find(target); it != bindings.end()) {
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=solve_all target=" << target
                          << " exit=bound]\n";
            return {it->second};
        }
        if (visited.count(target)) {
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=solve_all target=" << target
                          << " exit=visited-cycle]\n";
            return {};
        }
        visited.insert(target);

        std::vector<double> results;
        bool had_nan_inf = false;
        std::set<std::string> missing;

        auto try_expr_all = [&](const ExprPtr& expr, const std::string& label,
                                const Condition* cond) {
            auto b = bindings; // copy — each attempt gets fresh bindings
            bool nan_inf = false;
            if (try_resolve(expr, target, b, visited, depth, nan_inf, missing, dead_ends)) {
                double val = b.at(target);
                // Check equation condition
                if (cond && !check_condition(*cond, b)) return;
                // Check global conditions
                for (const auto& gc : global_conditions)
                    if (!check_condition(gc, b)) return;
                // Deduplicate
                for (auto r : results)
                    if (std::abs(r - val) < EPSILON_ZERO) return;
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
            charge_budget(); // Part C: insurance — per-candidate-evaluation
            if (c.type == CandidateType::EXPR) {
                if (winning_expr_group >= 0 && c.source_group != winning_expr_group)
                    return true; // moved to a new source group — stop enumeration
                // Check pre-condition
                if (c.condition && !check_condition(*c.condition, bindings)) return false;
                size_t added = results.size();
                try_expr_all(c.expr, c.desc, c.condition);
                added = results.size() - added;
                if (added > 0 && winning_expr_group < 0)
                    winning_expr_group = c.source_group;
            } else if (c.type == CandidateType::FORMULA_FWD) {
                if (formula_depth_ >= max_formula_depth) return false;
                try {
                    formula_depth_++;
                    struct DepthGuard { ~DepthGuard() { formula_depth_--; } } guard;
                    auto sub_binds = prepare_sub_bindings(*c.call, bindings, visited, depth,
                                                          "", true, &dead_ends);
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    sub_sys.max_formula_depth = max_formula_depth;
                    double val = sub_sys.resolve(c.call->query_var, sub_binds);
                    if (!std::isnan(val) && !std::isinf(val)) {
                        for (auto r : results)
                            if (std::abs(r - val) < EPSILON_ZERO) return false;
                        // Check global conditions
                        auto b = bindings; b[target] = val;
                        for (const auto& gc : global_conditions)
                            if (!check_condition(gc, b)) return false;
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
                for (double val : roots) {
                    if (results.size() >= MAX_NUMERIC_RESULTS) break;
                    bool dup = false;
                    for (auto r : results)
                        if (approx_equal(r, val)) { dup = true; break; }
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
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=solve_all target=" << target
                          << " exit=exhausted missing=" << diag_set_str(missing) << "]\n";
            std::string list;
            for (const auto& v : missing) list += (list.empty() ? "" : ", ") + ("'" + v + "'");
            throw std::runtime_error("Cannot solve for '" + target + "': no value for " + list);
        }
        if (results.empty()) {
            dead_ends.insert({target, bindings_keyset(bindings)});
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=solve_all target=" << target
                          << " exit=no-equation]\n";
            throw std::runtime_error("Cannot solve for '" + target + "'");
        }

        if (fwiz_trace_solver())
            std::cerr << "[depth=" << depth << " fn=solve_all target=" << target
                      << " exit=ok count=" << results.size() << "]\n";
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

    static size_t find_matching_rparen(const std::vector<Token>& tok, size_t lparen_pos) {
        int depth = 1;
        for (size_t i = lparen_pos + 1; i < tok.size(); i++) {
            if (tok[i].type == TokenType::LPAREN) depth++;
            else if (tok[i].type == TokenType::RPAREN) { if (--depth == 0) return i; }
        }
        return std::string::npos;
    }

    static bool has_question_in_range(const std::vector<Token>& tok, size_t from, size_t to) {
        for (size_t i = from; i < to; i++)
            if (tok[i].type == TokenType::QUESTION) return true;
        return false;
    }

    static FormulaCall parse_call_args(const std::vector<Token>& tok, size_t name_pos, size_t rparen_pos) {
        FormulaCall call;
        call.file_stem = tok[name_pos].text;

        // Parse comma-separated args between LPAREN and RPAREN
        size_t i = name_pos + 2; // skip IDENT and LPAREN
        while (i < rparen_pos) {
            // Skip commas
            if (tok[i].type == TokenType::COMMA) { i++; continue; }

            if (tok[i].type != TokenType::IDENT) { i++; continue; }

            // Check for query: IDENT EQUALS QUESTION [IDENT]
            if (i + 2 < rparen_pos
                && tok[i + 1].type == TokenType::EQUALS
                && tok[i + 2].type == TokenType::QUESTION) {
                call.query_var = tok[i].text;
                // Check for alias after ?
                if (i + 3 < rparen_pos && tok[i + 3].type == TokenType::IDENT) {
                    call.output_var = tok[i + 3].text;
                    i += 4;
                } else {
                    call.output_var = call.query_var;
                    i += 3;
                }
            }
            // Check for binding: IDENT EQUALS expr (up to next COMMA or RPAREN)
            else if (i + 1 < rparen_pos && tok[i + 1].type == TokenType::EQUALS) {
                std::string sub_var = tok[i].text;
                // Collect tokens from after = until COMMA or RPAREN
                size_t expr_start = i + 2;
                size_t expr_end = expr_start;
                int pd = 0;
                while (expr_end < rparen_pos) {
                    if (tok[expr_end].type == TokenType::LPAREN) pd++;
                    else if (tok[expr_end].type == TokenType::RPAREN) pd--;
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

        if (call.query_var.empty())
            throw std::runtime_error("Formula call '" + call.file_stem + "' has no query variable (use var=?)");

        return call;
    }

    static std::pair<std::vector<Token>, std::vector<FormulaCall>>
    extract_formula_calls(const std::vector<Token>& tok) {
        // Quick check: any QUESTION inside parens?
        int paren_depth = 0;
        bool has_call = false;
        for (const auto& t : tok) {
            if (t.type == TokenType::LPAREN) paren_depth++;
            else if (t.type == TokenType::RPAREN) paren_depth--;
            else if (t.type == TokenType::QUESTION && paren_depth > 0) { has_call = true; break; }
        }
        if (!has_call) return {tok, {}};

        std::vector<Token> result;
        std::vector<FormulaCall> calls;
        size_t i = 0;

        while (i < tok.size()) {
            if (tok[i].type == TokenType::IDENT
                && i + 1 < tok.size()
                && tok[i + 1].type == TokenType::LPAREN) {
                size_t rparen = find_matching_rparen(tok, i + 1);
                if (rparen != std::string::npos && has_question_in_range(tok, i + 2, rparen)) {
                    auto call = parse_call_args(tok, i, rparen);

                    // Implied alias: if preceded by "IDENT =" and call has no explicit alias
                    if (call.output_var == call.query_var
                        && result.size() >= 2
                        && result[result.size() - 1].type == TokenType::EQUALS
                        && result[result.size() - 2].type == TokenType::IDENT) {
                        call.output_var = result[result.size() - 2].text;
                    }

                    calls.push_back(call);
                    result.push_back(Token{TokenType::IDENT, call.output_var, 0});
                    i = rparen + 1;
                    continue;
                }
            }
            result.push_back(tok[i]);
            i++;
        }

        return {result, calls};
    }

    // Parse a condition string like "x > 0" or "x > 0 && x < 100"
    std::optional<Condition> parse_condition(const std::string& cond_str) {
        if (cond_str.empty()) return std::nullopt;

        Condition cond;
        // Split on && and || — collect clause strings and connectors
        std::vector<std::string> clause_strs;
        std::string remaining = cond_str;
        while (!remaining.empty()) {
            size_t and_pos = remaining.find("&&");
            size_t or_pos = remaining.find("||");
            size_t split = std::min(and_pos, or_pos);

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

            if (op_pos == std::string::npos) continue; // malformed clause, skip

            std::string lhs_str = trim(clause_str.substr(0, op_pos));
            std::string rhs_str = trim(clause_str.substr(op_pos + op_len));

            auto lhs_tok = Lexer(lhs_str).tokenize();
            auto rhs_tok = Lexer(rhs_str).tokenize();
            Parser lhs_p(lhs_tok), rhs_p(rhs_tok);

            cond.clauses.push_back({lhs_p.parse_expr(), rhs_p.parse_expr(), op});
        }

        return cond.clauses.empty() ? std::nullopt : std::optional<Condition>(cond);
    }

    // Check if a condition is satisfied given current bindings
    static bool check_condition(const Condition& cond,
                         const std::map<std::string, double>& bindings) {
        auto eval_clause = [&](const CondClause& c) -> std::optional<bool> {
            // Substitute known bindings into lhs and rhs
            ExprPtr lhs = c.lhs, rhs = c.rhs;
            std::set<std::string> vars;
            collect_vars(lhs, vars);
            collect_vars(rhs, vars);
            auto& consts = builtin_constants();
            for (auto& v : vars) {
                if (auto it = bindings.find(v); it != bindings.end()) {
                    lhs = substitute(lhs, v, Expr::Num(it->second));
                    rhs = substitute(rhs, v, Expr::Num(it->second));
                } else if (consts.count(v)) {
                    // Builtin constant — evaluate() handles it, no substitution needed
                } else {
                    return std::nullopt; // unknown variable — can't evaluate
                }
            }
            auto l_opt = evaluate(*simplify(lhs));
            auto r_opt = evaluate(*simplify(rhs));
            if (!l_opt || !r_opt) return std::nullopt;
            double l = l_opt.value();
            double r = r_opt.value();
            switch (c.op) {
                case CondOp::GT: return l > r;
                case CondOp::GE: return l >= r;
                case CondOp::LT: return l < r;
                case CondOp::LE: return l <= r;
                case CondOp::EQ: return std::abs(l - r) < EPSILON_ZERO;
                case CondOp::NE: return std::abs(l - r) >= EPSILON_ZERO;
                case CondOp::COUNT_: assert(false && "invalid CondOp"); return false;
            }
            return std::nullopt;
        };

        bool result = true;
        for (size_t i = 0; i < cond.clauses.size(); i++) {
            auto val = eval_clause(cond.clauses[i]);
            bool clause_result = !val.has_value() || val.value(); // unknown → true (satisfied)

            if (i == 0) {
                result = clause_result;
            } else {
                auto logic = cond.connectors[i - 1];
                if (logic == CondLogic::AND) result = result && clause_result;
                else                         result = result || clause_result;
            }
        }
        return result;
    }

    void parse_line(const std::string& line) {
        // Split at condition keyword: "if", "iff", or ":" (legacy)
        // Not inside parentheses. Optional comma before if/iff.
        std::string eq_part = line;
        std::string cond_part;
        bool is_iff = false;
        {
            int pd = 0;
            for (size_t i = 0; i < line.size(); i++) {
                char ch = line[i];
                if (ch == '(') { pd++; continue; }
                if (ch == ')') { pd--; continue; }
                if (pd != 0) continue;

                // "iff " keyword (must be preceded by space or comma)
                if (ch == 'i' && i + 3 < line.size()
                    && line[i+1] == 'f' && line[i+2] == 'f' && line[i+3] == ' '
                    && (i == 0 || line[i-1] == ' ' || line[i-1] == ',')) {
                    eq_part = line.substr(0, i);
                    while (!eq_part.empty() && (eq_part.back() == ' ' || eq_part.back() == ','))
                        eq_part.pop_back();
                    cond_part = line.substr(i + 4);
                    is_iff = true;
                    break;
                }

                // "if " keyword (preceded by space/comma, not followed by 'f')
                if (ch == 'i' && i + 2 < line.size()
                    && line[i+1] == 'f' && line[i+2] == ' '
                    && (i == 0 || line[i-1] == ' ' || line[i-1] == ',')) {
                    eq_part = line.substr(0, i);
                    while (!eq_part.empty() && (eq_part.back() == ' ' || eq_part.back() == ','))
                        eq_part.pop_back();
                    cond_part = line.substr(i + 3);
                    break;
                }
            }
        }

        // Check for standalone global condition vs equation
        // An equation has "ident = expr" where = is not part of >=, <=, !=
        bool is_equation = false;
        for (size_t ci = 0; ci < eq_part.size(); ci++) {
            if (eq_part[ci] == '=') {
                bool part_of_cmp = (ci > 0 && (eq_part[ci-1] == '>' || eq_part[ci-1] == '<' || eq_part[ci-1] == '!'));
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

        // Extract formula calls before expression parsing
        auto [mod_tok, calls] = extract_formula_calls(tok);
        for (auto& c : calls) formula_calls.push_back(std::move(c));

        // Standalone formula call: just "output_var END" after extraction
        if (mod_tok.size() <= 2) return;

        // Find the '=' token (not part of >=, <=, !=, ==)
        size_t eq_pos = 0;
        for (size_t i = 0; i < mod_tok.size(); i++) {
            if (mod_tok[i].type == TokenType::EQUALS) { eq_pos = i; break; }
        }
        if (eq_pos == 0) return;  // no '=' found

        // Simple equation: "var = expr" (IDENT followed by EQUALS)
        bool simple_lhs = (eq_pos == 1 && mod_tok[0].type == TokenType::IDENT);

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
            std::string desc = eq_part;
            std::string cond = (is_iff && !cond_part.empty()) ? trim(cond_part) : "";
            rewrite_rules.push_back({lhs_expr, rhs_expr, desc, cond, is_undefined(rhs_expr)});
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
        equations.push_back({lhs, p.parse_expr(), std::move(cond), is_iff});
    }

    // --- Sub-system loading ---

    const FormulaSystem& load_sub_system(const std::string& file_stem) const {
        // Split dotted names: "geometry.rectangle" → file="geometry", section="rectangle"
        std::string file_part = file_stem;
        std::string section;
        size_t dot = file_stem.find('.');
        if (dot != std::string::npos) {
            file_part = file_stem.substr(0, dot);
            section = file_stem.substr(dot + 1);
        }

        // Check custom and builtin function definitions
        auto& builtins = builtin_function_defs();
        auto blt = custom_function_defs_.find(file_part);
        if (blt == custom_function_defs_.end()) {
            auto bit = builtins.find(file_part);
            if (bit != builtins.end()) blt = custom_function_defs_.end(); // use builtins below
        }
        // Check builtins if not in custom
        const std::string* def_source = nullptr;
        if (blt != custom_function_defs_.end())
            def_source = &blt->second;
        else if (auto bit = builtins.find(file_part); bit != builtins.end())
            def_source = &bit->second;

        std::string path = base_dir + "/" + file_part;
        if (path.find('.') == std::string::npos) path += ".fw";
        std::string abs_path;
        try { abs_path = std::filesystem::weakly_canonical(path).string(); }
        catch (const std::filesystem::filesystem_error&) { abs_path = path; }

        // Cache key: defined functions use name directly, files use abs path
        std::string cache_key = def_source
            ? ("@def:" + file_part)
            : (abs_path + (section.empty() ? "" : "#" + section));
        auto it = sub_systems.find(cache_key);
        if (it != sub_systems.end()) return *it->second;

        auto sub = std::make_shared<FormulaSystem>();
        sub->trace = trace;
        sub->numeric_mode = numeric_mode;
        sub->custom_functions_ = custom_functions_;  // propagate to sub-systems

        // Try loading from file first; fall back to embedded definition
        bool loaded = false;
        if (!def_source) {
            sub->load_file(abs_path, section);
        } else {
            // Try file first (user can override definitions)
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
                sub->trace = trace;
                sub->numeric_mode = numeric_mode;
                sub->custom_functions_ = custom_functions_;
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
                        double val = solve_recursive(v, parent_bindings, visited, depth + 1, de);
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
                    double val = solve_recursive(call.output_var, parent_bindings, visited, depth + 1, de);
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
            int eq_group = next_group++;  // one group per source equation
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
        for (size_t i = 0; i < equations.size(); i++)
            for (size_t j = i + 1; j < equations.size(); j++) {
                if (equations[i].lhs_var != equations[j].lhs_var) continue;
                // Skip if both equations have different conditions — their domains
                // may not overlap (e.g., x>=0 and x<0 in piecewise abs)
                if (equations[i].condition && equations[j].condition) continue;
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
                    int pair_group = next_group++; // one group per (i,j,direction)
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
        for (auto& call : formula_calls)
            for (auto& [sub_var, expr] : call.bindings)
                if (contains_var(expr, target))
                    if (handler(Candidate{CandidateType::FORMULA_REV, nullptr,
                        target + " via " + call.file_stem + "(" + std::string(sub_var) + ")",
                        &call, sub_var, nullptr, next_group++}))
                        return;

        // Strategy 7: cross-equation variable elimination
        // For target T in equation E1 with unknown U, find E2 that can express U.
        // Substitute U into E1, then solve for T. If the result still contains
        // another unknown V, try a second elimination from remaining equations.
        if (equations.size() >= 2) {
        for (size_t i = 0; i < equations.size(); i++) {
            auto& e1 = equations[i];
            if (!contains_var(e1.rhs, target)) continue;
            std::set<std::string> e1_vars;
            collect_vars(e1.rhs, e1_vars);
            for (auto& u : e1_vars) {
                if (u == target) continue;
                if (sub_bindings && sub_bindings->count(u)) continue;
                if (is_active_builtin(u)) continue;
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
                            int sub_group = next_group++; // one group per elim source
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
        } // equations.size() >= 2

        // Strategy 6: numeric root-finding (--numeric only)
        if (numeric_mode) {
            for (auto& eq : equations) {
                if (eq.lhs_var != target && !contains_var(eq.rhs, target)) continue;
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
    static ExprPtr substitute_bindings(ExprPtr expr, const MapType& bindings,
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

    ExprPtr try_derive(const ExprPtr& expr, const std::string& target,
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
            trace.step("  assuming: " + a.desc + (a.inherent ? " (inherent)" : ""), depth + 1);

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

    ExprPtr derive_recursive(const std::string& target,
                             std::map<std::string, ExprPtr>& bindings,
                             std::set<std::string> visited, int depth,
                             DeadEndSet& dead_ends) const {
        // Diagnostic: hard cap to prevent true-infinite recursion during diagnosis.
        // Only enforced when FWIZ_TRACE_SOLVER is set.
        if (fwiz_trace_solver() && depth > MAX_DIAGNOSTIC_SOLVE_DEPTH)
            throw std::runtime_error("Max solve depth 100 reached (diagnostic cap)");
        if (fwiz_trace_solver() && depth <= MAX_DIAGNOSTIC_SOLVE_DEPTH) {
            std::cerr << "[depth=" << depth << " fn=derive_recursive target=" << target << "]\n"
                      << "  bindings: {";
            bool first = true;
            for (auto& [k, _] : bindings) {
                if (!first) std::cerr << ",";
                std::cerr << k;
                first = false;
            }
            std::cerr << "}\n  visited: " << diag_set_str(visited) << "\n"
                      << "  dead_ends: ";
            dump_dead_ends(std::cerr, dead_ends);
            std::cerr << "\n";
        }
        if (auto it = bindings.find(target); it != bindings.end()) {
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=derive_recursive target=" << target
                          << " exit=bound]\n";
            return it->second;
        }
        if (is_active_builtin(target)) {
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=derive_recursive target=" << target
                          << " exit=builtin]\n";
            return Expr::Var(target);
        }
        if (visited.count(target)) {
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=derive_recursive target=" << target
                          << " exit=visited-cycle]\n";
            return nullptr;
        }
        // Fix 1: pre-filter — skip if a sibling in this top-level derive
        // already discovered (target, current-bindings-keyset) is a dead-end.
        auto dead_key = std::make_pair(target, bindings_keyset(bindings));
        if (dead_ends.count(dead_key)) {
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=derive_recursive target=" << target
                          << " exit=dead-end-hit]\n";
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
                            if (!sub_sys.check_condition(*eq.condition, cond_binds))
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
                        bool has_formula_output = false;
                        for (const auto& fc : sub_sys.formula_calls)
                            if (remaining.count(fc.output_var))
                                { has_formula_output = true; break; }
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
                    std::string sub_target = c.sub_var;
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
                            if (!sub_sys.check_condition(*eq.condition, cond_binds))
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
        if (fwiz_trace_solver())
            std::cerr << "[depth=" << depth << " fn=derive_recursive target=" << target
                      << " exit=" << (found ? "found" : "exhausted") << "]\n";
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
    double resolve_memoized(const std::string& target,
                            std::map<std::string, double> bindings,
                            DeadEndSet* dead_ends = nullptr) const {
        // Build cache key: target + sorted bindings
        std::string key = target;
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
    std::vector<double> try_resolve_numeric(
            const ExprPtr& combined, const std::string& target,
            std::map<std::string, double>& bindings,
            const std::set<std::string>& visited, int depth,
            const Condition* eq_condition,
            DeadEndSet& dead_ends) const {
        charge_budget(); // Part C: insurance
        if (fwiz_trace_solver() && depth <= MAX_DIAGNOSTIC_SOLVE_DEPTH) {
            std::cerr << "[depth=" << depth << " fn=try_resolve_numeric target=" << target
                      << " expr=" << diag_expr_preview(combined) << "]\n"
                      << "  bindings: " << diag_keyset_str(bindings) << "\n"
                      << "  visited: " << diag_set_str(visited) << "\n"
                      << "  dead_ends: ";
            dump_dead_ends(std::cerr, dead_ends);
            std::cerr << "\n  budget: " << solve_budget_remaining_ << "\n";
        }

        // Re-entrance guard: prevent infinite recursion on coupled systems
        static thread_local std::set<std::string> numeric_active_;
        if (numeric_active_.count(target)) return {};
        numeric_active_.insert(target);
        struct NumericGuard { const std::string& t; std::set<std::string>& s;
            ~NumericGuard() { s.erase(t); } } ng_{target, numeric_active_};

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
                    const auto& visited_copy = visited;
                    double val = solve_recursive(v, bindings, visited_copy, depth + 1, dead_ends);
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
        bool has_target = contains_var(expr, target);

        // Heuristic: try integer mode if bounds are reasonable integers
        bool try_integer = (lo >= -10000 && hi <= 10000
            && std::floor(lo) == lo && std::floor(hi) == hi
            && (hi - lo) <= 20000);

        std::vector<double> roots;

        if (has_target && !has_formula_vars) {
            // Equation-based: f(target) = combined_expr(target) = 0
            auto f = [&, expr](double x) -> double {
                ExprPtr subst = substitute(expr, target, Expr::Num(x));
                return evaluate(*simplify(subst)).value_or_nan();
            };

            if (try_integer) {
                roots = find_numeric_roots(f, lo, hi, true, numeric_samples);
                if (!roots.empty()) goto filter;
            }
            roots = find_numeric_roots(f, lo, hi, false, numeric_samples);
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
            for (auto& [bvar, bval] : bindings) {
                if (bvar == target) continue;
                bool found = false;
                for (const auto& pv : probe_vars) if (pv == bvar) { found = true; break; }
                if (!found) probe_vars.push_back(bvar);
            }

            // Suppress trace during probe scans — each probe point calls
            // resolve_memoized which triggers full solve_recursive traces.
            // With 200+ scan points this produces enormous --steps output.
            auto saved_trace = trace.level;
            trace.level = TraceLevel::NONE;
            struct TraceGuard { Trace& t; TraceLevel l; ~TraceGuard() { t.level = l; } } tg_{trace, saved_trace};

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
                        double computed = resolve_memoized(probe_var, test_binds, &dead_ends);
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

        if (fwiz_trace_solver())
            std::cerr << "[depth=" << depth << " fn=try_resolve_numeric target=" << target
                      << " exit=empty]\n";
        return {};

        filter:
        // Filter by equation condition and global conditions
        std::vector<double> filtered;
        for (double r : roots) {
            auto test_binds = bindings;
            test_binds[target] = r;
            bool ok = true;
            if (eq_condition && !check_condition(*eq_condition, test_binds)) ok = false;
            for (const auto& gc : global_conditions)
                if (!check_condition(gc, test_binds)) ok = false;
            if (ok) filtered.push_back(r);
        }
        if (fwiz_trace_solver())
            std::cerr << "[depth=" << depth << " fn=try_resolve_numeric target=" << target
                      << " exit=roots count=" << filtered.size() << "]\n";
        return filtered;
    }

    // --- Solver ---

    double solve_recursive(const std::string& target,
                           std::map<std::string, double>& bindings,
                           std::set<std::string> visited, int depth,
                           DeadEndSet& dead_ends) const {
        // Diagnostic: hard cap to prevent true-infinite recursion during diagnosis.
        // Only enforced when FWIZ_TRACE_SOLVER is set, so legitimate deep chains
        // (e.g. 500-eq chain tests) don't break during normal test runs.
        if (fwiz_trace_solver() && depth > MAX_DIAGNOSTIC_SOLVE_DEPTH)
            throw std::runtime_error("Max solve depth 100 reached (diagnostic cap)");
        if (fwiz_trace_solver() && depth <= MAX_DIAGNOSTIC_SOLVE_DEPTH) {
            std::cerr << "[depth=" << depth << " fn=solve_recursive target=" << target << "]\n"
                      << "  bindings: " << diag_keyset_str(bindings) << "\n"
                      << "  visited: " << diag_set_str(visited) << "\n"
                      << "  dead_ends: ";
            dump_dead_ends(std::cerr, dead_ends);
            std::cerr << "\n  budget: " << solve_budget_remaining_ << "\n";
        }
        if (auto it = bindings.find(target); it != bindings.end()) {
            trace.calc("known: " + target + " = " + fmt_num(it->second), depth + 1);
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=solve_recursive target=" << target
                          << " exit=bound result=" << it->second << "]\n";
            return it->second;
        }
        if (is_active_builtin(target)) {
            double val = builtin_constants().at(target);
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
                throw std::runtime_error("Maximum formula call depth exceeded (possible infinite recursion)");
            trace.step("formula call: " + call.file_stem + "(" + resolve_var + ")", depth + 1);
            try {
                formula_depth_++;
                struct DepthGuard { ~DepthGuard() { formula_depth_--; } } guard;
                auto sub_binds = prepare_sub_bindings(call, bindings, visited, depth, skip_var,
                                                      true, &dead_ends);
                auto& sub_sys = load_sub_system(call.file_stem);
                sub_sys.max_formula_depth = max_formula_depth;
                for (auto& [sv, val] : sub_binds)
                    trace.calc("  binding: " + sv + " = " + fmt_num(val), depth + 2);

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
                                trace.step("  @extern " + sec.extern_func
                                    + "(" + fmt_num(ait->second) + ") = "
                                    + fmt_num(result), depth + 2);
                                used_extern = true;
                                break;
                            }
                        }
                    }
                }
                if (!used_extern)
                    result = sub_sys.resolve(resolve_var, sub_binds);
                if (std::isnan(result) || std::isinf(result)) { had_nan_inf = true; return false; }
                trace.step("  result: " + target + " = " + fmt_num(result), depth + 1);
                bindings[target] = result;
                return true;
            } catch (const std::runtime_error& e) {
                std::string msg = e.what();
                if (msg.find("depth") != std::string::npos) throw; // propagate depth guard
                trace.step("  failed: " + msg, depth + 2);
                return false;
            }
        };

        bool solved = false;
        enumerate_candidates(target, [&](const Candidate& c) {
            charge_budget(); // Part C: insurance — per-candidate-evaluation
            if (fwiz_trace_solver() && depth <= MAX_DIAGNOSTIC_SOLVE_DEPTH) {
                const char* tname = "?";
                switch (c.type) {
                    case CandidateType::EXPR: tname = "EXPR"; break;
                    case CandidateType::FORMULA_FWD: tname = "FORMULA_FWD"; break;
                    case CandidateType::FORMULA_REV: tname = "FORMULA_REV"; break;
                    case CandidateType::NUMERIC: tname = "NUMERIC"; break;
                    case CandidateType::COUNT_: break;
                }
                std::cerr << "  [depth=" << depth << " solve_recursive candidate type=" << tname
                          << " group=" << c.source_group
                          << " expr=" << diag_expr_preview(c.expr) << "]\n";
            }
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
                for (const auto& gc : global_conditions) {
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
            if (fwiz_trace_solver())
                std::cerr << "[depth=" << depth << " fn=solve_recursive target=" << target
                          << " exit=solved result=" << bindings.at(target) << "]\n";
            return bindings.at(target);
        }

        // Part A: record dead-end before propagating the failure — sibling
        // candidates in the outer query won't redundantly re-try the same
        // free vars with the same bindings.
        dead_ends.insert(dead_key);
        if (fwiz_trace_solver())
            std::cerr << "[depth=" << depth << " fn=solve_recursive target=" << target
                      << " exit=exhausted missing=" << diag_set_str(missing) << "]\n";

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

    bool try_resolve(const ExprPtr& expr, const std::string& target,
                     std::map<std::string, double>& bindings,
                     std::set<std::string> visited, int depth, // NOLINT(performance-unnecessary-value-param) — intentional copy per branch
                     bool& had_nan_inf, std::set<std::string>& missing,
                     DeadEndSet& dead_ends) const {
        charge_budget(); // Part C: insurance — should never trip given Part A
        if (fwiz_trace_solver() && depth <= MAX_DIAGNOSTIC_SOLVE_DEPTH) {
            std::cerr << "[depth=" << depth << " fn=try_resolve target=" << target
                      << " expr=" << diag_expr_preview(expr) << "]\n"
                      << "  bindings: " << diag_keyset_str(bindings) << "\n"
                      << "  dead_ends: ";
            dump_dead_ends(std::cerr, dead_ends);
            std::cerr << "\n  budget: " << solve_budget_remaining_ << "\n";
        }
        // Resolve all free variables in the expression
        std::set<std::string> vars;
        collect_vars(expr, vars);

        ExprPtr resolved = expr;
        for (auto& v : vars) {
            if (v == target) return false;
            if (auto it = bindings.find(v); it != bindings.end()) {
                trace.calc("substitute " + v + " = " + fmt_num(it->second), depth + 2);
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
                    double val = solve_recursive(v, bindings, visited, depth + 1, dead_ends);
                    trace.calc("substitute " + v + " = " + fmt_num(val), depth + 2);
                    resolved = substitute(resolved, v, Expr::Num(val));
                } catch (const SolveBudgetExceededError&) { throw; }
                catch (const std::runtime_error& e) {
                    if (std::string(e.what()).find("depth") != std::string::npos) throw;
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
            trace.step("  assuming: " + a.desc + (a.inherent ? " (inherent)" : ""), depth + 2);
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
        double result = result_opt.value();
        if (std::isinf(result)) {
            trace.step("result is inf, trying alternatives", depth + 1);
            had_nan_inf = true;
            return false;
        }
        trace.step("result: " + target + " = " + fmt_num(result), depth + 1);
        bindings[target] = result;
        if (fwiz_trace_solver())
            std::cerr << "[depth=" << depth << " fn=try_resolve target=" << target
                      << " exit=ok result=" << result << "]\n";
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
    std::map<std::string, std::string> symbolic; // formula_var -> output_name (derive mode)
};

inline CLIQuery parse_cli_query(const std::string& input,
                                bool allow_no_queries = false,
                                bool allow_symbolic = false) {
    CLIQuery q;

    size_t lparen = input.find('(');
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
        size_t dot = q.filename.find('.');
        if (dot == std::string::npos) {
            q.filename += ".fw";
        } else {
            std::string after_dot = q.filename.substr(dot + 1);
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
      for (size_t i = lparen + 1; i < input.size(); i++) {
          if (input[i] == '(') depth++;
          else if (input[i] == ')') { if (--depth == 0) { rparen = i; break; } }
      }
    }
    if (rparen == std::string::npos)
        throw std::runtime_error("Missing closing parenthesis");

    // Capture inline source (text after closing paren)
    if (rparen + 1 < input.size()) {
        std::string after = trim(input.substr(rparen + 1));
        if (!after.empty()) {
            q.inline_source = after;
            // If there was a "name" before (, it's a section selector, not a filename
            if (!q.filename.empty()) {
                // Strip .fw suffix if it was auto-added
                std::string raw = input.substr(0, lparen);
                q.section = raw;
                q.filename.clear();
            }
        }
    }

    // Split arguments by comma (respecting nested parens)
    std::vector<std::string> args;
    { int depth = 0; size_t start = lparen + 1;
      for (size_t i = start; i <= rparen; i++) {
          if (input[i] == '(') depth++;
          else if (input[i] == ')') depth--;
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

        size_t eq = arg.find('=');
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

        if (val.size() >= 1 && val[0] == '?') {
            // Query: "x=?" or "x=?!" or "x=?alias" or "x=?!alias"
            bool strict = false;
            std::string rest = val.substr(1);
            if (!rest.empty() && rest[0] == '!') {
                strict = true;
                rest = rest.substr(1);
            }
            std::string alias = rest.empty() ? name : trim(rest);
            q.queries.push_back({name, alias, strict});
        } else if (val.empty()) {
            throw std::runtime_error("Missing value for '" + name + "'");
        } else {
            double v = 0;
            size_t pos = 0;
            try { v = std::stod(val, &pos); }
            catch (const std::invalid_argument&) { pos = 0; }
            catch (const std::out_of_range&) { pos = 0; }
            if (pos != val.size()) {
                // Try parsing as expression (e.g. "10*2^3", "sqrt(2)")
                bool ok = false;
                try {
                    ExprArena temp_arena;
                    ExprArena::Scope scope(temp_arena);
                    auto expr = Parser(Lexer(val).tokenize()).parse_expr();
                    if (auto val_opt = evaluate(*simplify(expr))) {
                        v = val_opt.value();
                        ok = true;
                    }
                // NOLINTNEXTLINE(bugprone-empty-catch) — parser failure (malformed expression) handled by the !ok branch below
                } catch (const std::runtime_error&) {}
                if (!ok) {
                    if (allow_symbolic) {
                        q.symbolic[name] = val;
                        continue;
                    }
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
