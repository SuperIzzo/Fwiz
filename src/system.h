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
            auto& c = clauses[i];
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
            double val;
            try { val = evaluate(*simplify(resolved)); }
            catch (...) { return ValueSet::all(); }

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

            if (i == 0 || (i > 0 && connectors[i-1] == CondLogic::AND))
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

    std::string base_dir;
    Trace trace;
    mutable int max_formula_depth = 1000;
    mutable bool numeric_mode = false;
    int numeric_samples = NUMERIC_DEFAULT_SAMPLES;
    int fit_depth = FIT_DEFAULT_DEPTH;
    static inline thread_local int formula_depth_ = 0;
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
        for (auto& line : all_lines) {
            auto trimmed = trim(line);
            // Section header
            if (trimmed.size() >= 3 && trimmed.front() == '[' && trimmed.back() == ']'
                && trimmed.find('=') == std::string::npos) {
                auto sec = parse_section_header(trimmed);
                if (!sec.name.empty()) {
                    sec.lines = {};
                    result.push_back(std::move(sec));
                    continue;
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
        for (auto& raw : lines) {
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
        for (auto& s : sections_)
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
        for (auto& ancestor : chain) {
            bool found = false;
            for (auto& s : sections_) {
                if (s.name == ancestor) {
                    load_lines(s.lines);
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
        for (auto& eq : equations)
            if (eq.lhs_var == name) return false;
        return true;
    }

    void trace_loaded() const {
        if (!trace.show_steps()) return;
        for (auto& eq : equations)
            trace.step("  equation: " + eq.lhs_var + " = " + expr_to_string(eq.rhs));
        for (auto& [k, v] : defaults)
            trace.step("  default: " + k + " = " + fmt_num(v));
        for (auto& fc : formula_calls)
            trace.step("  formula call: " + fc.file_stem + "(" + fc.query_var + "=?" + fc.output_var + ")");
    }

    // Read all lines from a stream, stripping BOM from first line
    static std::vector<std::string> read_all_lines(std::istream& in) {
        std::vector<std::string> lines;
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first) { first = false; strip_bom(line); }
            lines.push_back(line);
        }
        return lines;
    }

    // Load lines with section selection (shared by load_file and load_string)
    // Built-in rewrite rules — loaded automatically, replace hardcoded C++ simplifier rules.
    // These are the .fw equivalents; the file examples/builtin.fw mirrors this for documentation.
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
x^0.5 = sqrt(x)
(x^a)^b = x^(a*b)
)";

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
                auto& rule = rewrite_rules[idx];
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
                } catch (...) { all_have_conditions = false; }
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

    void load_with_sections(const std::vector<std::string>& all_lines, const std::string& section) {
        sections_ = split_sections(all_lines);
        if (sections_.size() <= 1 && section.empty())
            load_lines(all_lines);
        else
            load_section(section);
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
                        double val = solve_recursive(v, b2, {target}, 0);
                        resolved = substitute(resolved, v, Expr::Num(val));
                    } catch (...) { return; }
                }
            }
            try {
                double computed = evaluate(simplify(resolved));
                if (std::isnan(computed) || std::isinf(computed)) return;
                results.push_back({desc, computed, approx_equal(computed, known_value)});
            } catch (...) { return; }
        };

        auto try_verify_formula = [&](const FormulaCall& call, const std::string& resolve_var,
                                      const std::string& desc) {
            try {
                auto sub_binds = prepare_sub_bindings(call, bindings, {}, 0, target, false);
                auto& sub_sys = load_sub_system(call.file_stem);
                double computed = sub_sys.resolve(resolve_var, sub_binds);
                if (!std::isnan(computed) && !std::isinf(computed))
                    results.push_back({desc, computed, approx_equal(computed, known_value)});
            } catch (...) { return; }
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

    // Format a derived ExprPtr as a string (evaluate if fully numeric)
    std::string format_derived(const ExprPtr& result) const {
        try {
            double val = evaluate(result);
            if (!std::isnan(val) && !std::isinf(val)) return fmt_num(val);
        } catch (const std::exception& ex) {
            trace.calc("derive: symbolic result (cannot evaluate: " + std::string(ex.what()) + ")");
        }
        return expr_to_string(result);
    }

    // Derive single result (backwards compatible)
    std::string derive(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings) const {
        ExprArena::Scope scope(arena);
        auto bindings = prepare_derive_bindings(target, numeric_bindings, symbolic_bindings);
        auto result = derive_recursive(target, bindings, {}, 0);
        if (!result) throw std::runtime_error("Cannot derive equation for '" + target + "'");
        return format_derived(result);
    }

    // Derive ALL results (for multi-valued inversions: abs, quadratic, etc.)
    std::vector<std::string> derive_all(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings) const {
        ExprArena::Scope scope(arena);
        RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &numeric_bindings);
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
            try { numeric[k] = evaluate(*v); } catch (...) {}
        }

        enumerate_candidates(target, [&](const Candidate& c) {
            if (c.condition && !check_condition(*c.condition, numeric)) return false;

            if (c.type == CandidateType::EXPR) {
                auto b = bindings;
                add_result(try_derive(c.expr, target, b, {}, 0));
            } else if (c.type == CandidateType::FORMULA_REV) {
                // Unfold sub-system equations and collect all solutions
                try {
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    std::map<std::string, ExprPtr> parent_map;
                    for (auto& [sv, expr] : c.call->bindings)
                        parent_map[sv] = expr;
                    std::string sub_target = c.sub_var;
                    ExprPtr binding_expr = parent_map[sub_target];

                    for (auto& eq : sub_sys.equations) {
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
                                add_result(try_derive(sol.expr, target, b, {}, 0));
                            } else {
                                auto final_sols = solve_for_all(sol.expr, binding_expr, target);
                                for (auto& fs : final_sols) {
                                    if (!fs.expr) continue;
                                    auto b = bindings;
                                    add_result(try_derive(fs.expr, target, b, {}, 0));
                                }
                            }
                        }
                    }
                } catch (...) {}
            } else if (c.type == CandidateType::FORMULA_FWD) {
                // Forward formula call — derive into sub-system
                auto b = bindings;
                auto result = derive_recursive(target, b, {}, 0);
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

                ExprPtr rhs_val = substitute_bindings(eq.rhs, bindings, target);
                bool matches = true;
                if (auto it = bindings.find(eq.lhs_var); it != bindings.end()) {
                    try {
                        double lhs_num = evaluate(*it->second);
                        double rhs_num = evaluate(*rhs_val);
                        if (!approx_equal(lhs_num, rhs_num)) matches = false;
                    } catch (...) {}
                }
                if (!matches) continue;

                std::string cond_str = eq.condition->to_valueset(target, {}).to_string();
                bool body_is_known = false;
                if (auto it = bindings.find(eq.lhs_var); it != bindings.end()) {
                    try { evaluate(*it->second); body_is_known = true; } catch (...) {}
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
                    try {
                        if (expr_equal(simplify(this_rhs), simplify(other_rhs))) {
                            exclusive = false; break;
                        }
                        // Also check numeric equality
                        double a = evaluate(*this_rhs), b = evaluate(*other_rhs);
                        if (approx_equal(a, b)) { exclusive = false; break; }
                    } catch (...) {}
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
        std::map<std::string, double> bounds_bindings = numeric_bindings;
        auto [lo, hi] = extract_bounds(bind_key, bounds_bindings);

        // Build evaluation lambda
        auto f = [&](double x) -> double {
            try {
                auto binds = numeric_bindings;
                binds[bind_key] = x;
                return resolve(target, binds);
            } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
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

    double resolve(const std::string& target,
                   std::map<std::string, double> bindings) const {
        ExprArena::Scope scope(arena);
        auto prepared = prepare_bindings(target, bindings);
        RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &prepared);
        if (auto it = prepared.find(target); it != prepared.end()) return it->second;
        return solve_recursive(target, prepared, {}, 0);
    }

    ValueSet resolve_all(const std::string& target,
                          std::map<std::string, double> bindings) const {
        ExprArena::Scope scope(arena);
        auto prepared = prepare_bindings(target, bindings);
        RewriteRulesGuard rr_guard(&rewrite_rules, &rewrite_exhaustive_flags_, &prepared);
        if (auto it = prepared.find(target); it != prepared.end())
            return ValueSet::eq(it->second);

        // Try solving for exact values
        std::vector<double> exact_results;
        try {
            exact_results = solve_all(target, prepared, {}, 0);

            // Cross-equation validation: verify each candidate against ALL equations
            // For each equation, substitute all known values + candidate,
            // then check LHS == evaluated RHS
            // Only cross-validate when there are multiple equations with known LHS values
            // (single-equation multiple roots are already valid by construction)
            int known_lhs_count = 0;
            for (auto& eq : equations)
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
                        try {
                            double computed = evaluate(*simplify(
                                substitute_bindings(eq.rhs, test)));
                            if (!std::isfinite(computed)) continue;
                            if (!approx_equal(computed, lhs_it->second)) {
                                valid = false; break;
                            }
                        } catch (...) {}
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
        } catch (...) {}

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
                    try {
                        double rhs_val = evaluate(*substitute_bindings(eq.rhs, prepared, target));
                        if (approx_equal(it->second, rhs_val)) {
                            // Equation body matches — condition constrains target
                            auto cond_vs = eq.condition->to_valueset(target, prepared);
                            constraints = constraints.intersect(cond_vs);
                        }
                    } catch (...) {}
                }
            }
        }
        for (auto& gc : global_conditions)
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
                                   std::set<std::string> visited, int depth) const {
        if (auto it = bindings.find(target); it != bindings.end())
            return {it->second};
        if (visited.count(target)) return {};
        visited.insert(target);

        std::vector<double> results;
        bool had_nan_inf = false;
        std::set<std::string> missing;

        auto try_expr_all = [&](const ExprPtr& expr, const std::string& label,
                                const Condition* cond) {
            auto b = bindings; // copy — each attempt gets fresh bindings
            bool nan_inf = false;
            if (try_resolve(expr, target, b, visited, depth, nan_inf, missing)) {
                double val = b.at(target);
                // Check equation condition
                if (cond && !check_condition(*cond, b)) return;
                // Check global conditions
                for (auto& gc : global_conditions)
                    if (!check_condition(gc, b)) return;
                // Deduplicate
                for (auto r : results)
                    if (std::abs(r - val) < EPSILON_ZERO) return;
                results.push_back(val);
            }
            if (nan_inf) had_nan_inf = true;
        };

        enumerate_candidates(target, [&](const Candidate& c) {
            if (c.type == CandidateType::EXPR) {
                // Check pre-condition
                if (c.condition && !check_condition(*c.condition, bindings)) return false;
                try_expr_all(c.expr, c.desc, c.condition);
            } else if (c.type == CandidateType::FORMULA_FWD) {
                if (formula_depth_ >= max_formula_depth) return false;
                try {
                    formula_depth_++;
                    struct DepthGuard { ~DepthGuard() { formula_depth_--; } } guard;
                    auto sub_binds = prepare_sub_bindings(*c.call, bindings, visited, depth);
                    auto& sub_sys = load_sub_system(c.call->file_stem);
                    sub_sys.max_formula_depth = max_formula_depth;
                    double val = sub_sys.resolve(c.call->query_var, sub_binds);
                    if (!std::isnan(val) && !std::isinf(val)) {
                        for (auto r : results)
                            if (std::abs(r - val) < EPSILON_ZERO) return false;
                        // Check global conditions
                        auto b = bindings; b[target] = val;
                        for (auto& gc : global_conditions)
                            if (!check_condition(gc, b)) return false;
                        results.push_back(val);
                    }
                } catch (...) {}
            } else if (c.type == CandidateType::NUMERIC) {
                // Cap numeric contributions to prevent explosion with trig equations
                constexpr size_t MAX_NUMERIC_RESULTS = 50;
                if (results.size() >= MAX_NUMERIC_RESULTS) return false;
                auto roots = try_resolve_numeric(c.expr, target, bindings,
                    visited, depth, c.condition);
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
            std::string list;
            for (auto& v : missing) list += (list.empty() ? "" : ", ") + ("'" + v + "'");
            throw std::runtime_error("Cannot solve for '" + target + "': no value for " + list);
        }
        if (results.empty())
            throw std::runtime_error("Cannot solve for '" + target + "'");

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
                std::vector<Token> expr_tok(tok.begin() + expr_start, tok.begin() + expr_end);
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

    std::pair<std::vector<Token>, std::vector<FormulaCall>>
    extract_formula_calls(const std::vector<Token>& tok) {
        // Quick check: any QUESTION inside parens?
        int paren_depth = 0;
        bool has_call = false;
        for (auto& t : tok) {
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
    bool check_condition(const Condition& cond,
                         const std::map<std::string, double>& bindings) const {
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
            try {
                double l = evaluate(*simplify(lhs));
                double r = evaluate(*simplify(rhs));
                switch (c.op) {
                    case CondOp::GT: return l > r;
                    case CondOp::GE: return l >= r;
                    case CondOp::LT: return l < r;
                    case CondOp::LE: return l <= r;
                    case CondOp::EQ: return std::abs(l - r) < EPSILON_ZERO;
                    case CondOp::NE: return std::abs(l - r) >= EPSILON_ZERO;
                    case CondOp::COUNT_: assert(false && "invalid CondOp"); return false;
                }
            } catch (...) { return std::nullopt; }
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
            } catch (...) { /* malformed, skip */ }
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
            auto lhs_tok = std::vector<Token>(mod_tok.begin(), mod_tok.begin() + eq_pos);
            lhs_tok.push_back(Token{TokenType::END, "", 0});
            Parser lp(lhs_tok);
            Parser rp(std::vector<Token>(mod_tok.begin() + eq_pos + 1, mod_tok.end()));
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
        try { cond = parse_condition(cond_part); } catch (...) { /* malformed condition → ignore */ }
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

        std::string path = base_dir + "/" + file_part;
        if (path.find('.') == std::string::npos) path += ".fw";
        std::string abs_path = std::filesystem::weakly_canonical(path).string();

        // Cache key includes section
        std::string cache_key = abs_path + (section.empty() ? "" : "#" + section);
        auto it = sub_systems.find(cache_key);
        if (it != sub_systems.end()) return *it->second;

        auto sub = std::make_shared<FormulaSystem>();
        sub->trace = trace;
        sub->numeric_mode = numeric_mode;
        sub->load_file(abs_path, section);
        sub_systems[cache_key] = sub;
        return *sub;
    }

    std::map<std::string, double> prepare_sub_bindings(
        const FormulaCall& call,
        std::map<std::string, double>& parent_bindings,
        std::set<std::string> visited = {}, int depth = 0,
        const std::string& skip_parent_var = "",
        bool resolve_unknowns = true) const
    {
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
                        double val = solve_recursive(v, parent_bindings, visited, depth + 1);
                        resolved = substitute(resolved, v, Expr::Num(val));
                    } catch (...) { return; }
                } else { return; }
            }
            try {
                sub[sub_var] = evaluate(*simplify(resolved));
            } catch (...) { return; }
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
                    double val = solve_recursive(call.output_var, parent_bindings, visited, depth + 1);
                    sub[call.query_var] = val;
                } catch (...) {}
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
    };

    // Generates candidates for solving a target variable.
    // Calls handler(candidate) for each. Handler returns true to stop.
    // Optional bindings are used for Strategy 4 (equating) to substitute
    // known values before solving, preventing spurious results.
    template<typename Handler>
    void enumerate_candidates(const std::string& target, Handler&& handler,
                              const std::map<std::string, double>* sub_bindings = nullptr) const {
        // Strategy 1: target on LHS — direct from RHS
        for (auto& eq : equations)
            if (eq.lhs_var == target)
                if (handler(Candidate{CandidateType::EXPR, eq.rhs,
                    target + " = " + expr_to_string(eq.rhs), nullptr, "",
                    eq.condition ? &*eq.condition : nullptr}))
                    return;

        // Strategy 2: target in RHS — algebraic inversion (may produce multiple solutions)
        for (auto& eq : equations) {
            if (!contains_var(eq.rhs, target)) continue;
            auto sols = solve_for_all(Expr::Var(eq.lhs_var), eq.rhs, target);
            for (auto& sol : sols)
                if (sol.expr)
                    if (handler(Candidate{CandidateType::EXPR, sol.expr,
                        target + " = " + expr_to_string(sol.expr)
                        + "  (from " + eq.lhs_var + " = " + expr_to_string(eq.rhs) + ")"
                        + (sol.cond_desc.empty() ? "" : "  [" + sol.cond_desc + "]"),
                        nullptr, "", eq.condition ? &*eq.condition : nullptr}))
                        return;
        }

        // Strategy 3: forward formula call
        for (auto& call : formula_calls)
            if (call.output_var == target)
                if (handler(Candidate{CandidateType::FORMULA_FWD, nullptr,
                    target + " via " + call.file_stem + "(" + call.query_var + "=?)",
                    &call, "", nullptr}))
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
                            nullptr, "", cond}))
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
                        &call, sub_var, nullptr}))
                        return;

        // Strategy 6: numeric root-finding (--numeric only)
        if (numeric_mode) {
            for (auto& eq : equations) {
                if (eq.lhs_var != target && !contains_var(eq.rhs, target)) continue;
                auto combined = simplify(Expr::BinOpExpr(BinOp::SUB,
                    Expr::Var(eq.lhs_var), eq.rhs));
                if (handler(Candidate{CandidateType::NUMERIC, combined,
                    target + " ~= numeric  (from " + eq.lhs_var + " = " + expr_to_string(eq.rhs) + ")",
                    nullptr, "", eq.condition ? &*eq.condition : nullptr}))
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
    std::map<std::string, ExprPtr> derive_unfold_bindings(
            const FormulaCall& call,
            const std::map<std::string, ExprPtr>& bindings) const {
        std::map<std::string, ExprPtr> parent_map;
        for (auto& [sv, expr] : call.bindings)
            parent_map[sv] = substitute_bindings(expr, bindings);
        return parent_map;
    }

    // --- Derive (symbolic solver) ---

    ExprPtr try_derive(const ExprPtr& expr, const std::string& target,
                       std::map<std::string, ExprPtr>& bindings,
                       std::set<std::string> visited, int depth) const { // NOLINT(performance-unnecessary-value-param) — intentional copy per branch
        std::set<std::string> vars;
        collect_vars(expr, vars);

        bool has_target = false;
        ExprPtr resolved = expr;
        for (auto& v : vars) {
            if (v == target) { has_target = true; continue; }
            if (auto it = bindings.find(v); it != bindings.end()) {
                resolved = substitute(resolved, v, it->second);
            } else {
                auto sub_expr = derive_recursive(v, bindings, visited, depth + 1);
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
        for (auto& a : simplify_get_assumptions())
            trace.step("  assuming: " + a.desc + (a.inherent ? " (inherent)" : ""), depth + 1);

        // If the target appears in the resolved expression, we have:
        //   target = f(target, ...) — try to solve algebraically
        if (has_target) {
            auto sol = solve_for(Expr::Var(target), result, target);
            if (sol) return simplify(sol);
            return nullptr; // Non-linear in target — can't solve
        }

        // Try full evaluation — if it works, return a clean number
        try {
            double val = evaluate(result);
            if (!std::isnan(val) && !std::isinf(val)) return Expr::Num(val);
        } catch (...) { return result; }
        return result;
    }

    ExprPtr derive_recursive(const std::string& target,
                             std::map<std::string, ExprPtr>& bindings,
                             std::set<std::string> visited, int depth) const {
        if (auto it = bindings.find(target); it != bindings.end()) return it->second;
        if (is_active_builtin(target)) return Expr::Var(target);
        if (visited.count(target)) return nullptr;
        visited.insert(target);

        // Check condition using symbolic bindings (evaluate what we can)
        auto derive_check_condition = [&](const Condition* cond) -> bool {
            if (!cond) return true; // no condition = always valid
            std::map<std::string, double> numeric;
            for (auto& [k, v] : bindings) {
                try { numeric[k] = evaluate(*v); } catch (...) {}
            }
            return check_condition(*cond, numeric);
        };

        ExprPtr found = nullptr;
        enumerate_candidates(target, [&](const Candidate& c) {
            // Check condition before trying the candidate
            if (!derive_check_condition(c.condition)) return false;

            if (c.type == CandidateType::EXPR) {
                auto result = try_derive(c.expr, target, bindings, visited, depth);
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
                                try { cond_binds[sv] = evaluate(*pe); } catch (...) {}
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
                        for (auto& fc : sub_sys.formula_calls)
                            if (remaining.count(fc.output_var))
                                { has_formula_output = true; break; }
                        if (!has_formula_output) {
                            bindings[target] = unfolded;
                            found = unfolded;
                            return true;
                        }
                    }
                } catch (...) {}

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
                        auto result = sub_sys.derive_recursive(c.call->query_var, sub_binds, {}, depth + 1);
                        if (result) { bindings[target] = result; found = result; return true; }
                    } catch (...) { return false; }
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
                    for (auto& eq : sub_sys.equations) {
                        if (eq.lhs_var != c.call->query_var) continue;
                        // Check sub-system equation condition if possible
                        if (eq.condition) {
                            std::map<std::string, double> cond_binds;
                            for (auto& [sv, pe] : parent_map) {
                                try { cond_binds[sv] = evaluate(*pe); } catch (...) {}
                            }
                            for (auto& [k, v] : bindings) {
                                try { cond_binds[k] = evaluate(*v); } catch (...) {}
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
                                final_expr = try_derive(sol.expr, target, b, visited, depth);
                            } else {
                                auto final_sols = solve_for_all(sol.expr, binding_expr, target);
                                for (auto& fs : final_sols) {
                                    if (!fs.expr) continue;
                                    auto b = bindings;
                                    final_expr = try_derive(fs.expr, target, b, visited, depth);
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
                } catch (...) {}
            }
            return false;
        });
        return found;
    }

    // --- Numeric solver ---

    // Memoized resolve for numeric scanning — caches results to avoid
    // redundant recursive evaluations (critical for factorial, fibonacci)
    double resolve_memoized(const std::string& target,
                            std::map<std::string, double> bindings) const {
        // Build cache key: target + sorted bindings
        std::string key = target;
        for (auto& [k, v] : bindings)
            key += "," + k + "=" + fmt_num(v);

        auto it = numeric_memo_.find(key);
        if (it != numeric_memo_.end()) return it->second;

        double result = resolve(target, bindings);
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
            for (auto& iv : vs.intervals()) {
                if (!std::isinf(iv.low) && iv.low > lo) lo = iv.low;
                if (!std::isinf(iv.high) && iv.high < hi) hi = iv.high;
            }
        };

        // Equation condition
        if (eq_condition) apply_valueset(eq_condition->to_valueset(target, bindings));

        // Global conditions
        for (auto& gc : global_conditions)
            apply_valueset(gc.to_valueset(target, bindings));

        return {lo, hi};
    }

    // Try to solve for target numerically by finding roots of f(target) = 0
    std::vector<double> try_resolve_numeric(
            const ExprPtr& combined, const std::string& target,
            std::map<std::string, double>& bindings,
            std::set<std::string> visited, int depth,
            const Condition* eq_condition = nullptr) const {

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
                try {
                    double val = solve_recursive(v, bindings, visited, depth + 1);
                    expr = substitute(expr, v, Expr::Num(val));
                } catch (...) { has_formula_vars = true; } // treat as unresolvable
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
                try {
                    ExprPtr subst = substitute(expr, target, Expr::Num(x));
                    return evaluate(*simplify(subst));
                } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
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
            for (auto& eq : equations) {
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
                for (auto& pv : probe_vars) if (pv == bvar) { found = true; break; }
                if (!found) probe_vars.push_back(bvar);
            }

            for (auto& probe_var : probe_vars) {
                if (!bindings.count(probe_var)) continue;
                double expected = bindings.at(probe_var);

                auto f = [&](double x) -> double {
                    try {
                        auto test_binds = bindings;
                        test_binds[target] = x;
                        test_binds.erase(probe_var); // remove so it gets recomputed
                        double computed = resolve_memoized(probe_var, test_binds);
                        return computed - expected;
                    } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
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
        for (double r : roots) {
            auto test_binds = bindings;
            test_binds[target] = r;
            bool ok = true;
            if (eq_condition && !check_condition(*eq_condition, test_binds)) ok = false;
            for (auto& gc : global_conditions)
                if (!check_condition(gc, test_binds)) ok = false;
            if (ok) filtered.push_back(r);
        }
        return filtered;
    }

    // --- Solver ---

    double solve_recursive(const std::string& target,
                           std::map<std::string, double>& bindings,
                           std::set<std::string> visited, int depth) const {
        if (auto it = bindings.find(target); it != bindings.end()) {
            trace.calc("known: " + target + " = " + fmt_num(it->second), depth + 1);
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
        visited.insert(target);

        bool found_eq = false;
        bool had_nan_inf = false;
        std::set<std::string> missing;

        auto try_expr = [&](const ExprPtr& expr, const std::string& label) -> bool {
            found_eq = true;
            trace.step(label, depth + 1);
            return try_resolve(expr, target, bindings, visited, depth, had_nan_inf, missing);
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
                auto sub_binds = prepare_sub_bindings(call, bindings, visited, depth, skip_var);
                auto& sub_sys = load_sub_system(call.file_stem);
                sub_sys.max_formula_depth = max_formula_depth;
                for (auto& [sv, val] : sub_binds)
                    trace.calc("  binding: " + sv + " = " + fmt_num(val), depth + 2);
                double result = sub_sys.resolve(resolve_var, sub_binds);
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
                    found_eq = true;
                    trace.step(c.desc, depth + 1);
                    auto roots = try_resolve_numeric(c.expr, target, bindings,
                        visited, depth, c.condition);
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
                for (auto& gc : global_conditions) {
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
        if (solved) return bindings.at(target);

        // Error reporting
        if (!found_eq)
            throw std::runtime_error("No equation found for '" + target + "'");
        if (had_nan_inf && missing.empty())
            throw std::runtime_error("Cannot solve for '" + target
                + "': all equations produced invalid results (NaN or infinity)");
        if (!missing.empty()) {
            std::string list;
            for (auto& v : missing) list += (list.empty() ? "" : ", ") + ("'" + v + "'");
            throw std::runtime_error("Cannot solve for '" + target + "': no value for " + list);
        }
        throw std::runtime_error("Cannot solve for '" + target + "'");
    }

    bool try_resolve(const ExprPtr& expr, const std::string& target,
                     std::map<std::string, double>& bindings,
                     std::set<std::string> visited, int depth, // NOLINT(performance-unnecessary-value-param) — intentional copy per branch
                     bool& had_nan_inf, std::set<std::string>& missing) const {
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
                trace.step("need: " + v, depth + 2);
                try {
                    double val = solve_recursive(v, bindings, visited, depth + 1);
                    trace.calc("substitute " + v + " = " + fmt_num(val), depth + 2);
                    resolved = substitute(resolved, v, Expr::Num(val));
                } catch (const std::runtime_error& e) {
                    if (std::string(e.what()).find("depth") != std::string::npos) throw;
                    missing.insert(v);
                    return false;
                } catch (...) {
                    missing.insert(v);
                    return false;
                }
            }
        }

        try {
            trace.calc("evaluate: " + expr_to_string(resolved), depth + 2);
            simplify_clear_assumptions();
            auto simplified = simplify(resolved);
            auto assumptions = simplify_get_assumptions();
            for (auto& a : assumptions)
                trace.step("  assuming: " + a.desc + (a.inherent ? " (inherent)" : ""), depth + 2);
            double result = evaluate(simplified);
            if (std::isnan(result) || std::isinf(result)) {
                trace.step("result is " + std::string(std::isnan(result) ? "NaN" : "inf")
                    + ", trying alternatives", depth + 1);
                had_nan_inf = true;
                return false;
            }
            trace.step("result: " + target + " = " + fmt_num(result), depth + 1);
            bindings[target] = result;
            return true;
        } catch (...) {
            return false;
        }
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
        if (eq == std::string::npos) continue;

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
            catch (...) { pos = 0; }
            if (pos != val.size()) {
                // Try parsing as expression (e.g. "10*2^3", "sqrt(2)")
                try {
                    ExprArena temp_arena;
                    ExprArena::Scope scope(temp_arena);
                    auto expr = Parser(Lexer(val).tokenize()).parse_expr();
                    v = evaluate(*simplify(expr));
                } catch (...) {
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
