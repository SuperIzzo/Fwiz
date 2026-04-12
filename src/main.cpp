#include "system.h"
#include <iostream>
#include <iomanip>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: fwiz [flags] <formula>(var=?, var=?alias, var=value, ...)\n"
                  << "       fwiz [flags] (var=?, var=value, ...) \"equations\"\n"
                  << "       echo \"equations\" | fwiz [flags] (var=?, var=value, ...)\n"
                  << "\n"
                  << "  var=?          solve for var\n"
                  << "  var=?alias     solve for var, output as alias\n"
                  << "  --steps        show algebraic reasoning\n"
                  << "  --calc         show steps + numeric evaluation detail\n"
                  << "  --explore      solve what you can, print ? for the rest\n"
                  << "  --explore-full like --explore but prints all variables in the system\n"
                  << "  --verify all   verify all known variables against all equations\n"
                  << "  --verify A,B   verify specific variables\n"
                  << "  --derive       output symbolic equation instead of numeric result\n"
                  << "  --no-numeric   disable numeric solving (algebraic only)\n"
                  << "  --fit [N]      fit a curve (depth N, default 2: compose templates)\n"
                  << "  --output FILE  write fitted equation to a .fw file\n"
                  << "  --precision N  set sample density (default 200)\n"
                  << "\n"
                  << "Example: fwiz physics(force=?, mass=10)\n"
                  << "         fwiz --explore triangle(a=?, b=?, c=?, A=40, B=80)\n"
                  << "         fwiz --verify all triangle(A=40, B=60, C=80)\n"
                  << "         fwiz --derive triangle(C=?, a=a, b=b, c=c)\n";
        return 1;
    }

    try {
        TraceLevel level = TraceLevel::NONE;
        bool explore = false;
        bool explore_full = false;
        bool derive_mode = false;
        bool fit_mode = false;
        int fit_depth = FIT_DEFAULT_DEPTH;
        bool numeric_mode = true;
        int sys_samples = NUMERIC_DEFAULT_SAMPLES;
        std::string verify_arg;
        std::string output_file;
        std::string query_str;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if      (arg == "--steps")        level = TraceLevel::STEPS;
            else if (arg == "--calc")         level = TraceLevel::CALC;
            else if (arg == "--explore")      explore = true;
            else if (arg == "--explore-full") { explore = true; explore_full = true; }
            else if (arg == "--derive")       derive_mode = true;
            else if (arg == "--fit") {
                fit_mode = true;
                // Optional depth argument: --fit 3
                if (i + 1 < argc) {
                    try { fit_depth = std::stoi(argv[i + 1]); i++; }
                    catch (...) {} // not a number — leave for query string
                }
            }
            else if (arg == "--no-numeric")   numeric_mode = false;
            else if (arg == "--output") {
                if (i + 1 < argc) output_file = argv[++i];
                else { std::cerr << "Error: --output requires a filename\n"; return 1; }
            }
            else if (arg == "--precision") {
                if (i + 1 < argc) {
                    try { sys_samples = std::stoi(argv[++i]); }
                    catch (...) { std::cerr << "Error: --precision requires a number\n"; return 1; }
                } else { std::cerr << "Error: --precision requires an argument\n"; return 1; }
            }
            else if (arg == "--verify") {
                if (i + 1 < argc) verify_arg = argv[++i];
                else { std::cerr << "Error: --verify requires an argument (all or var1,var2,...)\n"; return 1; }
            }
            else    { if (!query_str.empty()) query_str += ' '; query_str += arg; }
        }

        if (query_str.empty()) {
            std::cerr << "Error: no query provided\n";
            return 1;
        }

        bool has_verify = !verify_arg.empty();
        bool allow_missing = explore || explore_full || has_verify || derive_mode || fit_mode;
        bool allow_symbolic = derive_mode || fit_mode;
        auto query = parse_cli_query(query_str, allow_missing, allow_symbolic);
        FormulaSystem sys;
        sys.trace.level = level;
        sys.numeric_mode = numeric_mode;
        sys.numeric_samples = sys_samples;
        sys.fit_depth = fit_depth;

        if (query.filename.empty()) {
            // Query-first format: inline source or stdin
            if (!query.inline_source.empty()) {
                // Replace semicolons with newlines for compact inline format
                std::string source = query.inline_source;
                for (auto& c : source) if (c == ';') c = '\n';
                sys.load_string(source, "<inline>", query.section);
            } else {
                // Read from stdin
                std::string source, line;
                while (std::getline(std::cin, line))
                    source += line + "\n";
                if (source.empty()) {
                    std::cerr << "Error: no equations provided (use inline or pipe from stdin)\n";
                    return 1;
                }
                sys.load_string(source, "<stdin>");
            }
        } else {
            sys.load_file(query.filename, query.section);
        }

        // --- Derive mode ---
        if (derive_mode) {
            std::map<std::string, std::string> derived_eqs;
            for (auto& q : query.queries) {
                try {
                    auto result = sys.derive(q.variable, query.bindings, query.symbolic);
                    derived_eqs[q.variable] = result;
                    std::cout << q.alias << " = " << result << '\n';
                } catch (const std::exception& e) {
                    if (!fit_mode) {
                        std::cerr << "Error: " << e.what() << '\n';
                        return 1;
                    }
                    // derive failed but fit might still work — continue
                }
            }
            if (!fit_mode) return 0;

            // --- Fit after derive ---
            for (auto& q : query.queries) {
                try {
                    auto result = sys.fit(q.variable, query.bindings, query.symbolic);
                    auto print_if_new = [&](const FormulaSystem::FitOutput& f) {
                        if (derived_eqs.count(q.variable) && derived_eqs[q.variable] == f.equation)
                            return;
                        std::string sign = f.exact ? " = " : " ~ ";
                        std::cout << q.alias << sign << f.equation << '\n';
                        std::cerr << "  R² = " << fmt_num(f.r_squared)
                                  << ", max error = " << fmt_num(f.max_error) << '\n';
                    };
                    print_if_new(result);
                    for (auto& alt : result.alternatives)
                        print_if_new(alt);
                } catch (const std::exception& e) {
                    std::cerr << "Error (fit): " << e.what() << '\n';
                }
            }
            return 0;
        }

        // --- Fit mode (without derive) ---
        if (fit_mode) {
            auto print_fit = [](const std::string& alias, const FormulaSystem::FitOutput& f) {
                std::string sign = f.exact ? " = " : " ~ ";
                std::cout << alias << sign << f.equation << '\n';
                std::cerr << "  R² = " << fmt_num(f.r_squared)
                          << ", max error = " << fmt_num(f.max_error) << '\n';
            };
            for (auto& q : query.queries) {
                try {
                    auto result = sys.fit(q.variable, query.bindings, query.symbolic);
                    print_fit(q.alias, result);
                    for (auto& alt : result.alternatives)
                        print_fit(q.alias, alt);
                } catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << '\n';
                    return 1;
                }
            }

            // Write .fw file if --output specified
            if (!output_file.empty()) {
                std::ofstream out(output_file);
                if (!out.is_open()) {
                    std::cerr << "Error: cannot write to " << output_file << '\n';
                    return 1;
                }
                out << "# Generated by fwiz --fit\n";
                for (auto& q : query.queries) {
                    try {
                        auto result = sys.fit(q.variable, query.bindings, query.symbolic);
                        out << q.variable << " = " << result.equation << '\n';
                    } catch (...) {}
                }
            }
            return 0;
        }

        // --- Pass 1: solve queries ---
        std::map<std::string, double> solved = query.bindings;

        if (explore) {
            std::vector<std::pair<std::string, std::string>> vars;
            if (explore_full) {
                for (auto& v : sys.all_variables())
                    vars.push_back({v, v});
            } else {
                for (auto& [k, v] : query.bindings)
                    vars.push_back({k, k});
                for (auto& q : query.queries)
                    if (!query.bindings.count(q.variable))
                        vars.push_back({q.variable, q.alias});
            }

            for (auto& [var, alias] : vars) {
                if (solved.count(var)) {
                    std::cout << alias << " = " << fmt_num(solved.at(var)) << '\n';
                } else {
                    try {
                        double result = sys.resolve(var, query.bindings);
                        std::cout << alias << " = " << fmt_num(result) << '\n';
                        solved[var] = result;
                    } catch (...) {
                        std::cout << alias << " = ?\n";
                    }
                }
            }
        } else if (!query.queries.empty()) {
            for (auto& q : query.queries) {
                try {
                    auto eq_sign = [&](const std::string& var) {
                        auto it = sys.numeric_results_.find(var);
                        if (it == sys.numeric_results_.end()) return " = ";
                        return it->second ? " = " : " ~ ";
                    };
                    if (q.strict) {
                        double result = sys.resolve_one(q.variable, query.bindings);
                        std::cout << q.alias << eq_sign(q.variable) << fmt_num(result) << '\n';
                        solved[q.variable] = result;
                    } else {
                        auto result = sys.resolve_all(q.variable, query.bindings);
                        if (result.is_discrete()) {
                            for (auto r : result.discrete())
                                std::cout << q.alias << eq_sign(q.variable) << fmt_num(r) << '\n';
                            if (!result.discrete().empty())
                                solved[q.variable] = result.discrete()[0];
                        } else {
                            std::cout << q.alias << " : " << result.to_string() << '\n';
                        }
                    }
                } catch (const std::exception& e) {
                    if (has_verify) {
                        std::cout << q.alias << " = ?\n";
                    } else {
                        throw;
                    }
                }
            }
        }

        // --- Pass 2: verify ---
        if (has_verify) {
            // Determine which variables to verify
            std::vector<std::string> to_verify;
            if (verify_arg == "all") {
                for (auto& [k, v] : solved)
                    to_verify.push_back(k);
            } else {
                std::istringstream ss(verify_arg);
                std::string var;
                while (std::getline(ss, var, ',')) {
                    var = trim(var);
                    if (!var.empty()) to_verify.push_back(var);
                }
            }

            if (!query.queries.empty())
                std::cout << "---\n";

            int total_checks = 0, total_failed = 0;
            bool any_output = false;

            for (auto& var : to_verify) {
                if (!solved.count(var)) {
                    std::cout << var << ":\n  (not known, skipped)\n";
                    any_output = true;
                    continue;
                }
                auto results = sys.verify_variable(var, solved.at(var), solved);
                if (results.empty()) continue;

                any_output = true;
                std::cout << var << ":\n";
                for (auto& r : results) {
                    total_checks++;
                    if (r.pass) {
                        std::cout << "  " << r.equation_desc << " = "
                                  << fmt_num(r.computed) << " PASS\n";
                    } else {
                        total_failed++;
                        std::cout << "  " << r.equation_desc << " = "
                                  << fmt_num(r.computed) << " FAIL (expected "
                                  << fmt_num(solved.at(var)) << ")\n";
                    }
                }
            }

            if (any_output) {
                std::cout << "---\nVerified: " << to_verify.size() << " variables, "
                          << total_checks << " checks, " << total_failed << " failed\n";
            }

            if (total_failed > 0) return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
