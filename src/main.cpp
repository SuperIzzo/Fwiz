#include "system.h"
#include <iostream>
#include <iomanip>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: fwiz [--steps|--calc] <formula>(var=?, var=?alias, var=value, ...)\n"
                  << "\n"
                  << "  var=?        solve for var\n"
                  << "  var=?alias   solve for var, output as alias\n"
                  << "  --steps      show algebraic reasoning\n"
                  << "  --calc       show steps + numeric evaluation detail\n"
                  << "\n"
                  << "Example: fwiz physics(force=?, mass=10)\n"
                  << "         fwiz physics(force=?f, distance=?d, mass=10, speed=60, time=2)\n";
        return 1;
    }

    try {
        TraceLevel level = TraceLevel::NONE;
        std::string query_str;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if      (arg == "--steps") level = TraceLevel::STEPS;
            else if (arg == "--calc")  level = TraceLevel::CALC;
            else    { if (!query_str.empty()) query_str += ' '; query_str += arg; }
        }

        if (query_str.empty()) {
            std::cerr << "Error: no query provided\n";
            return 1;
        }

        auto query = parse_cli_query(query_str);
        FormulaSystem sys;
        sys.trace.level = level;
        sys.load_file(query.filename);

        for (auto& [var, alias] : query.queries) {
            double result = sys.resolve(var, query.bindings);
            std::cout << alias << " = " << fmt_num(result) << '\n';
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
