#include "system.h"
#include <iostream>

// Render a solve result. In exact mode (the default), fmt_exact_double runs
// fraction and constant recognition to match --derive output ('pi', '5 / 3',
// etc.). In approximate mode (--approximate, or '~' numeric results), skip
// recognition and emit fmt_num — users asking for a float get a float.
// User-defined aliases (from .fw `name = <num>` defaults) are threaded
// through so solve output surfaces the user's names (e.g. `deg`) instead
// of raw decimals when the numeric value matches.
static std::string fmt_solve_result(double v, bool try_exact,
        const std::map<std::string, double>& aliases = {}) {
    return try_exact ? fmt_exact_double(v, aliases) : fmt_num(v);
}

int main(int argc, const char* argv[]) {
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
                  << "  --cse [N]      extract at most N helpers, ranked by value (default N=3)\n"
                  << "                 as named helpers t1, t2, ... in a # Helpers preamble\n"
                  << "  --no-numeric   disable numeric solving (algebraic only)\n"
                  << "  --approximate  collapse exact output (fractions, pi, etc.) to floating-point\n"
                  << "  --exact        force exact output — default; useful to override --approximate\n"
                  << "  --fit [N]      fit a curve (depth N, default 2: compose templates)\n"
                  << "  --output FILE  write fitted equation to a .fw file\n"
                  << "  --precision N  set sample density (default 200)\n"
                  << "  --table        evaluate across range inputs, emit TSV\n"
                  << "                 ranges: a=[1..10], a=[0..1 @ 0.1], a=[1..5, 7..10]\n"
                  << "  --zip          with --table: zip ranges element-wise (default: cartesian)\n"
                  << "  -I <dir>       add a directory to the @include / cross-file search path\n"
                  << "                 (repeatable; FWIZ_PATH env var dirs are searched after)\n"
                  << "\n"
                  << "Example: fwiz physics(force=?, mass=10)\n"
                  << "         fwiz --explore triangle(a=?, b=?, c=?, A=40, B=80)\n"
                  << "         fwiz --verify all triangle(A=40, B=60, C=80)\n"
                  << "         fwiz --derive triangle(C=?, a=a, b=b, c=c)\n"
                  << "         fwiz --table triangle(C=?, a=[1..10], b=4, c=5)\n"
                  << "         fwiz 'examples/nested_demo(result=?, nested_inner(z=?x, p=3))'\n";
        return 1;
    }

    try {
        TraceLevel level = TraceLevel::NONE;
        bool explore = false;
        bool explore_full = false;
        bool derive_mode = false;
        int derive_cap = 0;  // 0 or negative → unbounded; >= 1 → cap at N
        int cse_threshold = 0;  // 0 → CSE disabled (default); >= 1 → extract at most N helpers (Option C top-N by value)
        bool fit_mode = false;
        int fit_depth = FIT_DEFAULT_DEPTH;
        bool numeric_mode = true;
        bool approximate_mode = false;
        bool table_mode = false;
        bool zip_mode = false;
        int sys_samples = NUMERIC_DEFAULT_SAMPLES;
        std::string verify_arg;
        std::string output_file;
        std::string query_str;
        std::vector<std::string> include_dirs;  // Future #80: -I dirs (order-preserving)

        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if      (arg == "--steps")        level = TraceLevel::STEPS;
            else if (arg == "--calc")         level = TraceLevel::CALC;
            else if (arg == "--explore")      explore = true;
            else if (arg == "--explore-full") { explore = true; explore_full = true; }
            else if (arg == "--derive") {
                derive_mode = true;
                // Optional cap argument: --derive N (non-numeric next arg → flag-only)
                if (i + 1 < argc) {
                    try { derive_cap = std::stoi(argv[i + 1]); i++; }
                    // NOLINTNEXTLINE(bugprone-empty-catch) — not a number; leave for query string
                    catch (const std::invalid_argument&) {}
                    // NOLINTNEXTLINE(bugprone-empty-catch) — value out of range; leave for query string
                    catch (const std::out_of_range&) {}
                }
            }
            else if (arg == "--cse") {
                cse_threshold = 3;  // default when bare
                // Optional threshold: --cse N (non-numeric next arg → flag-only)
                if (i + 1 < argc) {
                    try { cse_threshold = std::stoi(argv[i + 1]); i++; }
                    // NOLINTNEXTLINE(bugprone-empty-catch) — not a number; leave for query string
                    catch (const std::invalid_argument&) {}
                    // NOLINTNEXTLINE(bugprone-empty-catch) — value out of range; leave for query string
                    catch (const std::out_of_range&) {}
                }
            }
            else if (arg == "--fit") {
                fit_mode = true;
                // Optional depth argument: --fit 3 (non-numeric next arg → treat --fit as flag-only)
                if (i + 1 < argc) {
                    try { fit_depth = std::stoi(argv[i + 1]); i++; }
                    // NOLINTNEXTLINE(bugprone-empty-catch) — not a number; leave for query string
                    catch (const std::invalid_argument&) {}
                    // NOLINTNEXTLINE(bugprone-empty-catch) — value out of range; leave for query string
                    catch (const std::out_of_range&) {}
                }
            }
            else if (arg == "--no-numeric")   numeric_mode = false;
            else if (arg == "--approximate")  approximate_mode = true;
            else if (arg == "--exact")        approximate_mode = false;
            else if (arg == "--table")        table_mode = true;
            else if (arg == "--zip")          zip_mode = true;
            else if (arg == "--output") {
                if (i + 1 < argc) output_file = argv[++i];
                else { std::cerr << "Error: --output requires a filename\n"; return 1; }
            }
            else if (arg == "--precision") {
                if (i + 1 < argc) {
                    try { sys_samples = std::stoi(argv[++i]); }
                    catch (const std::invalid_argument&) { std::cerr << "Error: --precision requires a number\n"; return 1; }
                    catch (const std::out_of_range&) { std::cerr << "Error: --precision value out of range\n"; return 1; }
                } else { std::cerr << "Error: --precision requires an argument\n"; return 1; }
            }
            else if (arg == "--verify") {
                if (i + 1 < argc) verify_arg = argv[++i];
                else { std::cerr << "Error: --verify requires an argument (all or var1,var2,...)\n"; return 1; }
            }
            else if (arg == "-I") {
                // Future #80: @include / cross-file search directory (repeatable).
                if (i + 1 < argc) include_dirs.emplace_back(argv[++i]);
                else { std::cerr << "Error: -I requires a directory\n"; return 1; }
            }
            else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
                include_dirs.emplace_back(arg.substr(2));  // attached form: -Idir
            }
            else    { if (!query_str.empty()) query_str += ' '; query_str += arg; }
        }

        if (query_str.empty()) {
            std::cerr << "Error: no query provided\n";
            return 1;
        }

        const bool has_verify = !verify_arg.empty();
        // Future #5: --table is a row-shaped output mode; incompatible with
        // anything that produces non-row-shaped output (symbolic equations,
        // verification report, fit equation, full enumeration). Inverted
        // enumeration (one guard naming all conflicting modes) keeps the
        // matrix linear as new output modes land — adding a new mode only
        // requires extending this single condition.
        if (table_mode) {
            if (derive_mode || has_verify || fit_mode || explore || explore_full) {
                std::cerr << "Error: --table is incompatible with "
                             "--derive/--verify/--fit/--explore\n";
                return 1;
            }
            level = TraceLevel::NONE;  // --steps/--calc would interleave with TSV
        }
        if (zip_mode && !table_mode) {
            std::cerr << "Error: --zip requires --table\n";
            return 1;
        }
        const bool allow_missing = explore || explore_full || has_verify || derive_mode || fit_mode;
        const bool allow_symbolic = derive_mode || fit_mode;

        FormulaSystem sys;
        sys.trace.level = level;
        sys.numeric_mode = numeric_mode;
        sys.approximate_mode = approximate_mode;
        sys.numeric_samples = sys_samples;
        sys.fit_depth = fit_depth;

        // Future #80: @include / cross-file search path. -I dirs first (CLI
        // order), then FWIZ_PATH dirs (split on ':' and ';' for portability),
        // appended after. base_dir (the file's own directory) is always tried
        // before either inside resolve_file_path.
        sys.include_dirs = include_dirs;
        if (const char* fwiz_path = std::getenv("FWIZ_PATH")) {
            const std::string envp = fwiz_path;
            std::string cur;
            for (char ch : envp) {
                if (ch == ':' || ch == ';') {
                    if (!cur.empty()) { sys.include_dirs.push_back(cur); cur.clear(); }
                } else {
                    cur += ch;
                }
            }
            if (!cur.empty()) sys.include_dirs.push_back(cur);
        }

        // ExprArena scope for parse_cli_query is required by Future #21
        // (nested form): a nested-call binding like `inner(p=3)` parses
        // `Expr::Num(3)` into the active arena, and the resulting
        // `FormulaCall` is later moved into `sys.formula_calls`. Allocating
        // those nodes in `sys.arena` ensures they outlive parse_cli_query
        // and remain valid through every subsequent solve / derive pass
        // (load_file / resolve / derive_all all open nested scopes on the
        // same arena).
        const ExprArena::Scope cli_scope(sys.arena);

        auto query = parse_cli_query(query_str, allow_missing, allow_symbolic);

        if (query.filename.empty()) {
            // Query-first format: inline source or stdin
            if (!query.inline_source.empty()) {
                // Replace semicolons with newlines for compact inline format
                std::string source = query.inline_source;
                std::replace(source.begin(), source.end(), ';', '\n');
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

        // Future #67: post-load synthetic equations from CLI sugar
        // (`integral(...)=?[alias]`, `diff(...)=?[alias]`, or binding RHS
        // `<name>=integral(...)` / `<name>=diff(...)`). Loaded as a single
        // chunk; the system's post-load passes (`resolve_diff_in_equations`,
        // `resolve_integral_in_equations`) rewrite them to derivative /
        // antiderivative trees. The dirty-flag mechanism (system.h) handles
        // incremental re-resolution across multiple load_string calls.
        if (!query.synthetic_equations.empty())
            sys.load_string(query.synthetic_equations, "<cli-resolve-at-load>");

        // Future #21 (nested form): inject CLI-supplied nested formula calls
        // into the loaded system. Strategy 3 (FORMULA_FWD) and strategy 5
        // (FORMULA_REV) read `formula_calls` at solve time, so the inner call
        // is resolved through the same path as `.fw`-file-declared calls. The
        // synthetic alias on the inner `=?` (e.g. `inner(z=?x, ...)` produces
        // `output_var = "x"`) routes the inner result into the parent scope.
        for (auto& fc : query.nested_calls)
            sys.formula_calls.push_back(std::move(fc));

        // --- Table mode (Future #5) ---
        if (table_mode) {
            if (query.range_bindings.empty())
                throw std::runtime_error(
                    "--table requires at least one range binding (e.g., a=[1..10])");

            // Cartesian-product row-count soft cap. Warn-and-continue (NOT a
            // hard error) — a user requesting `[1..1000] x [1..1000]` already
            // knows they want 1M rows. `--table-max-rows N` is a parked
            // Future.md item (reopen trigger: friction report).
            constexpr size_t TABLE_WARN_ROWS = 1'000'000;
            if (!zip_mode) {
                size_t total_rows = 1;
                bool too_large = false;
                for (const auto& rb : query.range_bindings) {
                    if (rb.second.empty()) break;  // any_empty handled below — no warning needed
                    if (total_rows > TABLE_WARN_ROWS / rb.second.size()) {
                        too_large = true;
                        break;
                    }
                    total_rows *= rb.second.size();
                }
                if (too_large)
                    std::cerr << "Warning: --table cartesian product is large "
                                 "(> " << TABLE_WARN_ROWS << " rows)\n";
            }

            // Zip mismatch warning (Python/numpy zip() semantics — truncate to min).
            if (zip_mode) {
                size_t min_len = query.range_bindings[0].second.size();
                size_t max_len = min_len;
                for (const auto& rb : query.range_bindings) {
                    min_len = std::min(min_len, rb.second.size());
                    max_len = std::max(max_len, rb.second.size());
                }
                if (min_len != max_len)
                    std::cerr << "Warning: --zip ranges have different lengths ("
                              << min_len << " vs " << max_len
                              << "); truncating to " << min_len << " rows\n";
            }

            // Output stream: stdout default; --output FILE redirects.
            std::ostream* outp = &std::cout;
            std::ofstream out_file;
            if (!output_file.empty()) {
                out_file.open(output_file);
                if (!out_file.is_open()) {
                    std::cerr << "Error: cannot write to " << output_file << '\n';
                    return 1;
                }
                outp = &out_file;
            }
            std::ostream& out = *outp;

            // fmt_exact_double allocates Expr nodes (constant recognition)
            // into the arena; one scope wraps every row.
            const ExprArena::Scope table_fmt_scope(sys.arena);
            sys.populate_aliases_();

            // Header row: range vars (CLI order) then query aliases (CLI order).
            // Tab-separated; deterministic shape per output stream contract.
            {
                bool first = true;
                for (const auto& rb : query.range_bindings) {
                    if (!first) out << '\t';
                    first = false;
                    out << rb.first;
                }
                for (const auto& qv : query.queries)
                    out << '\t' << qv.alias;
                out << '\n';
            }

            // Emit one TSV row given an index vector into range_bindings.
            // numeric_memo_ keys on target + serialized bindings, so different
            // row values produce different keys — safe across rows.
            auto emit_row = [&](const std::vector<double>& row_vals) {
                auto bindings_copy = query.bindings;
                for (size_t i = 0; i < query.range_bindings.size(); i++)
                    bindings_copy[query.range_bindings[i].first] = row_vals[i];

                bool first = true;
                for (const double v : row_vals) {
                    if (!first) out << '\t';
                    first = false;
                    out << fmt_solve_result(v, !approximate_mode, sys.aliases_);
                }

                for (const auto& qv : query.queries) {
                    out << '\t';
                    try {
                        auto result = sys.resolve_all(qv.variable, bindings_copy);
                        if (result.is_discrete() && !result.discrete().empty()) {
                            const auto it = sys.numeric_results_.find(qv.variable);
                            const bool exact = (it == sys.numeric_results_.end()) || it->second;
                            out << fmt_solve_result(result.discrete()[0],
                                                    exact && !approximate_mode,
                                                    sys.aliases_);
                        } else {
                            out << '?';  // no discrete result (range, periodic, or empty)
                        }
                    } catch (const std::exception&) {
                        // Intentionally broad: this is the per-row SOLVE-phase
                        // handler. Load-phase fatals (sibling exceptions like
                        // RaggedMatrixError / CrossFileResolutionCycleError —
                        // see parser.h / system.h) have already propagated
                        // before table iteration begins, so the only thing
                        // reaching here is solve failure for this row's input.
                        // If a future sibling exception ever fires at resolve
                        // time, this catch will silently render `?` — promote
                        // to a narrower runtime_error catch at that point.
                        out << '?';
                    }
                }
                out << '\n';
            };

            if (zip_mode) {
                const size_t n = std::accumulate(
                    query.range_bindings.begin(), query.range_bindings.end(),
                    query.range_bindings[0].second.size(),
                    [](size_t acc, const auto& rb) {
                        return std::min(acc, rb.second.size());
                    });
                for (size_t i = 0; i < n; i++) {
                    std::vector<double> row;
                    row.reserve(query.range_bindings.size());
                    std::transform(
                        query.range_bindings.begin(), query.range_bindings.end(),
                        std::back_inserter(row),
                        [i](const auto& rb) { return rb.second[i]; });
                    emit_row(row);
                }
            } else {
                // Cartesian product via odometer: rightmost index advances
                // fastest (matches MATLAB meshgrid / Mathematica Table column
                // order). Any empty range collapses the product to zero rows.
                const bool any_empty = std::any_of(
                    query.range_bindings.begin(), query.range_bindings.end(),
                    [](const auto& rb) { return rb.second.empty(); });
                if (!any_empty) {
                    std::vector<size_t> idx(query.range_bindings.size(), 0);
                    bool done = false;
                    while (!done) {
                        std::vector<double> row;
                        row.reserve(query.range_bindings.size());
                        for (size_t i = 0; i < query.range_bindings.size(); i++)
                            row.push_back(query.range_bindings[i].second[idx[i]]);
                        emit_row(row);

                        done = true;
                        for (size_t k = query.range_bindings.size(); k-- > 0;) {
                            if (++idx[k] < query.range_bindings[k].second.size()) {
                                done = false;
                                break;
                            }
                            idx[k] = 0;
                        }
                    }
                }
            }
            return 0;
        }

        // --- Derive mode ---
        if (derive_mode) {
            std::map<std::string, std::set<std::string>> derived_eqs;
            for (const auto& q : query.queries) {
                try {
                    std::vector<std::string> helpers;
                    const bool cse_active = cse_threshold >= 1;
                    auto results = sys.derive_all(
                        q.variable, query.bindings, query.symbolic,
                        cse_active ? &helpers : nullptr,
                        cse_active ? cse_threshold : 0,
                        derive_cap);
                    if (cse_active && !helpers.empty()) {
                        std::cout << "# Helpers\n";
                        for (const auto& h : helpers) std::cout << h << '\n';
                        std::cout << '\n';
                    }
                    for (auto& r : results) {
                        derived_eqs[q.variable].insert(r);
                        std::cout << q.alias << " = " << r << '\n';
                    }
                } catch (const std::exception& e) {
                    if (!fit_mode) {
                        std::cerr << "Error: " << e.what() << '\n';
                        return 1;
                    }
                }
            }
            if (!fit_mode) return 0;

            // --- Fit after derive ---
            for (const auto& q : query.queries) {
                try {
                    auto result = sys.fit(q.variable, query.bindings, query.symbolic);
                    auto print_if_new = [&](const FormulaSystem::FitOutput& f) {
                        if (derived_eqs.count(q.variable) && derived_eqs[q.variable].count(f.equation))
                            return;
                        const std::string sign = f.exact ? " = " : " ~ ";
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
                const std::string sign = f.exact ? " = " : " ~ ";
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
                for (const auto& q : query.queries) {
                    try {
                        auto result = sys.fit(q.variable, query.bindings, query.symbolic);
                        out << q.variable << " = " << result.equation << '\n';
                    // NOLINTNEXTLINE(bugprone-empty-catch) — fit failure for one var → skip, continue with others
                    } catch (const std::runtime_error&) {}
                }
            }
            return 0;
        }

        // --- Pass 1: solve queries ---
        std::map<std::string, double> solved = query.bindings;

        // fmt_exact_double allocates Expr nodes into the arena (for constant
        // recognition); require an active Scope around the output section.
        const ExprArena::Scope solve_fmt_scope(sys.arena);

        // User-defined aliases surface in exact-mode solve output. Populated
        // by `populate_aliases_()` (called from resolve()/resolve_all() entry
        // points and explicitly here for the explore-fast-path branch where a
        // queried var is already in `solved` and skips resolve()).
        sys.populate_aliases_();

        if (explore) {
            std::vector<std::pair<std::string, std::string>> vars;
            if (explore_full) {
                auto all_vars = sys.all_variables();
                std::transform(all_vars.begin(), all_vars.end(), std::back_inserter(vars),
                    [](const std::string& v) { return std::make_pair(v, v); });
            } else {
                for (auto& [k, v] : query.bindings)
                    vars.push_back({k, k});
                for (auto& q : query.queries)
                    if (!query.bindings.count(q.variable))
                        vars.push_back({q.variable, q.alias});
            }

            for (auto& [var, alias] : vars) {
                if (solved.count(var)) {
                    std::cout << alias << " = " << fmt_solve_result(solved.at(var), !approximate_mode, sys.aliases_) << '\n';
                } else {
                    try {
                        const double result = sys.resolve(var, query.bindings);
                        std::cout << alias << " = " << fmt_solve_result(result, !approximate_mode, sys.aliases_) << '\n';
                        solved[var] = result;
                    } catch (const std::runtime_error&) {
                        std::cout << alias << " = ?\n";
                    }
                }
            }
        } else if (!query.queries.empty()) {
            for (auto& q : query.queries) {
                try {
                    // Exactness drives both the '=' vs '~' sign AND whether
                    // structural-fraction display is attempted. Source of truth:
                    // numeric_results_ — absent OR true means exact.
                    auto is_exact_result = [&](const std::string& var) {
                        auto it = sys.numeric_results_.find(var);
                        return it == sys.numeric_results_.end() || it->second;
                    };
                    if (q.strict) {
                        const double result = sys.resolve_one(q.variable, query.bindings);
                        const bool exact = is_exact_result(q.variable);
                        std::cout << q.alias << (exact ? " = " : " ~ ")
                                  << fmt_solve_result(result, exact && !approximate_mode, sys.aliases_) << '\n';
                        solved[q.variable] = result;
                    } else {
                        auto result = sys.resolve_all(q.variable, query.bindings);
                        if (result.is_discrete()) {
                            const bool exact = is_exact_result(q.variable);
                            for (auto r : result.discrete())
                                std::cout << q.alias << (exact ? " = " : " ~ ")
                                          << fmt_solve_result(r, exact && !approximate_mode, sys.aliases_) << '\n';
                            if (!result.discrete().empty())
                                solved[q.variable] = result.discrete()[0];
                        } else if (result.has_periodic()) {
                            // 12h: periodic families render per-line with '=' / '~'
                            // separator (same as discrete), one line per dedup'd
                            // family. Dedup via ValueSet::periodic_render_lines().
                            const bool exact = is_exact_result(q.variable);
                            for (const auto& line : result.periodic_render_lines())
                                std::cout << q.alias << (exact ? " = " : " ~ ") << line << '\n';
                        } else {
                            std::cout << q.alias << " : " << result.to_string() << '\n';
                        }
                    }
                } catch (const std::exception& e) {
                    // Future #67: free-variable case for synthetic-equation
                    // aliases (diff/integral CLI sugar). Print the
                    // (post-load-rewritten) symbolic RHS so users still get
                    // an answer like `slope = velocity` instead of a thrown
                    // error. Matches the pre-unification Pass 1.5/1.6
                    // fallback behaviour.
                    if (query.synthetic_aliases.count(q.alias)) {
                        bool emitted = false;
                        for (const auto& eq : sys.equations) {
                            // cppcheck-suppress useStlAlgorithm
                            if (eq.lhs_var == q.alias) {
                                std::cout << q.alias << " = "
                                          << expr_to_string(eq.rhs) << '\n';
                                emitted = true;
                                break;
                            }
                        }
                        if (!emitted) std::cout << q.alias << " = ?\n";
                    } else if (has_verify) {
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

            for (const auto& var : to_verify) {
                if (!solved.count(var)) {
                    std::cout << var << ":\n  (not known, skipped)\n";
                    any_output = true;
                    continue;
                }
                auto results = sys.verify_variable(var, solved.at(var), solved);
                if (results.empty()) continue;

                any_output = true;
                std::cout << var << ":\n";
                for (const auto& r : results) {
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

    } catch (const SolveBudgetExceededError& e) {
        // Budget sentinel — distinct exit code so library users / scripts can
        // differentiate from regular solve failures.
        std::cerr << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
