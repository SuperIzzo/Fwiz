#pragma once
#include "expr.h"
#include "lexer.h"
#include "parser.h"
#include "trace.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
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

struct Equation {
    std::string lhs_var;
    ExprPtr rhs;
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
    // sub_system_var -> parent_var
    std::map<std::string, std::string> bindings;
};

class FormulaSystem {
public:
    mutable ExprArena arena;
    std::vector<Equation> equations;
    std::map<std::string, double> defaults;
    std::vector<FormulaCall> formula_calls;
    std::string base_dir;
    Trace trace;
    mutable int max_formula_depth = 1000;
    static inline thread_local int formula_depth_ = 0;
    mutable std::map<std::string, std::shared_ptr<FormulaSystem>> sub_systems;

    std::set<std::string> all_variables() const {
        std::set<std::string> vars;
        for (auto& eq : equations) {
            vars.insert(eq.lhs_var);
            collect_vars(eq.rhs, vars);
        }
        for (auto& [k, v] : defaults) vars.insert(k);
        for (auto& fc : formula_calls) {
            vars.insert(fc.output_var);
            for (auto& [sub_var, parent_var] : fc.bindings)
                vars.insert(parent_var);
        }
        return vars;
    }

    void load_file(const std::string& path) {
        ExprArena::Scope scope(arena);
        if (path.empty())
            throw std::runtime_error("No file path provided");
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
            throw std::runtime_error("Path is a directory, not a file: " + path);

        base_dir = std::filesystem::path(path).parent_path().string();
        if (base_dir.empty()) base_dir = ".";

        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open file: " + path);

        trace.step("loading " + path);
        std::string line;
        int line_num = 0;
        bool first = true;

        while (std::getline(f, line)) {
            line_num++;
            if (first) { first = false; strip_bom(line); }
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            try { parse_line(line); }
            catch (const std::exception& e) {
                trace.step("  warning: skipping line " + std::to_string(line_num) + ": " + e.what());
            }
        }

        if (trace.show_steps()) {
            for (auto& eq : equations)
                trace.step("  equation: " + eq.lhs_var + " = " + expr_to_string(eq.rhs));
            for (auto& [k, v] : defaults)
                trace.step("  default: " + k + " = " + fmt_num(v));
            for (auto& fc : formula_calls) {
                std::string s = "  formula call: " + fc.file_stem + "(" + fc.query_var + "=?" + fc.output_var;
                for (auto& [sv, pv] : fc.bindings) { s += ", "; s += sv; s += "="; s += pv; }
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
            }
            return false; // verify collects ALL, never stops
        });

        return results;
    }

    // --- Derive (symbolic) ---

    std::string derive(const std::string& target,
                       const std::map<std::string, double>& numeric_bindings,
                       const std::map<std::string, std::string>& symbolic_bindings) const {
        ExprArena::Scope scope(arena);
        std::map<std::string, ExprPtr> bindings;
        for (auto& [k, v] : numeric_bindings) bindings[k] = Expr::Num(v);
        for (auto& [k, v] : symbolic_bindings) bindings[k] = Expr::Var(v);
        // Apply defaults as numeric
        for (auto& [k, v] : defaults)
            if (!bindings.count(k) && k != target) bindings[k] = Expr::Num(v);

        auto result = derive_recursive(target, bindings, {}, 0);
        if (!result) throw std::runtime_error("Cannot derive equation for '" + target + "'");

        // Try to evaluate to a number; if symbolic vars remain, return as expression
        try {
            double val = evaluate(result);
            if (!std::isnan(val) && !std::isinf(val)) return fmt_num(val);
        } catch (const std::exception& ex) {
            trace.calc("derive: symbolic result (cannot evaluate: " + std::string(ex.what()) + ")");
        }

        return expr_to_string(result);
    }

    double resolve(const std::string& target,
                   std::map<std::string, double> bindings) const {
        ExprArena::Scope scope(arena);
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
        if (auto it = bindings.find(target); it != bindings.end()) return it->second;

        return solve_recursive(target, bindings, {}, 0);
    }

private:
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
            // Check for binding: IDENT EQUALS IDENT
            else if (i + 2 < rparen_pos
                     && tok[i + 1].type == TokenType::EQUALS
                     && tok[i + 2].type == TokenType::IDENT) {
                call.bindings[tok[i].text] = tok[i + 2].text;
                i += 3;
            }
            // Shorthand binding: bare IDENT
            else {
                call.bindings[tok[i].text] = tok[i].text;
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

    void parse_line(const std::string& line) {
        auto tok = Lexer(line).tokenize();
        if (tok.size() < 2) return;

        // Extract formula calls before expression parsing
        auto [mod_tok, calls] = extract_formula_calls(tok);
        for (auto& c : calls) formula_calls.push_back(std::move(c));

        // Standalone formula call: just "output_var END" after extraction
        if (mod_tok.size() <= 2) return;

        if (mod_tok[0].type != TokenType::IDENT || mod_tok[1].type != TokenType::EQUALS) return;

        const std::string& lhs = mod_tok[0].text;

        // Degenerate "x = x" from implied alias — skip
        if (mod_tok[2].type == TokenType::IDENT && mod_tok[2].text == lhs
            && mod_tok[3].type == TokenType::END)
            return;

        // Default: "x = 42" or "x = -42"
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

        // Equation: parse RHS as expression
        Parser p(std::vector<Token>(mod_tok.begin() + 2, mod_tok.end()));
        equations.push_back({lhs, p.parse_expr()});
    }

    // --- Sub-system loading ---

    const FormulaSystem& load_sub_system(const std::string& file_stem) const {
        std::string path = base_dir + "/" + file_stem;
        if (path.find('.') == std::string::npos) path += ".fw";
        std::string abs_path = std::filesystem::weakly_canonical(path).string();

        auto it = sub_systems.find(abs_path);
        if (it != sub_systems.end()) return *it->second;

        auto sub = std::make_shared<FormulaSystem>();
        sub->trace = trace;
        sub->load_file(abs_path);
        sub_systems[abs_path] = sub;
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

        // Try to get a parent variable's value — from bindings or by solving
        auto try_bind = [&](const std::string& sub_var, const std::string& parent_var) {
            if (auto it = parent_bindings.find(parent_var); it != parent_bindings.end()) {
                sub[sub_var] = it->second;
            } else if (resolve_unknowns) {
                try {
                    double val = solve_recursive(parent_var, parent_bindings, visited, depth + 1);
                    sub[sub_var] = val;
                } catch (...) { return; }
            }
        };

        for (auto& [sv, pv] : call.bindings) {
            if (pv != skip_parent_var) try_bind(sv, pv);
        }

        // Bridge: output_var -> query_var
        if (call.output_var != skip_parent_var)
            try_bind(call.query_var, call.output_var);

        return sub;
    }

    // --- Strategy enumeration ---

    enum class CandidateType : uint8_t { EXPR, FORMULA_FWD, FORMULA_REV };
    struct Candidate {
        CandidateType type;
        ExprPtr expr;           // for EXPR candidates
        std::string desc;
        const FormulaCall* call;  // for formula candidates
        std::string sub_var;      // for FORMULA_REV: which sub-system var to solve
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
                    target + " = " + expr_to_string(eq.rhs), nullptr, ""}))
                    return;

        // Strategy 2: target in RHS — algebraic inversion
        for (auto& eq : equations) {
            if (!contains_var(eq.rhs, target)) continue;
            auto sol = solve_for(Expr::Var(eq.lhs_var), eq.rhs, target);
            if (sol)
                if (handler(Candidate{CandidateType::EXPR, sol,
                    target + " = " + expr_to_string(sol)
                    + "  (from " + eq.lhs_var + " = " + expr_to_string(eq.rhs) + ")",
                    nullptr, ""}))
                    return;
        }

        // Strategy 3: forward formula call
        for (auto& call : formula_calls)
            if (call.output_var == target)
                if (handler(Candidate{CandidateType::FORMULA_FWD, nullptr,
                    target + " via " + call.file_stem + "(" + call.query_var + "=?)",
                    &call, ""}))
                    return;

        // Strategy 4: equate RHS of equations sharing a LHS variable
        for (size_t i = 0; i < equations.size(); i++)
            for (size_t j = i + 1; j < equations.size(); j++) {
                if (equations[i].lhs_var != equations[j].lhs_var) continue;
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
                    auto sol = solve_for(a, b, target);
                    if (sol)
                        if (handler(Candidate{CandidateType::EXPR, sol,
                            target + " = " + expr_to_string(sol)
                            + "  (via " + equations[i].lhs_var + ")",
                            nullptr, ""}))
                            return;
                }
            }

        // Strategy 5: reverse formula call
        for (auto& call : formula_calls)
            for (auto& [sub_var, parent_var] : call.bindings)
                if (parent_var == target)
                    if (handler(Candidate{CandidateType::FORMULA_REV, nullptr,
                        target + " via " + call.file_stem + "(" + std::string(sub_var) + ")",
                        &call, sub_var}))
                        return;
    }

    // --- Derive (symbolic solver) ---

    ExprPtr try_derive(const ExprPtr& expr, const std::string& target,
                       std::map<std::string, ExprPtr>& bindings,
                       std::set<std::string> visited, int depth) const { // NOLINT(performance-unnecessary-value-param) — intentional copy per branch
        std::set<std::string> vars;
        collect_vars(expr, vars);

        ExprPtr resolved = expr;
        for (auto& v : vars) {
            if (v == target) return nullptr;
            if (auto it = bindings.find(v); it != bindings.end()) {
                resolved = substitute(resolved, v, it->second);
            } else {
                auto sub_expr = derive_recursive(v, bindings, visited, depth + 1);
                if (sub_expr)
                    resolved = substitute(resolved, v, sub_expr);
                else
                    return nullptr; // Can't resolve this variable — try next equation
            }
        }

        auto result = simplify(resolved);
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
        if (visited.count(target)) return nullptr;
        visited.insert(target);

        ExprPtr found = nullptr;
        enumerate_candidates(target, [&](const Candidate& c) {
            if (c.type == CandidateType::EXPR) {
                auto result = try_derive(c.expr, target, bindings, visited, depth);
                if (result) { bindings[target] = result; found = result; return true; }
            } else if (c.type == CandidateType::FORMULA_FWD) {
                std::map<std::string, ExprPtr> sub_binds;
                for (auto& [sv, pv] : c.call->bindings)
                    if (auto it = bindings.find(pv); it != bindings.end()) sub_binds[sv] = it->second;
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
            // FORMULA_REV not needed for derive (symbolic doesn't reverse through bindings)
            return false;
        });
        return found;
    }

    // --- Solver ---

    double solve_recursive(const std::string& target,
                           std::map<std::string, double>& bindings,
                           std::set<std::string> visited, int depth) const {
        if (auto it = bindings.find(target); it != bindings.end()) {
            trace.calc("known: " + target + " = " + fmt_num(it->second), depth + 1);
            return it->second;
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
            switch (c.type) {
                case CandidateType::EXPR:
                    if (try_expr(c.expr, c.desc)) { solved = true; return true; }
                    break;
                case CandidateType::FORMULA_FWD:
                    if (try_formula(*c.call, c.call->query_var)) { solved = true; return true; }
                    break;
                case CandidateType::FORMULA_REV:
                    if (try_formula(*c.call, c.sub_var, target)) { solved = true; return true; }
                    break;
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
            double result = evaluate(simplify(resolved));
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

struct CLIQuery {
    std::string filename;
    std::vector<std::pair<std::string, std::string>> queries; // {variable, alias}
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
    if (q.filename.find('.') == std::string::npos) q.filename += ".fw";

    size_t rparen = input.find(')', lparen);
    if (rparen == std::string::npos)
        throw std::runtime_error("Missing closing parenthesis");

    // Parse comma-separated "name=value" or "name=?" or "name=?alias" pairs
    std::istringstream ss(input.substr(lparen + 1, rparen - lparen - 1));
    std::string arg;
    while (std::getline(ss, arg, ',')) {
        arg = trim(arg);
        if (arg.empty()) continue;

        size_t eq = arg.find('=');
        if (eq == std::string::npos) continue;

        std::string name = trim(arg.substr(0, eq));
        std::string val  = trim(arg.substr(eq + 1));

        if (name.empty())
            throw std::runtime_error("Missing variable name in '" + arg + "'");

        if (val.size() >= 1 && val[0] == '?') {
            // Query: "x=?" or "x=?alias"
            std::string alias = (val.size() > 1) ? trim(val.substr(1)) : name;
            q.queries.push_back({name, alias});
        } else if (val.empty()) {
            throw std::runtime_error("Missing value for '" + name + "'");
        } else {
            double v;
            try { v = std::stod(val); }
            catch (...) {
                if (allow_symbolic) {
                    q.symbolic[name] = val;
                    continue;
                }
                throw std::runtime_error("Invalid number '" + val + "' for variable '" + name + "'");
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
