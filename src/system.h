#pragma once
#include "expr.h"
#include "lexer.h"
#include "parser.h"
#include "trace.h"
#include <string>
#include <vector>
#include <map>
#include <set>
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

class FormulaSystem {
public:
    std::vector<Equation> equations;
    std::map<std::string, double> defaults;
    std::string base_dir;
    Trace trace;

    void load_file(const std::string& path) {
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
        }
    }

    double resolve(const std::string& target,
                   std::map<std::string, double> bindings) const {
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
        if (bindings.count(target)) return bindings.at(target);

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

    void parse_line(const std::string& line) {
        auto tok = Lexer(line).tokenize();
        if (tok.size() < 3) return;
        if (tok[0].type != TokenType::IDENT || tok[1].type != TokenType::EQUALS) return;

        const std::string& lhs = tok[0].text;

        // Default: "x = 42" or "x = -42"
        if (tok[2].type == TokenType::NUMBER && tok[3].type == TokenType::END) {
            defaults[lhs] = tok[2].numval;
            return;
        }
        if (tok[2].type == TokenType::MINUS
            && tok[3].type == TokenType::NUMBER
            && tok[4].type == TokenType::END) {
            defaults[lhs] = -tok[3].numval;
            return;
        }

        // Equation: parse RHS as expression
        Parser p(std::vector<Token>(tok.begin() + 2, tok.end()));
        equations.push_back({lhs, p.parse_expr()});
    }

    // --- Solver ---

    double solve_recursive(const std::string& target,
                           std::map<std::string, double>& bindings,
                           std::set<std::string> visited, int depth) const {
        if (bindings.count(target)) {
            trace.calc("known: " + target + " = " + fmt_num(bindings.at(target)), depth + 1);
            return bindings.at(target);
        }
        if (visited.count(target))
            throw std::runtime_error(
                "Circular dependency: '" + target + "' depends on itself through a chain of equations");
        visited.insert(target);

        bool found_eq = false;
        bool had_nan_inf = false;
        std::set<std::string> missing;

        // Helper: try evaluating a candidate expression for the target
        auto try_expr = [&](const ExprPtr& expr, const std::string& label) -> bool {
            found_eq = true;
            trace.step(label, depth + 1);
            return try_resolve(expr, target, bindings, visited, depth, had_nan_inf, missing);
        };

        // Strategy 1: target on LHS — direct evaluation
        for (auto& eq : equations)
            if (eq.lhs_var == target)
                if (try_expr(eq.rhs, "found: " + eq.lhs_var + " = " + expr_to_string(eq.rhs)))
                    return bindings.at(target);

        // Strategy 2: target in RHS — algebraic inversion
        for (auto& eq : equations) {
            if (!contains_var(eq.rhs, target)) continue;
            trace.step("inverting: " + eq.lhs_var + " = " + expr_to_string(eq.rhs), depth + 1);
            auto sol = solve_for(Expr::Var(eq.lhs_var), eq.rhs, target);
            if (sol) {
                if (try_expr(sol, "  => " + target + " = " + expr_to_string(sol)))
                    return bindings.at(target);
            } else {
                trace.step("  (cannot isolate — nonlinear)", depth + 1);
            }
        }

        // Strategy 3: equate RHS of equations sharing a LHS variable
        for (size_t i = 0; i < equations.size(); i++)
            for (size_t j = i + 1; j < equations.size(); j++) {
                if (equations[i].lhs_var != equations[j].lhs_var) continue;
                found_eq = true;
                trace.step("substituting via '" + equations[i].lhs_var + "':", depth + 1);
                trace.step("  " + expr_to_string(equations[i].rhs)
                    + "  =  " + expr_to_string(equations[j].rhs), depth + 1);

                // Substitute known bindings so solve_for can detect
                // tautological equations (e.g. a*c = a*c when a=b)
                auto sub_bindings = [&](const ExprPtr& e) {
                    ExprPtr r = e;
                    for (auto& [v, val] : bindings)
                        if (v != target) r = substitute(r, v, Expr::Num(val));
                    return simplify(r);
                };
                auto ei = sub_bindings(equations[i].rhs);
                auto ej = sub_bindings(equations[j].rhs);

                // Try both directions
                for (auto& [a, b] : {std::pair{ei, ej}, std::pair{ej, ei}}) {
                    auto sol = solve_for(a, b, target);
                    if (sol)
                        if (try_expr(sol, "  => " + target + " = " + expr_to_string(sol)))
                            return bindings.at(target);
                }
            }

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
                     std::set<std::string> visited, int depth,
                     bool& had_nan_inf, std::set<std::string>& missing) const {
        // Resolve all free variables in the expression
        std::set<std::string> vars;
        collect_vars(expr, vars);

        ExprPtr resolved = expr;
        for (auto& v : vars) {
            if (v == target) return false;
            if (bindings.count(v)) {
                trace.calc("substitute " + v + " = " + fmt_num(bindings.at(v)), depth + 2);
                resolved = substitute(resolved, v, Expr::Num(bindings.at(v)));
            } else {
                trace.step("need: " + v, depth + 2);
                try {
                    double val = solve_recursive(v, bindings, visited, depth + 1);
                    trace.calc("substitute " + v + " = " + fmt_num(val), depth + 2);
                    resolved = substitute(resolved, v, Expr::Num(val));
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
};

inline CLIQuery parse_cli_query(const std::string& input) {
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
                throw std::runtime_error("Invalid number '" + val + "' for variable '" + name + "'");
            }
            if (std::isnan(v))
                throw std::runtime_error("NaN is not a valid value for '" + name + "'");
            if (std::isinf(v))
                throw std::runtime_error("Infinity is not a valid value for '" + name + "'");
            q.bindings[name] = v;
        }
    }

    if (q.queries.empty())
        throw std::runtime_error("No query variable (use var=?)");
    return q;
}
