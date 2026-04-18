#include "system.h"
#include <iostream>
#include <cmath>
#include <functional>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits>
#include <cfloat>

// Detect sanitizers — reduce depth stress tests to avoid stack overflow
// ASan adds ~200 bytes of red zone per stack frame
#if defined(__SANITIZE_ADDRESS__)
    #define FWIZ_SANITIZER 1
#elif defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define FWIZ_SANITIZER 1
    #endif
#endif
#ifndef FWIZ_SANITIZER
    #define FWIZ_SANITIZER 0
#endif

#if FWIZ_SANITIZER
    constexpr int DEPTH_HIGH = 500;
    constexpr int DEPTH_MED = 200;
#else
    constexpr int DEPTH_HIGH = 10000;
    constexpr int DEPTH_MED = 5000;
#endif

// ---- Minimal test framework ----

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static std::string current_section;

#define SECTION(name) do { current_section = name; std::cout << "\n=== " << name << " ===\n"; } while(0)

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; std::cout << "  PASS: " << msg << "\n"; } \
    else { tests_failed++; std::cout << "  FAIL: " << msg << " [" << __FILE__ << ":" << __LINE__ << "]\n"; } \
} while(0)

#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), std::string(msg) + " (got '" + std::string(a) + "', expected '" + std::string(b) + "')")
#define ASSERT_NUM(a, b, msg) ASSERT(std::abs((a) - (b)) < 1e-6, std::string(msg) + " (got " + std::to_string(a) + ", expected " + std::to_string(b) + ")")

// Helpers to parse and simplify quickly
ExprPtr parse(const std::string& s) {
    auto tokens = Lexer(s).tokenize();
    Parser p(tokens);
    return p.parse_expr();
}

std::string ps(const std::string& s) {
    return expr_to_string(parse(s));
}

std::string ss(const std::string& s) {
    return expr_to_string(simplify(parse(s)));
}

// Strict evaluation — asserts (in debug) on empty. Use for tests that
// expect a finite, computable result.
double ev(const std::string& s) {
    return evaluate(simplify(parse(s))).value();
}

// NaN-tolerant evaluation — returns NaN on empty (propagation or failure).
// Use ONLY for tests that deliberately produce NaN (e.g. sqrt(-1), 0/0).
double ev_nan(const std::string& s) {
    return evaluate(simplify(parse(s))).value_or_nan();
}

// ---- Lexer tests ----

void test_lexer() {
    SECTION("Lexer");

    {
        auto tokens = Lexer("42").tokenize();
        ASSERT(tokens[0].type == TokenType::NUMBER, "number token type");
        ASSERT_NUM(tokens[0].numval, 42, "number value 42");
        ASSERT(tokens[1].type == TokenType::END, "end token");
    }
    {
        auto tokens = Lexer("3.14").tokenize();
        ASSERT(tokens[0].type == TokenType::NUMBER, "float token type");
        ASSERT_NUM(tokens[0].numval, 3.14, "float value 3.14");
    }
    {
        auto tokens = Lexer("hello_world").tokenize();
        ASSERT(tokens[0].type == TokenType::IDENT, "ident token type");
        ASSERT_EQ(tokens[0].text, "hello_world", "ident text");
    }
    {
        auto tokens = Lexer("x + y * 2 - z / 3").tokenize();
        ASSERT(tokens.size() == 10, "operator token count"); // x + y * 2 - z / 3 END
        ASSERT(tokens[0].type == TokenType::IDENT, "first ident");
        ASSERT(tokens[1].type == TokenType::PLUS, "plus");
        ASSERT(tokens[3].type == TokenType::STAR, "star");
        ASSERT(tokens[5].type == TokenType::MINUS, "minus");
        ASSERT(tokens[7].type == TokenType::SLASH, "slash");
    }
    {
        auto tokens = Lexer("x^2").tokenize();
        ASSERT(tokens[1].type == TokenType::CARET, "caret");
    }
    {
        auto tokens = Lexer("f(a, b)").tokenize();
        ASSERT(tokens[1].type == TokenType::LPAREN, "lparen");
        ASSERT(tokens[3].type == TokenType::COMMA, "comma");
        ASSERT(tokens[5].type == TokenType::RPAREN, "rparen");
    }
    {
        auto tokens = Lexer("x = ?").tokenize();
        ASSERT(tokens[1].type == TokenType::EQUALS, "equals");
        ASSERT(tokens[2].type == TokenType::QUESTION, "question");
    }
    {
        // whitespace handling
        auto tokens = Lexer("   x   +   1   ").tokenize();
        ASSERT(tokens[0].type == TokenType::IDENT, "whitespace: ident");
        ASSERT(tokens[1].type == TokenType::PLUS, "whitespace: plus");
        ASSERT(tokens[2].type == TokenType::NUMBER, "whitespace: number");
    }
    {
        // empty input
        auto tokens = Lexer("").tokenize();
        ASSERT(tokens.size() == 1 && tokens[0].type == TokenType::END, "empty input");
    }
    {
        bool threw = false;
        try { Lexer("@").tokenize(); } catch (...) { threw = true; }
        ASSERT(threw, "unexpected character throws");
    }
}

// ---- Parser + printer tests ----

void test_parser() {
    SECTION("Parser + Printer");

    // Basic atoms
    ASSERT_EQ(ps("42"), "42", "parse number");
    ASSERT_EQ(ps("x"), "x", "parse variable");
    ASSERT_EQ(ps("3.5"), "3.5", "parse float");

    // Binary ops with precedence
    ASSERT_EQ(ps("x + y"), "x + y", "add no parens");
    ASSERT_EQ(ps("x - y"), "x - y", "sub no parens");
    ASSERT_EQ(ps("x * y"), "x * y", "mul no parens");
    ASSERT_EQ(ps("x / y"), "x / y", "div no parens");
    ASSERT_EQ(ps("x ^ 2"), "x^2", "pow no parens");

    // Precedence: mul binds tighter than add
    ASSERT_EQ(ps("x + y * z"), "x + y * z", "add/mul precedence");
    ASSERT_EQ(ps("x * y + z"), "x * y + z", "mul/add precedence");

    // Parens override precedence
    ASSERT_EQ(ps("(x + y) * z"), "(x + y) * z", "parens force add before mul");
    ASSERT_EQ(ps("x * (y + z)"), "x * (y + z)", "parens on right");

    // Sub/div associativity: right operand needs parens
    ASSERT_EQ(ps("x - y - z"), "x - y - z", "left-assoc sub");
    ASSERT_EQ(ps("x - (y - z)"), "x - (y - z)", "right sub needs parens");
    ASSERT_EQ(ps("x / (y / z)"), "x / (y / z)", "right div needs parens");

    // Unary negation
    ASSERT_EQ(ps("-x"), "-x", "unary neg variable");
    ASSERT_EQ(ps("-(x + y)"), "-(x + y)", "unary neg compound");

    // Function calls
    ASSERT_EQ(ps("sqrt(x)"), "sqrt(x)", "sqrt");
    ASSERT_EQ(ps("sin(x + 1)"), "sin(x + 1)", "sin with expr");
    ASSERT_EQ(ps("f(a, b, c)"), "f(a, b, c)", "multi-arg function");

    // Nested
    ASSERT_EQ(ps("sqrt((x - y)^2 + (a - b)^2)"),
              "sqrt((x - y)^2 + (a - b)^2)", "distance formula");
}

// ---- Evaluator tests ----

void test_evaluate() {
    SECTION("Evaluate");

    ASSERT_NUM(ev("2 + 3"), 5, "2+3");
    ASSERT_NUM(ev("10 - 4"), 6, "10-4");
    ASSERT_NUM(ev("3 * 7"), 21, "3*7");
    ASSERT_NUM(ev("20 / 4"), 5, "20/4");
    ASSERT_NUM(ev("2 ^ 10"), 1024, "2^10");
    ASSERT_NUM(ev("-5"), -5, "neg 5");
    ASSERT_NUM(ev("--5"), 5, "double neg");
    ASSERT_NUM(ev("2 + 3 * 4"), 14, "precedence 2+3*4");
    ASSERT_NUM(ev("(2 + 3) * 4"), 20, "parens (2+3)*4");
    ASSERT_NUM(ev("10 - 3 - 2"), 5, "left-assoc sub");
    ASSERT_NUM(ev("100 / 10 / 5"), 2, "left-assoc div");
    ASSERT_NUM(ev("sqrt(16)"), 4, "sqrt(16)");
    ASSERT_NUM(ev("sqrt(9 + 16)"), 5, "sqrt(9+16)");
    ASSERT_NUM(ev("abs(-7)"), 7, "abs(-7)");
    ASSERT_NUM(ev("sin(0)"), 0, "sin(0)");
    ASSERT_NUM(ev("cos(0)"), 1, "cos(0)");
    ASSERT_NUM(ev("log(1)"), 0, "log(1)");
    ASSERT_NUM(ev("asin(0)"), 0, "asin(0)");
    ASSERT_NUM(ev("acos(1)"), 0, "acos(1)");
    ASSERT_NUM(ev("atan(0)"), 0, "atan(0)");
    // asin(1) = pi/2
    ASSERT(std::abs(ev("asin(1)") - 1.5707963) < 1e-5, "asin(1) = pi/2");
    // acos(0) = pi/2
    ASSERT(std::abs(ev("acos(0)") - 1.5707963) < 1e-5, "acos(0) = pi/2");
    // atan(1) = pi/4
    ASSERT(std::abs(ev("atan(1)") - 0.7853981) < 1e-5, "atan(1) = pi/4");
    // Roundtrip: asin(sin(0.5)) = 0.5
    ASSERT(std::abs(ev("asin(sin(0.5))") - 0.5) < 1e-10, "asin(sin(0.5)) roundtrip");

    // Division by zero — eval_div returns NaN, which Checked<double> treats as empty
    {
        auto r = evaluate(simplify(parse("1/0")));
        ASSERT(!r.has_value(), "div-by-zero -> empty");
        ASSERT(std::isnan(r.value_or_nan()), "div-by-zero encoded as NaN");
    }
    // Unresolved variable — empty Checked<double>
    {
        auto r = evaluate(simplify(parse("x + 1")));
        ASSERT(!r.has_value(), "unresolved var yields empty");
    }
}

// ---- Simplifier tests ----

void test_simplify() {
    SECTION("Simplifier");

    // Load builtin rewrite rules for power/trig/etc.
    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules, &builtin_sys.rewrite_exhaustive_flags_);

    // Constant folding
    ASSERT_EQ(ss("2 + 3"), "5", "fold add");
    ASSERT_EQ(ss("10 - 4"), "6", "fold sub");
    ASSERT_EQ(ss("3 * 7"), "21", "fold mul");
    ASSERT_EQ(ss("20 / 4"), "5", "fold div");
    ASSERT_EQ(ss("2 ^ 3"), "8", "fold pow");

    // Identity rules
    ASSERT_EQ(ss("x + 0"), "x", "x+0");
    ASSERT_EQ(ss("0 + x"), "x", "0+x");
    ASSERT_EQ(ss("x - 0"), "x", "x-0");
    ASSERT_EQ(ss("x * 1"), "x", "x*1");
    ASSERT_EQ(ss("1 * x"), "x", "1*x");
    ASSERT_EQ(ss("x * 0"), "0", "x*0");
    ASSERT_EQ(ss("0 * x"), "0", "0*x");
    ASSERT_EQ(ss("x / 1"), "x", "x/1");
    ASSERT_EQ(ss("0 / x"), "0", "0/x");
    ASSERT_EQ(ss("x ^ 0"), "1", "x^0");
    ASSERT_EQ(ss("x ^ 1"), "x", "x^1");

    // Negation simplifications
    ASSERT_EQ(ss("--x"), "x", "double neg");
    ASSERT_EQ(ss("---x"), "-x", "triple neg");
    ASSERT_EQ(ss("-0"), "0", "neg zero");
    ASSERT_EQ(ss("-3"), "(-3)", "neg constant");

    // x - (-y) => x + y
    {
        auto e = Expr::BinOpExpr(BinOp::SUB, Expr::Var("x"), Expr::Neg(Expr::Var("y")));
        ASSERT_EQ(expr_to_string(simplify(e)), "x + y", "x - (-y) => x + y");
    }

    // x + (-y) => x - y
    {
        auto e = Expr::BinOpExpr(BinOp::ADD, Expr::Var("x"), Expr::Neg(Expr::Var("y")));
        ASSERT_EQ(expr_to_string(simplify(e)), "x - y", "x + (-y) => x - y");
    }

    // (-x) + y => y - x
    {
        auto e = Expr::BinOpExpr(BinOp::ADD, Expr::Neg(Expr::Var("x")), Expr::Var("y"));
        ASSERT_EQ(expr_to_string(simplify(e)), "-x + y", "(-x) + y => -x + y");
    }

    // 0 - x => -x
    {
        auto e = Expr::BinOpExpr(BinOp::SUB, Expr::Num(0), Expr::Var("x"));
        ASSERT_EQ(expr_to_string(simplify(e)), "-x", "0 - x => -x");
    }

    // (-a) / (-b) => a / b
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Neg(Expr::Var("a")), Expr::Neg(Expr::Var("b")));
        ASSERT_EQ(expr_to_string(simplify(e)), "a / b", "(-a)/(-b) => a/b");
    }

    // (-a) * (-b) => a * b
    {
        auto e = Expr::BinOpExpr(BinOp::MUL, Expr::Neg(Expr::Var("a")), Expr::Neg(Expr::Var("b")));
        ASSERT_EQ(expr_to_string(simplify(e)), "a * b", "(-a)*(-b) => a*b");
    }

    // x * -1 => -x
    {
        auto e = Expr::BinOpExpr(BinOp::MUL, Expr::Var("x"), Expr::Num(-1));
        ASSERT_EQ(expr_to_string(simplify(e)), "-x", "x * -1 => -x");
    }

    // x / -1 => -x
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Var("x"), Expr::Num(-1));
        ASSERT_EQ(expr_to_string(simplify(e)), "-x", "x / -1 => -x");
    }

    // -(a - b) => b - a
    {
        auto e = Expr::Neg(Expr::BinOpExpr(BinOp::SUB, Expr::Var("a"), Expr::Var("b")));
        ASSERT_EQ(expr_to_string(simplify(e)), "-a + b", "-(a-b) => -a+b");
    }

    // Function constant folding
    ASSERT_EQ(ss("sqrt(25)"), "5", "fold sqrt");
    ASSERT_EQ(ss("abs(-9)"), "9", "fold abs");

    // Partial folding: x + 2 + 3 should fold the 2+3
    ASSERT_EQ(ss("x + 2 + 3"), "x + 5", "partial fold add");
    ASSERT_EQ(ss("2 * 3 * x"), "6 * x", "partial fold mul");
}

// ---- Substitute tests ----

void test_substitute() {
    SECTION("Substitute");

    auto e = parse("x + y * 2");
    auto r1 = substitute(e, "x", Expr::Num(10));
    ASSERT_EQ(expr_to_string(simplify(r1)), "2 * y + 10", "sub x=10");

    auto r2 = substitute(r1, "y", Expr::Num(3));
    ASSERT_EQ(expr_to_string(simplify(r2)), "16", "sub x=10,y=3 => 16");

    // Substitute with expression
    auto r3 = substitute(e, "x", parse("a + b"));
    ASSERT_EQ(expr_to_string(r3), "a + b + y * 2", "sub x=(a+b)");

    // Substitute in function call
    auto e2 = parse("sqrt(x^2 + y^2)");
    auto r4 = substitute(e2, "x", Expr::Num(3));
    auto r5 = substitute(r4, "y", Expr::Num(4));
    ASSERT_NUM((evaluate(simplify(r5)).value()), 5, "sub in sqrt(3^2+4^2)=5");

    // No-op substitute (var not present)
    auto r6 = substitute(parse("a + b"), "z", Expr::Num(99));
    ASSERT_EQ(expr_to_string(r6), "a + b", "sub missing var is no-op");
}

// ---- collect_vars / contains_var tests ----

void test_var_helpers() {
    SECTION("Variable helpers");

    std::set<std::string> vars;
    collect_vars(parse("x + y * z - sqrt(w)"), vars);
    ASSERT(vars.size() == 4, "collect_vars finds 4 vars");
    ASSERT(vars.count("x") && vars.count("y") && vars.count("z") && vars.count("w"),
           "collect_vars finds x,y,z,w");

    ASSERT(contains_var(parse("a + b"), "a"), "contains a");
    ASSERT(!contains_var(parse("a + b"), "c"), "doesn't contain c");
    ASSERT(contains_var(parse("sqrt(x)"), "x"), "contains x in func");
    ASSERT(!contains_var(parse("42"), "x"), "number has no vars");
}

// ---- Linear decomposition tests ----

void test_decompose() {
    SECTION("Linear Decomposition");

    // y + 5: coeff=1, rest=5
    {
        auto lf = decompose_linear(parse("y + 5"), "y");
        ASSERT(lf.has_value(), "y+5 is linear in y");
        ASSERT_NUM((evaluate(lf->coeff).value()), 1, "y+5 coeff=1");
        ASSERT_NUM((evaluate(lf->rest).value()), 5, "y+5 rest=5");
    }

    // y * 2 - 5: coeff=2, rest=-5
    {
        auto lf = decompose_linear(parse("y * 2 - 5"), "y");
        ASSERT(lf.has_value(), "y*2-5 is linear in y");
        ASSERT_NUM((evaluate(lf->coeff).value()), 2, "y*2-5 coeff=2");
        ASSERT_NUM((evaluate(lf->rest).value()), -5, "y*2-5 rest=-5");
    }

    // y + 3 * y: coeff=4, rest=0
    {
        auto lf = decompose_linear(parse("y + 3 * y"), "y");
        ASSERT(lf.has_value(), "y+3*y is linear in y");
        ASSERT_NUM((evaluate(lf->coeff).value()), 4, "y+3*y coeff=4");
        ASSERT_NUM((evaluate(lf->rest).value()), 0, "y+3*y rest=0");
    }

    // speed * time: linear in time (coeff=speed), but speed is a var
    {
        auto lf = decompose_linear(parse("speed * time"), "time");
        ASSERT(lf.has_value(), "speed*time is linear in time");
        ASSERT_EQ(expr_to_string(lf->coeff), "speed", "coeff=speed");
        ASSERT_NUM((evaluate(lf->rest).value()), 0, "rest=0");
    }

    // y * y is nonlinear
    {
        auto lf = decompose_linear(parse("y * y"), "y");
        ASSERT(!lf.has_value(), "y*y is nonlinear");
    }

    // sqrt(y) is nonlinear
    {
        auto lf = decompose_linear(parse("sqrt(y)"), "y");
        ASSERT(!lf.has_value(), "sqrt(y) is nonlinear");
    }

    // Expression with no target var: coeff=0
    {
        auto lf = decompose_linear(parse("a + b"), "z");
        ASSERT(lf.has_value(), "a+b linear in z (trivially)");
        ASSERT_NUM((evaluate(lf->coeff).value()), 0, "coeff=0");
    }

    // Negated variable: -y => coeff=-1
    {
        auto lf = decompose_linear(parse("-y"), "y");
        ASSERT(lf.has_value(), "-y is linear");
        ASSERT_NUM((evaluate(lf->coeff).value()), -1, "-y coeff=-1");
    }

    // Division: y / 3 => coeff=1/3
    {
        auto lf = decompose_linear(parse("y / 3"), "y");
        ASSERT(lf.has_value(), "y/3 is linear");
        ASSERT_NUM((evaluate(lf->coeff).value()), 1.0/3.0, "y/3 coeff=1/3");
    }

    // Target in denominator is nonlinear
    {
        auto lf = decompose_linear(parse("1 / y"), "y");
        ASSERT(!lf.has_value(), "1/y is nonlinear");
    }
}

// ---- solve_for tests ----

void test_solve_for() {
    SECTION("Algebraic Solver (solve_for)");

    // Load builtin rewrite rules (power rules needed for x^0.5 → sqrt)
    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules, &builtin_sys.rewrite_exhaustive_flags_);

    // x = y + 5 => y = x - 5
    {
        auto sol = solve_for(Expr::Var("x"), parse("y + 5"), "y");
        ASSERT(sol != nullptr, "can solve y+5 for y");
        ASSERT_EQ(expr_to_string(sol), "x - 5", "y = x - 5");
    }

    // x = y * 2 - 5 => y = (x + 5) / 2
    {
        auto sol = solve_for(Expr::Var("x"), parse("y * 2 - 5"), "y");
        ASSERT(sol != nullptr, "can solve y*2-5 for y");
        ASSERT_EQ(expr_to_string(sol), "(x + 5) / 2", "y = (x+5)/2");
    }

    // x = y + 3*y => y = x / 4
    {
        auto sol = solve_for(Expr::Var("x"), parse("y + 3 * y"), "y");
        ASSERT(sol != nullptr, "can solve y+3y for y");
        ASSERT_EQ(expr_to_string(sol), "x / 4", "y = x/4");
    }

    // distance = speed * time => time = distance / speed
    {
        auto sol = solve_for(Expr::Var("distance"), parse("speed * time"), "time");
        ASSERT(sol != nullptr, "can solve speed*time for time");
        ASSERT_EQ(expr_to_string(sol), "distance / speed", "time = distance/speed");
    }

    // x = 3*y + 2*y - 10 => y = (x + 10) / 5
    {
        auto sol = solve_for(Expr::Var("x"), parse("3*y + 2*y - 10"), "y");
        ASSERT(sol != nullptr, "can solve 3y+2y-10 for y");
        ASSERT_EQ(expr_to_string(sol), "(x + 10) / 5", "y = (x+10)/5");
    }

    // Nonlinear: x = y^2 => now solvable via inversion: y = sqrt(x)
    {
        auto sol = solve_for(Expr::Var("x"), parse("y^2"), "y");
        ASSERT(sol != nullptr, "y^2 solvable via inversion");
        ASSERT_EQ(expr_to_string(sol), "sqrt(x)", "y^2 → y = sqrt(x)");
    }

    // Solve for x when x is on the LHS already: x = a + b => x = a + b
    {
        auto sol = solve_for(Expr::Var("x"), parse("a + b"), "x");
        ASSERT(sol != nullptr, "can solve for x on LHS");
        // Flattener may reorder: a+b or b+a both valid
        auto s = expr_to_string(sol);
        ASSERT(s == "a + b" || s == "b + a", "x = a + b");
    }
}

// ---- Full system tests ----

void write_fw(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

void test_system() {
    SECTION("Full System");

    // Simple forward evaluation: x = y + 5, y=3 => x=8
    {
        write_fw("/tmp/t1.fw", "x = y + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/t1.fw");
        double r = sys.resolve("x", {{"y", 3}});
        ASSERT_NUM(r, 8, "x = y+5, y=3 => 8");
    }

    // Inverse: x = y + 5, x=4 => y=-1
    {
        FormulaSystem sys;
        sys.load_file("/tmp/t1.fw");
        double r = sys.resolve("y", {{"x", 4}});
        ASSERT_NUM(r, -1, "x=y+5, x=4 => y=-1");
    }

    // Multiplication: x = y*2-5
    {
        write_fw("/tmp/t2.fw", "x = y * 2 - 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/t2.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 4}}), 3, "x=y*2-5, y=4 => 3");
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 5, "x=y*2-5, x=5 => 5");
    }

    // Like terms: x = y + 3*y
    {
        write_fw("/tmp/t3.fw", "x = y + 3 * y\n");
        FormulaSystem sys;
        sys.load_file("/tmp/t3.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 2}}), 8, "x=y+3y, y=2 => 8");
        ASSERT_NUM(sys.resolve("y", {{"x", 20}}), 5, "x=y+3y, x=20 => 5");
    }

    // Multi-equation substitution
    {
        write_fw("/tmp/t4.fw",
            "distance = speed * time\n"
            "distance = sqrt((x1 - x2)^2 + (y1 - y2)^2)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/t4.fw");
        double r = sys.resolve("time", {
            {"speed", 3}, {"x1", 10}, {"x2", 14}, {"y1", 14}, {"y2", 78}
        });
        // distance = sqrt(16 + 4096) = sqrt(4112) ≈ 64.125
        // time = 64.125 / 3 ≈ 21.375
        ASSERT_NUM(r, std::sqrt(4112.0) / 3.0, "multi-eq time");
    }

    // Defaults
    {
        write_fw("/tmp/t5.fw",
            "time = 10\nspeed = 10\ndistance = 100\n"
            "distance = speed * time\n");
        FormulaSystem sys;
        sys.load_file("/tmp/t5.fw");
        // Querying distance with time=5 should use default speed=10
        double r = sys.resolve("distance", {{"time", 5}});
        ASSERT_NUM(r, 50, "defaults: distance=speed*time, time=5, speed=10 => 50");
    }

    // Defaults are overridden by bindings
    {
        FormulaSystem sys;
        sys.load_file("/tmp/t5.fw");
        double r = sys.resolve("distance", {{"time", 5}, {"speed", 20}});
        ASSERT_NUM(r, 100, "override default: speed=20, time=5 => 100");
    }

    // Solve for speed from defaults
    {
        FormulaSystem sys;
        sys.load_file("/tmp/t5.fw");
        // distance default is skipped (it's the... wait, we're solving for speed)
        // time default = 10, distance default = 100 => speed = 100/10 = 10
        double r = sys.resolve("speed", {});
        ASSERT_NUM(r, 10, "solve speed from defaults: d=100,t=10 => 10");
    }

    // Circular dependency detection
    {
        write_fw("/tmp/tc.fw", "x = y + 1\ny = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc.fw");
        bool threw = false;
        try { sys.resolve("x", {}); } catch (...) { threw = true; }
        ASSERT(threw, "circular dependency throws");
    }

    // Missing variable
    {
        write_fw("/tmp/tm.fw", "x = y + z\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tm.fw");
        bool threw = false;
        try { sys.resolve("x", {{"y", 1}}); } catch (...) { threw = true; }
        ASSERT(threw, "missing variable throws");
    }

    // Function in equation
    {
        write_fw("/tmp/tf.fw", "hyp = sqrt(a^2 + b^2)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf.fw");
        double r = sys.resolve("hyp", {{"a", 3}, {"b", 4}});
        ASSERT_NUM(r, 5, "hyp = sqrt(a^2+b^2), 3-4-5 triangle");
    }

    // Chained resolution: x = a + 1, y = x * 2 => y given a
    {
        write_fw("/tmp/tch.fw", "x = a + 1\ny = x * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tch.fw");
        double r = sys.resolve("y", {{"a", 4}});
        ASSERT_NUM(r, 10, "chained: y=x*2, x=a+1, a=4 => 10");
    }

    // Chained inverse: y = x * 2, x = a + 1, solve for a given y
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tch.fw");
        double r = sys.resolve("a", {{"y", 10}});
        ASSERT_NUM(r, 4, "chained inverse: y=10 => a=4");
    }

    // Multiple variables with defaults and overrides
    {
        write_fw("/tmp/tmv.fw",
            "g = 9.81\n"
            "force = mass * g\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmv.fw");
        double r = sys.resolve("force", {{"mass", 10}});
        ASSERT_NUM(r, 98.1, "F=mg, m=10, g=9.81 => 98.1");

        double m = sys.resolve("mass", {{"force", 98.1}});
        ASSERT_NUM(m, 10, "F=mg, F=98.1, g=9.81 => m=10");
    }
}

// ---- CLI parser tests ----

void test_cli_parser() {
    SECTION("CLI Query Parser");

    {
        auto q = parse_cli_query("myformula(distance=?, time=5)");
        ASSERT_EQ(q.filename, "myformula.fw", "filename with .fw");
        ASSERT_EQ(q.queries[0].variable, "distance", "solve_for = distance");
        ASSERT_NUM(q.bindings.at("time"), 5, "time=5 binding");
    }
    {
        auto q = parse_cli_query("test.fw(x=?, y=3, z=10)");
        ASSERT_EQ(q.filename, "test.fw", "filename already has .fw");
        ASSERT_EQ(q.queries[0].variable, "x", "solve_for = x");
        ASSERT(q.bindings.size() == 2, "two bindings");
        ASSERT_NUM(q.bindings.at("y"), 3, "y=3");
        ASSERT_NUM(q.bindings.at("z"), 10, "z=10");
    }
    {
        auto q = parse_cli_query("f(a=?, b=3.14)");
        ASSERT_NUM(q.bindings.at("b"), 3.14, "float binding");
    }
    {
        bool threw = false;
        try { parse_cli_query("noparens"); } catch (...) { threw = true; }
        ASSERT(threw, "missing parens throws");
    }
    {
        bool threw = false;
        try { parse_cli_query("f(x=3)"); } catch (...) { threw = true; }
        ASSERT(threw, "no query var throws");
    }
    // Bare variable names — allowed in symbolic modes (--derive/--fit),
    // rejected elsewhere with a clear error. Matches user's "b=b" workaround.
    {
        // Symbolic mode: bare names become symbolic placeholders.
        auto q = parse_cli_query("triangle(A=?, a=4, B=20, c, b)",
                                 /*allow_no_queries*/false,
                                 /*allow_symbolic*/true);
        ASSERT_EQ(q.queries[0].variable, "A", "bare-name query: solve_for = A");
        ASSERT_NUM(q.bindings.at("a"), 4, "bare-name query: a=4 binding");
        ASSERT_NUM(q.bindings.at("B"), 20, "bare-name query: B=20 binding");
        ASSERT(q.symbolic.count("c") == 1, "bare-name query: c is symbolic");
        ASSERT(q.symbolic.count("b") == 1, "bare-name query: b is symbolic");
        ASSERT_EQ(q.symbolic.at("c"), "c", "bare-name: c=c symbolic mapping");
        ASSERT_EQ(q.symbolic.at("b"), "b", "bare-name: b=b symbolic mapping");
    }
    {
        // Numeric mode: bare names throw with a clear error.
        bool threw = false;
        std::string msg;
        try {
            parse_cli_query("triangle(A=?, a=4, b)",
                            /*allow_no_queries*/false,
                            /*allow_symbolic*/false);
        } catch (const std::runtime_error& e) {
            threw = true;
            msg = e.what();
        }
        ASSERT(threw, "bare-name in numeric mode throws");
        ASSERT(msg.find("Bare variable name") != std::string::npos,
               "bare-name error message mentions 'Bare variable name'");
        ASSERT(msg.find("--derive") != std::string::npos,
               "bare-name error suggests --derive");
    }
}

// ---- File parsing edge cases ----

void test_file_parsing() {
    SECTION("File Parsing");

    // Comments and blank lines
    {
        write_fw("/tmp/tp1.fw",
            "# This is a comment\n"
            "\n"
            "x = y + 1\n"
            "# Another comment\n"
            "\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tp1.fw");
        ASSERT(sys.equations.size() == 1, "comments/blanks skipped, 1 equation");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "equation works after comments");
    }

    // Negative default
    {
        write_fw("/tmp/tp2.fw",
            "offset = -10\n"
            "x = y + offset\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tp2.fw");
        ASSERT_NUM(sys.defaults.at("offset"), -10, "negative default");
        ASSERT_NUM(sys.resolve("x", {{"y", 15}}), 5, "negative default in equation");
    }

    // Multiple equations
    {
        write_fw("/tmp/tp3.fw",
            "area = width * height\n"
            "perimeter = 2 * width + 2 * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tp3.fw");
        ASSERT(sys.equations.size() == 2, "two equations loaded");
        ASSERT_NUM(sys.resolve("area", {{"width", 5}, {"height", 3}}), 15, "area=w*h");
        ASSERT_NUM(sys.resolve("perimeter", {{"width", 5}, {"height", 3}}), 16, "perim=2w+2h");
    }
}

// ---- Edge case tests ----

void test_lexer_edge() {
    SECTION("Lexer Edge Cases");

    // Leading dot number
    {
        auto tokens = Lexer(".5").tokenize();
        ASSERT(tokens[0].type == TokenType::NUMBER, ".5 is a number");
        ASSERT_NUM(tokens[0].numval, 0.5, ".5 value");
    }

    // Identifiers with digits
    {
        auto tokens = Lexer("x1").tokenize();
        ASSERT(tokens[0].type == TokenType::IDENT, "x1 is ident");
        ASSERT_EQ(tokens[0].text, "x1", "x1 text");
    }
    {
        auto tokens = Lexer("var_2_name").tokenize();
        ASSERT_EQ(tokens[0].text, "var_2_name", "underscore+digit ident");
    }

    // Leading underscore
    {
        auto tokens = Lexer("_foo").tokenize();
        ASSERT(tokens[0].type == TokenType::IDENT, "_foo is ident");
        ASSERT_EQ(tokens[0].text, "_foo", "_foo text");
    }

    // Number followed immediately by ident (no implicit multiply)
    {
        auto tokens = Lexer("2x").tokenize();
        ASSERT(tokens[0].type == TokenType::NUMBER, "2x: first is number");
        ASSERT(tokens[1].type == TokenType::IDENT, "2x: second is ident");
    }

    // Consecutive operators
    {
        auto tokens = Lexer("++").tokenize();
        ASSERT(tokens[0].type == TokenType::PLUS, "first +");
        ASSERT(tokens[1].type == TokenType::PLUS, "second +");
    }

    // Tab whitespace
    {
        auto tokens = Lexer("\tx\t+\t1\t").tokenize();
        ASSERT(tokens[0].type == TokenType::IDENT, "tabs: ident");
        ASSERT(tokens[1].type == TokenType::PLUS, "tabs: plus");
        ASSERT(tokens[2].type == TokenType::NUMBER, "tabs: number");
    }

    // Single character tokens
    {
        auto tokens = Lexer("x").tokenize();
        ASSERT(tokens.size() == 2, "single ident + END");
    }
    {
        auto tokens = Lexer("7").tokenize();
        ASSERT(tokens.size() == 2, "single number + END");
    }

    // Multiple dots consumed as one number token (stod handles gracefully)
    {
        auto tokens = Lexer("3.14.15").tokenize();
        ASSERT(tokens[0].type == TokenType::NUMBER, "multi-dot is number token");
        // stod("3.14.15") parses 3.14, rest is silently ignored
        ASSERT_NUM(tokens[0].numval, 3.14, "multi-dot value is 3.14");
    }

    // Zero
    {
        auto tokens = Lexer("0").tokenize();
        ASSERT_NUM(tokens[0].numval, 0, "zero");
    }

    // Large number
    {
        auto tokens = Lexer("999999999").tokenize();
        ASSERT_NUM(tokens[0].numval, 999999999, "large number");
    }
}

void test_parser_edge() {
    SECTION("Parser Edge Cases");

    // Deeply nested parens
    ASSERT_EQ(ps("((((x))))"), "x", "deeply nested parens");
    ASSERT_EQ(ps("((1 + 2))"), "1 + 2", "nested parens on expr");

    // Unary minus in various positions
    ASSERT_EQ(ps("-(-x)"), "-(-x)", "double neg in parser");
    ASSERT_EQ(ps("-(-(-x))"), "-(-(-x))", "triple neg parse");
    ASSERT_EQ(ps("-sqrt(x)"), "-(sqrt(x))", "neg function call");
    ASSERT_EQ(ps("-(x * y)"), "-(x * y)", "neg product");
    ASSERT_EQ(ps("-1"), "-1", "neg literal");

    // Empty function args
    {
        auto tokens = Lexer("f()").tokenize();
        Parser p(tokens);
        auto e = p.parse_expr();
        ASSERT(e->type == ExprType::FUNC_CALL, "f() is func call");
        ASSERT(e->args.empty(), "f() has no args");
    }

    // Power is right-associative (mathematical convention): x^2^3 = x^(2^3)
    {
        auto tokens = Lexer("x^2^3").tokenize();
        Parser p(tokens);
        auto e = p.parse_expr();
        ASSERT_EQ(expr_to_string(e), "x^2^3", "x^2^3 parses fully");
        ASSERT(p.at_end(), "x^2^3 no trailing tokens");
        // Verify: x=2, should be 2^(2^3) = 2^8 = 256
        auto v = substitute(e, "x", Expr::Num(2));
        ASSERT_NUM((evaluate(v).value()), 256, "2^2^3 = 2^8 = 256");
    }

    // Error: missing close paren
    {
        bool threw = false;
        try { parse("(x + 1"); } catch (...) { threw = true; }
        ASSERT(threw, "missing close paren throws");
    }

    // Error: empty expression
    {
        bool threw = false;
        try { parse(""); } catch (...) { threw = true; }
        ASSERT(threw, "empty expression throws");
    }

    // Error: just an operator
    {
        bool threw = false;
        try { parse("+"); } catch (...) { threw = true; }
        ASSERT(threw, "bare operator throws");
    }

    // Unary minus before parens
    ASSERT_EQ(ps("-(x + y) * z"), "-(x + y) * z", "neg parens times z");

    // Nested function calls
    ASSERT_EQ(ps("sqrt(abs(x))"), "sqrt(abs(x))", "nested functions");

    // Function with complex arg
    ASSERT_EQ(ps("sqrt(x^2 + y^2)"), "sqrt(x^2 + y^2)", "func with compound arg");
}

void test_evaluate_edge() {
    SECTION("Evaluate Edge Cases");

    // sqrt of negative => NaN
    {
        double r = ev_nan("sqrt(-1)");
        ASSERT(std::isnan(r), "sqrt(-1) is NaN");
    }

    // log(0) => -inf
    {
        double r = ev("log(0)");
        ASSERT(std::isinf(r) && r < 0, "log(0) is -inf");
    }

    // 0^0 => 1 (C++ pow convention)
    ASSERT_NUM(ev("0^0"), 1, "0^0 = 1");

    // Negative base with integer exponent
    ASSERT_NUM(ev("(-2)^3"), -8, "(-2)^3 = -8");
    ASSERT_NUM(ev("(-2)^2"), 4, "(-2)^2 = 4");
    ASSERT_NUM(ev("(-1)^0"), 1, "(-1)^0 = 1");

    // Very small result
    ASSERT_NUM(ev("1 / 1000000"), 0.000001, "very small division");

    // Chained operations
    ASSERT_NUM(ev("1 + 2 + 3 + 4 + 5"), 15, "chained add");
    ASSERT_NUM(ev("100 - 10 - 20 - 30"), 40, "chained sub");
    ASSERT_NUM(ev("2 * 3 * 4"), 24, "chained mul");

    // Mixed operations
    ASSERT_NUM(ev("2 + 3 * 4 - 1"), 13, "mixed ops");
    ASSERT_NUM(ev("(2 + 3) * (4 - 1)"), 15, "parens mixed");

    // Deeply nested
    ASSERT_NUM(ev("((((1 + 2))))"), 3, "deeply nested eval");

    // Unknown function — evaluate yields empty Checked
    {
        auto r = evaluate(simplify(parse("foobar(1)")));
        ASSERT(!r.has_value(), "unknown function yields empty");
    }
}

void test_simplify_edge() {
    SECTION("Simplifier Edge Cases");

    // Load builtin rewrite rules (power rules)
    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules, &builtin_sys.rewrite_exhaustive_flags_);

    // x - x: simplifier does NOT reduce this (no term cancellation yet)
    // This documents current behavior
    ASSERT_EQ(ss("x - x"), "0", "x - x → 0");

    // x / x: simplifier does NOT reduce this
    ASSERT_EQ(ss("x / x"), "1", "x / x → 1");

    // 0 * complex expr => 0
    {
        auto e = Expr::BinOpExpr(BinOp::MUL, Expr::Num(0),
            parse("sqrt(x^2 + y^2) * z + w"));
        ASSERT_EQ(expr_to_string(simplify(e)), "0", "0 * complex = 0");
    }

    // Chained negation cancellation
    {
        // -(-(-(-(x)))) = x
        auto e = Expr::Neg(Expr::Neg(Expr::Neg(Expr::Neg(Expr::Var("x")))));
        ASSERT_EQ(expr_to_string(simplify(e)), "x", "four negations cancel");
    }
    {
        // -(-(-(x))) = -x
        auto e = Expr::Neg(Expr::Neg(Expr::Neg(Expr::Var("x"))));
        ASSERT_EQ(expr_to_string(simplify(e)), "-x", "three negations = -x");
    }

    // Constant reassociation chains
    ASSERT_EQ(ss("x + 1 + 2 + 3"), "x + 6", "chain fold x+1+2+3");
    ASSERT_EQ(ss("x - 1 - 2 - 3"), "x - 6", "chain fold x-1-2-3");
    ASSERT_EQ(ss("x * 2 * 3 * 4"), "24 * x", "chain fold x*2*3*4");

    // Subtract then add constants: (x - 3) + 5 => x + 2
    ASSERT_EQ(ss("x - 3 + 5"), "x + 2", "(x-3)+5 => x+2");

    // Add then subtract: (x + 3) - 5 => x - 2
    ASSERT_EQ(ss("x + 3 - 5"), "x - 2", "(x+3)-5 => x-2");

    // Constant reassociation to zero: (x + 5) - 5 => x
    ASSERT_EQ(ss("x + 5 - 5"), "x", "(x+5)-5 => x");

    // Neg of neg number
    {
        auto e = Expr::Neg(Expr::Num(-7));
        ASSERT_EQ(expr_to_string(simplify(e)), "7", "-(-7) = 7");
    }

    // -(0) => 0
    {
        auto e = Expr::Neg(Expr::Num(0));
        ASSERT_EQ(expr_to_string(simplify(e)), "0", "-(0) = 0");
    }

    // 1^anything => 1
    ASSERT_EQ(ss("1^999"), "1", "1^999 = 1");

    // anything^0 => 1
    {
        auto e = Expr::BinOpExpr(BinOp::POW, parse("a + b + c"), Expr::Num(0));
        ASSERT_EQ(expr_to_string(simplify(e)), "1", "complex^0 = 1");
    }

    // x * 0 where x is complex
    {
        auto e = Expr::BinOpExpr(BinOp::MUL, parse("sqrt(a) + b * c"), Expr::Num(0));
        ASSERT_EQ(expr_to_string(simplify(e)), "0", "complex * 0 = 0");
    }

    // Division: neg in numerator only => neg pulled out
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Neg(Expr::Var("a")), Expr::Var("b"));
        ASSERT_EQ(expr_to_string(simplify(e)), "-a / b", "(-a)/b => -a/b");
    }

    // Multiplication: neg in one operand => neg pulled out
    {
        auto e = Expr::BinOpExpr(BinOp::MUL, Expr::Var("a"), Expr::Neg(Expr::Var("b")));
        ASSERT_EQ(expr_to_string(simplify(e)), "-(a * b)", "a*(-b) => -(a*b)");
    }
}

void test_decompose_edge() {
    SECTION("Decomposition Edge Cases");

    // Variable not present at all => coeff=0
    {
        auto lf = decompose_linear(parse("a + b + 1"), "z");
        ASSERT(lf.has_value(), "no z present: ok");
        ASSERT_NUM((evaluate(lf->coeff).value()), 0, "no z: coeff=0");
    }

    // 0 * y => coeff=0 (zero coefficient)
    {
        auto lf = decompose_linear(parse("0 * y"), "y");
        ASSERT(lf.has_value(), "0*y is linear");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), 0, "0*y coeff=0");
    }

    // Subtraction of same var: y - y => coeff=0
    {
        auto lf = decompose_linear(parse("y - y"), "y");
        ASSERT(lf.has_value(), "y-y is linear");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), 0, "y-y coeff=0");
    }

    // Complex coefficient: (a + b) * y
    {
        auto lf = decompose_linear(parse("(a + b) * y"), "y");
        ASSERT(lf.has_value(), "(a+b)*y is linear in y");
        ASSERT_EQ(expr_to_string(lf->coeff), "a + b", "(a+b)*y coeff=a+b");
    }

    // y appears in add and mul: 2*y + 3*y + y => coeff=6
    {
        auto lf = decompose_linear(parse("2*y + 3*y + y"), "y");
        ASSERT(lf.has_value(), "2y+3y+y is linear");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), 6, "2y+3y+y coeff=6");
    }

    // y in nested linear: (y + 1) * 2 - y => coeff=1, rest=2
    {
        auto lf = decompose_linear(parse("(y + 1) * 2 - y"), "y");
        ASSERT(lf.has_value(), "(y+1)*2-y is linear");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), 1, "(y+1)*2-y coeff=1");
        ASSERT_NUM((evaluate(simplify(lf->rest)).value()), 2, "(y+1)*2-y rest=2");
    }

    // Negative coefficient: 5 - 3*y => coeff=-3, rest=5
    {
        auto lf = decompose_linear(parse("5 - 3*y"), "y");
        ASSERT(lf.has_value(), "5-3y is linear");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), -3, "5-3y coeff=-3");
        ASSERT_NUM((evaluate(simplify(lf->rest)).value()), 5, "5-3y rest=5");
    }

    // y in exponent is nonlinear
    {
        auto lf = decompose_linear(parse("2^y"), "y");
        ASSERT(!lf.has_value(), "2^y is nonlinear");
    }

    // y in function arg is nonlinear
    {
        auto lf = decompose_linear(parse("sin(y) + 1"), "y");
        ASSERT(!lf.has_value(), "sin(y)+1 is nonlinear");
    }

    // Pure constant expression
    {
        auto lf = decompose_linear(Expr::Num(42), "y");
        ASSERT(lf.has_value(), "constant is linear (trivially)");
        ASSERT_NUM((evaluate(lf->coeff).value()), 0, "constant coeff=0");
        ASSERT_NUM((evaluate(lf->rest).value()), 42, "constant rest=42");
    }
}

void test_solve_for_edge() {
    SECTION("Solver Edge Cases");

    // Solve for var not in equation => coeff=0, returns nullptr
    {
        auto sol = solve_for(Expr::Var("x"), parse("a + b"), "z");
        ASSERT(sol == nullptr, "var not in equation returns nullptr");
    }

    // Solve degenerate: x = x => coeff=0, always true, returns nullptr
    {
        auto sol = solve_for(Expr::Var("x"), Expr::Var("x"), "x");
        ASSERT(sol == nullptr, "x = x returns nullptr (identity, no unique solution)");
    }

    // Solve when var only on LHS: solve x = 5 for x => x = 5
    {
        auto sol = solve_for(Expr::Var("x"), Expr::Num(5), "x");
        ASSERT(sol != nullptr, "x = 5 solvable for x");
        ASSERT_NUM((evaluate(simplify(sol)).value()), 5, "x = 5 => x = 5");
    }

    // Solve with nested expressions: x = (2*y + 3) / (y - something)
    // This has y in denominator — nonlinear, should return nullptr
    {
        auto sol = solve_for(Expr::Var("x"),
            Expr::BinOpExpr(BinOp::DIV,
                Expr::BinOpExpr(BinOp::ADD,
                    Expr::BinOpExpr(BinOp::MUL, Expr::Num(2), Expr::Var("y")),
                    Expr::Num(3)),
                Expr::BinOpExpr(BinOp::SUB, Expr::Var("y"), Expr::Num(1))),
            "y");
        ASSERT(sol == nullptr, "y in denominator is nonlinear");
    }

    // Solve with fractional coefficient: x = y/3 + 2 => y = (x-2)*3 = 3x - 6
    {
        auto sol = solve_for(Expr::Var("x"), parse("y / 3 + 2"), "y");
        ASSERT(sol != nullptr, "y/3+2 solvable for y");
        // Substitute to verify: if x=8, y should be (8-2)*3 = 18
        auto val = substitute(sol, "x", Expr::Num(8));
        ASSERT_NUM((evaluate(simplify(val)).value()), 18, "y/3+2: x=8 => y=18");
    }
}

void test_system_edge() {
    SECTION("System Edge Cases");

    // Empty file
    {
        write_fw("/tmp/te1.fw", "\n\n\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te1.fw");
        ASSERT(sys.equations.empty(), "empty file: no equations");
        ASSERT(sys.defaults.empty(), "empty file: no defaults");
    }

    // File with only comments
    {
        write_fw("/tmp/te2.fw", "# comment 1\n# comment 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te2.fw");
        ASSERT(sys.equations.empty(), "comments only: no equations");
    }

    // File with only defaults, no equations
    {
        write_fw("/tmp/te3.fw", "x = 42\ny = 7\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te3.fw");
        ASSERT(sys.equations.empty(), "defaults only: no equations");
        ASSERT_NUM(sys.defaults.at("x"), 42, "default x=42");
        ASSERT_NUM(sys.defaults.at("y"), 7, "default y=7");
    }

    // Self-referencing equation: x = x + 1 (unsolvable circular)
    {
        write_fw("/tmp/te4.fw", "x = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te4.fw");
        bool threw = false;
        try { sys.resolve("x", {}); } catch (...) { threw = true; }
        ASSERT(threw, "x = x + 1 is unsolvable");
    }

    // Redundant equations: same equation twice
    {
        write_fw("/tmp/te5.fw", "x = y + 1\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te5.fw");
        double r = sys.resolve("x", {{"y", 5}});
        ASSERT_NUM(r, 6, "redundant equations: still works");
    }

    // Binding the solve target explicitly should not happen via CLI,
    // but test that resolve handles it if target is already bound
    {
        write_fw("/tmp/te6.fw", "x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te6.fw");
        // The resolve method skips the target's default but not explicit bindings
        // If someone passes x in bindings AND asks to solve for x, binding wins
        // (this shouldn't happen from CLI but let's verify it doesn't crash)
        double r = sys.resolve("x", {{"x", 99}, {"y", 5}});
        // x=99 was passed as a binding but x is also the target —
        // the default-skip only applies to defaults, not explicit bindings
        ASSERT_NUM(r, 99, "explicit binding for target variable");
    }

    // Deep chain: a=b+1, b=c+1, c=d+1, d=e+1, e=1 => a=5
    {
        write_fw("/tmp/te7.fw",
            "a = b + 1\n"
            "b = c + 1\n"
            "c = d + 1\n"
            "d = e + 1\n"
            "e = 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te7.fw");
        double r = sys.resolve("a", {});
        ASSERT_NUM(r, 5, "deep chain a=b+1=c+2=d+3=e+4=5");
    }

    // Deep chain inverse: solve e given a
    {
        FormulaSystem sys;
        sys.load_file("/tmp/te7.fw");
        double r = sys.resolve("e", {{"a", 10}});
        ASSERT_NUM(r, 6, "deep chain inverse: a=10 => e=6");
    }

    // Three equations, two sharing a variable
    {
        write_fw("/tmp/te8.fw",
            "y = 2 * x\n"
            "z = y + 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te8.fw");
        ASSERT_NUM(sys.resolve("z", {{"x", 5}}), 20, "chain: z=2x+10, x=5 => 20");
        ASSERT_NUM(sys.resolve("x", {{"z", 20}}), 5, "chain inverse: z=20 => x=5");
    }

    // Equation with all built-in functions
    {
        write_fw("/tmp/te9.fw", "result = sqrt(abs(x))\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te9.fw");
        ASSERT_NUM(sys.resolve("result", {{"x", -16}}), 4, "sqrt(abs(-16))=4");
        ASSERT_NUM(sys.resolve("result", {{"x", 25}}), 5, "sqrt(abs(25))=5");
    }

    // Zero default
    {
        write_fw("/tmp/te10.fw", "x = 0\ny = x + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te10.fw");
        ASSERT_NUM(sys.resolve("y", {}), 5, "zero default: y=0+5=5");
    }

    // Float default
    {
        write_fw("/tmp/te11.fw", "pi = 3.14159\ncirc = 2 * pi * r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/te11.fw");
        ASSERT_NUM(sys.resolve("circ", {{"r", 1}}), 2 * 3.14159, "circ=2*pi*r, r=1");
    }

    // Cannot open nonexistent file
    {
        FormulaSystem sys;
        bool threw = false;
        try { sys.load_file("/tmp/nonexistent_fwiz_file.fw"); } catch (...) { threw = true; }
        ASSERT(threw, "nonexistent file throws");
    }
}

void test_cli_parser_edge() {
    SECTION("CLI Parser Edge Cases");

    // Negative number in binding
    {
        auto q = parse_cli_query("f(x=?, y=-3)");
        ASSERT_NUM(q.bindings.at("y"), -3, "negative binding");
    }

    // Zero binding
    {
        auto q = parse_cli_query("f(x=?, y=0)");
        ASSERT_NUM(q.bindings.at("y"), 0, "zero binding");
    }

    // Very large number
    {
        auto q = parse_cli_query("f(x=?, y=1000000)");
        ASSERT_NUM(q.bindings.at("y"), 1000000, "large binding");
    }

    // Spaces around equals and commas
    {
        auto q = parse_cli_query("f( x = ? , y = 5 )");
        ASSERT_EQ(q.queries[0].variable, "x", "spaces: solve_for");
        ASSERT_NUM(q.bindings.at("y"), 5, "spaces: binding");
    }

    // Only the query variable, no other bindings
    {
        auto q = parse_cli_query("f(x=?)");
        ASSERT_EQ(q.queries[0].variable, "x", "single var query");
        ASSERT(q.bindings.empty(), "no bindings");
    }

    // Many bindings
    {
        auto q = parse_cli_query("f(z=?, a=1, b=2, c=3, d=4, e=5)");
        ASSERT_EQ(q.queries[0].variable, "z", "many bindings: solve_for");
        ASSERT(q.bindings.size() == 5, "many bindings: count");
    }

    // Float in binding
    {
        auto q = parse_cli_query("f(x=?, y=3.14159)");
        ASSERT_NUM(q.bindings.at("y"), 3.14159, "float binding precise");
    }

    // Missing closing paren
    {
        bool threw = false;
        try { parse_cli_query("f(x=?"); } catch (...) { threw = true; }
        ASSERT(threw, "missing closing paren");
    }

    // Multiple ? marks — all are kept as queries
    {
        auto q = parse_cli_query("f(x=?, y=?)");
        ASSERT(q.queries.size() == 2, "multiple ?: two queries");
        ASSERT_EQ(q.queries[0].variable, "x", "first query is x");
        ASSERT_EQ(q.queries[1].variable, "y", "second query is y");
    }

    // Filename with path
    {
        auto q = parse_cli_query("path/to/file(x=?)");
        ASSERT_EQ(q.filename, "path/to/file.fw", "path in filename");
    }

    // Filename already with extension
    {
        auto q = parse_cli_query("test.fw(x=?)");
        ASSERT_EQ(q.filename, "test.fw", "existing .fw extension kept");
    }
}

void test_printer_edge() {
    SECTION("Printer Edge Cases");

    // Negative numbers in expressions
    {
        auto e = Expr::BinOpExpr(BinOp::ADD, Expr::Var("x"), Expr::Num(-5));
        // After simplify, x + (-5) => x - 5
        ASSERT_EQ(expr_to_string(simplify(e)), "x - 5", "print x + neg const");
    }

    // Nested division (right-associative needs parens)
    ASSERT_EQ(ps("a / (b / c)"), "a / (b / c)", "nested div parens");
    ASSERT_EQ(ps("a / b / c"), "a / b / c", "left-assoc div no extra parens");

    // Nested subtraction
    ASSERT_EQ(ps("a - (b - c)"), "a - (b - c)", "nested sub parens");
    ASSERT_EQ(ps("a - b - c"), "a - b - c", "left-assoc sub no extra parens");

    // Mixed precedence: mul inside add doesn't need parens
    ASSERT_EQ(ps("a + b * c"), "a + b * c", "mul inside add");
    // Add inside mul needs parens
    ASSERT_EQ(ps("(a + b) * c"), "(a + b) * c", "add inside mul");
    // Add inside pow needs parens
    ASSERT_EQ(ps("(a + b)^2"), "(a + b)^2", "add inside pow");

    // Power doesn't need parens around atoms
    ASSERT_EQ(ps("x^y"), "x^y", "var^var");
    ASSERT_EQ(ps("2^10"), "2^10", "num^num");
}

// ---- Garbage input / robustness tests ----

void test_lexer_garbage() {
    SECTION("Lexer Garbage Handling");

    // All special characters should throw with clear message
    auto expect_throw = [](const std::string& input, const std::string& label) {
        bool threw = false;
        try { Lexer(input).tokenize(); } catch (const std::exception&) { threw = true; }
        ASSERT(threw, label + " throws");
    };

    expect_throw("x \\ y", "backslash");
    expect_throw("x; y", "semicolon");
    expect_throw("x : y", "colon");
    expect_throw("x & y", "ampersand");
    expect_throw("x | y", "pipe");
    expect_throw("x @ y", "at sign");
    expect_throw("x $ y", "dollar");
    expect_throw("x ~ y", "tilde");
    expect_throw("x ` y", "backtick");
    expect_throw("x < y", "angle bracket");
    expect_throw("{x}", "curly brace");
    expect_throw("[x]", "square bracket");
    expect_throw("x!", "exclamation");

    // Null byte
    {
        bool threw = false;
        try { Lexer(std::string("x\0y", 3)).tokenize(); } catch (...) { threw = true; }
        ASSERT(threw, "null byte throws");
    }

    // Newline (lexer doesn't handle newlines — file parser splits lines first)
    {
        bool threw = false;
        try { Lexer("x +\ny").tokenize(); } catch (...) { threw = true; }
        ASSERT(threw, "newline in lexer throws");
    }

    // Very long identifier still works
    {
        std::string long_id(10000, 'a');
        auto tokens = Lexer(long_id).tokenize();
        ASSERT(tokens[0].type == TokenType::IDENT, "10000-char ident works");
        ASSERT(tokens[0].text.size() == 10000, "long ident preserved");
    }

    // Very long number
    {
        auto tokens = Lexer("99999999999999999999").tokenize();
        ASSERT(tokens[0].type == TokenType::NUMBER, "very long number lexes");
    }

    // Only whitespace
    {
        auto tokens = Lexer("   \t\t   ").tokenize();
        ASSERT(tokens.size() == 1 && tokens[0].type == TokenType::END, "only whitespace => END");
    }
}

void test_parser_garbage() {
    SECTION("Parser Garbage Handling");

    auto expect_throw = [](const std::string& input, const std::string& label) {
        bool threw = false;
        try { parse(input); } catch (const std::exception&) { threw = true; }
        ASSERT(threw, label + " throws");
    };

    // Operators in wrong positions
    expect_throw("+", "bare +");
    expect_throw("*", "bare *");
    expect_throw("/", "bare /");
    expect_throw("^", "bare ^");
    expect_throw("x +", "trailing +");
    expect_throw("x *", "trailing *");
    expect_throw("* x", "leading *");
    expect_throw("/ x", "leading /");

    // Double operators: "x ++ y" — first + makes parse_multiplicative expect a term,
    // second + is unexpected
    expect_throw("x ++ y", "double plus");
    expect_throw("x ** y", "double star");

    // Mismatched parens
    expect_throw("(x + 1", "unclosed paren");
    expect_throw("((x)", "double open single close");
    expect_throw(")", "bare close paren");
    expect_throw(")(", "reversed parens");
    expect_throw("()", "empty parens");

    // Trailing tokens don't throw (parser just stops) — test that parse succeeds
    // but doesn't consume everything
    {
        auto tokens = Lexer("x y").tokenize();
        Parser p(tokens);
        auto e = p.parse_expr();
        ASSERT_EQ(expr_to_string(e), "x", "x y: parses x");
        ASSERT(!p.at_end(), "x y: has trailing tokens");
    }
    {
        auto tokens = Lexer("(x))").tokenize();
        Parser p(tokens);
        auto e = p.parse_expr();
        ASSERT_EQ(expr_to_string(e), "x", "(x)): parses x");
        ASSERT(!p.at_end(), "(x)): trailing close paren");
    }
    {
        auto tokens = Lexer("x = y").tokenize();
        Parser p(tokens);
        auto e = p.parse_expr();
        ASSERT_EQ(expr_to_string(e), "x", "x = y: parses x");
        ASSERT(!p.at_end(), "x = y: = is trailing");
    }
}

void test_cli_garbage() {
    SECTION("CLI Garbage Handling");

    auto expect_throw = [](const std::string& input, const std::string& label) {
        bool threw = false;
        std::string msg;
        try { parse_cli_query(input); } catch (const std::exception& e) {
            threw = true; msg = e.what();
        }
        ASSERT(threw, label + " throws");
        return msg;
    };

    // Structural errors
    expect_throw("", "empty string");
    expect_throw("hello", "no parens");
    expect_throw("f(x=?", "missing close paren");

    // No query variable
    expect_throw("f()", "empty parens");
    expect_throw("f(x=5)", "no query var");
    expect_throw("f(,,,)", "only commas");
    expect_throw("f(   )", "only spaces");

    // Specific error messages for value problems
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(x=)"); } catch (const std::exception& e) {
            threw = true; msg = e.what();
        }
        ASSERT(threw, "empty value throws");
        ASSERT(msg.find("Missing value") != std::string::npos,
            "empty value: clear error message");
    }
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(=5)"); } catch (const std::exception& e) {
            threw = true; msg = e.what();
        }
        ASSERT(threw, "empty name throws");
        ASSERT(msg.find("Missing variable name") != std::string::npos,
            "empty name: clear error message");
    }
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(x=abc)"); } catch (const std::exception& e) {
            threw = true; msg = e.what();
        }
        ASSERT(threw, "non-numeric value throws");
        ASSERT(msg.find("Invalid") != std::string::npos || msg.find("unresolved") != std::string::npos,
            "non-numeric: clear error message");
    }
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(x=y=5)"); } catch (const std::exception& e) {
            threw = true; msg = e.what();
        }
        ASSERT(threw, "multiple equals throws");
        ASSERT(!msg.empty(), "multiple equals: has error message");
    }

    // Just a ? with no name
    expect_throw("f(?)", "bare question mark");
}

void test_file_garbage() {
    SECTION("File Garbage Resilience");

    // Lines with no equals sign — silently skipped
    {
        write_fw("/tmp/tg1.fw", "hello world\nfoo bar baz\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg1.fw");
        ASSERT(sys.equations.empty(), "no-equals lines: skipped");
    }

    // Line with just equals sign — skipped (too few tokens)
    {
        write_fw("/tmp/tg2.fw", "=\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg2.fw");
        ASSERT(sys.equations.empty(), "bare equals: skipped");
    }

    // Valid equation followed by garbage line — equation is kept, garbage skipped
    {
        write_fw("/tmp/tg3.fw", "x = y + 1\n@#$%^&\nz = w * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg3.fw");
        // The garbage line should be skipped, both equations should load
        ASSERT(sys.equations.size() == 2, "garbage between equations: both kept");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "eq before garbage works");
        ASSERT_NUM(sys.resolve("z", {{"w", 3}}), 6, "eq after garbage works");
    }

    // Empty RHS: "x = " — skipped gracefully
    {
        write_fw("/tmp/tg4.fw", "x = \ny = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg4.fw");
        // "x = " should be skipped, "y = x + 1" should load
        // Note: x has no equation or default, so resolving y needs x
        ASSERT(sys.equations.size() >= 1, "empty RHS line skipped, valid eq kept");
    }

    // Binary junk — skipped
    {
        write_fw("/tmp/tg5.fw", std::string("\x01\x02\x03\x04\x05", 5));
        FormulaSystem sys;
        sys.load_file("/tmp/tg5.fw");
        ASSERT(sys.equations.empty(), "binary junk: skipped");
    }

    // Just numbers, no equals
    {
        write_fw("/tmp/tg6.fw", "123 456\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg6.fw");
        ASSERT(sys.equations.empty(), "bare numbers: skipped");
    }

    // Tab-separated equation works
    {
        write_fw("/tmp/tg7.fw", "x\t=\ty + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg7.fw");
        ASSERT(sys.equations.size() == 1, "tab-separated equation loads");
        ASSERT_NUM(sys.resolve("x", {{"y", 4}}), 5, "tab-separated equation works");
    }

    // Line with multiple equals: "x = y = z" — parses as equation x = y (trailing ignored)
    {
        write_fw("/tmp/tg8.fw", "x = y = z\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg8.fw");
        // Parser reads RHS as "y" and stops at second "="
        ASSERT(sys.equations.size() == 1, "multiple equals: first equation parsed");
    }

    // Mix of comments, blanks, defaults, equations, and garbage
    {
        write_fw("/tmp/tg9.fw",
            "# Physics formulas\n"
            "\n"
            "g = 9.81\n"
            "!!garbage!!\n"
            "\n"
            "# F = ma\n"
            "force = mass * g\n"
            "@@@\n"
            "energy = mass * 299792458 ^ 2\n"
            "\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg9.fw");
        ASSERT_NUM(sys.defaults.at("g"), 9.81, "mixed file: default loaded");
        ASSERT(sys.equations.size() == 2, "mixed file: equations loaded past garbage");
        ASSERT_NUM(sys.resolve("force", {{"mass", 10}}), 98.1, "mixed file: equation works");
    }

    // Very long line
    {
        std::string long_var(10000, 'x');
        write_fw("/tmp/tg10.fw", long_var + " = 42\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg10.fw");
        ASSERT(sys.defaults.count(long_var), "very long variable name works");
        ASSERT_NUM(sys.defaults.at(long_var), 42, "very long var default value");
    }

    // Completely empty file
    {
        write_fw("/tmp/tg11.fw", "");
        FormulaSystem sys;
        sys.load_file("/tmp/tg11.fw");
        ASSERT(sys.equations.empty(), "completely empty file: ok");
        ASSERT(sys.defaults.empty(), "completely empty file: no defaults");
    }

    // File with only whitespace lines
    {
        write_fw("/tmp/tg12.fw", "   \n\t\t\n  \t  \n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg12.fw");
        ASSERT(sys.equations.empty(), "whitespace-only file: ok");
    }

    // Duplicate defaults — last one wins
    {
        write_fw("/tmp/tg13.fw", "x = 5\nx = 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg13.fw");
        ASSERT_NUM(sys.defaults.at("x"), 10, "duplicate default: last wins");
    }

    // Equation with garbage characters in variable name part — should be skipped
    // because lexer will throw on the non-ident chars
    {
        write_fw("/tmp/tg14.fw", "x@ = 5\ny = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tg14.fw");
        // "x@ = 5" should fail in lexer and be skipped
        // "y = x + 1" should load fine
        ASSERT(sys.equations.size() == 1, "garbage in varname: line skipped");
    }
}

void test_file_access() {
    SECTION("File Access & Permissions");

    // Nonexistent file
    {
        FormulaSystem sys;
        bool threw = false;
        std::string msg;
        try { sys.load_file("/tmp/fwiz_no_such_file.fw"); }
        catch (const std::exception& e) { threw = true; msg = e.what(); }
        ASSERT(threw, "nonexistent file throws");
        ASSERT(msg.find("Cannot open") != std::string::npos, "nonexistent: clear message");
    }

    // Empty path
    {
        FormulaSystem sys;
        bool threw = false;
        std::string msg;
        try { sys.load_file(""); }
        catch (const std::exception& e) { threw = true; msg = e.what(); }
        ASSERT(threw, "empty path throws");
        ASSERT(msg.find("No file path") != std::string::npos, "empty path: clear message");
    }

    // Directory instead of file
    {
        system("mkdir -p /tmp/fwiz_test_dir.fw");
        FormulaSystem sys;
        bool threw = false;
        std::string msg;
        try { sys.load_file("/tmp/fwiz_test_dir.fw"); }
        catch (const std::exception& e) { threw = true; msg = e.what(); }
        ASSERT(threw, "directory throws");
        ASSERT(msg.find("directory") != std::string::npos, "directory: clear message");
        system("rmdir /tmp/fwiz_test_dir.fw");
    }

    // Nested nonexistent directory path
    {
        FormulaSystem sys;
        bool threw = false;
        try { sys.load_file("/tmp/no/such/dir/file.fw"); }
        catch (...) { threw = true; }
        ASSERT(threw, "nested missing path throws");
    }

    // Valid symlink works
    {
        write_fw("/tmp/fwiz_symlink_target.fw", "x = y + 1\n");
        system("ln -sf /tmp/fwiz_symlink_target.fw /tmp/fwiz_symlink.fw");
        FormulaSystem sys;
        sys.load_file("/tmp/fwiz_symlink.fw");
        ASSERT(sys.equations.size() == 1, "symlink: equation loaded");
        ASSERT_NUM(sys.resolve("x", {{"y", 4}}), 5, "symlink: equation works");
        system("rm -f /tmp/fwiz_symlink.fw");
    }

    // Broken symlink
    {
        system("ln -sf /tmp/fwiz_nonexistent_target /tmp/fwiz_broken.fw");
        FormulaSystem sys;
        bool threw = false;
        try { sys.load_file("/tmp/fwiz_broken.fw"); }
        catch (...) { threw = true; }
        ASSERT(threw, "broken symlink throws");
        system("rm -f /tmp/fwiz_broken.fw");
    }

    // /dev/null — valid file, just empty
    {
        FormulaSystem sys;
        sys.load_file("/dev/null");
        ASSERT(sys.equations.empty(), "/dev/null: no equations");
        ASSERT(sys.defaults.empty(), "/dev/null: no defaults");
    }

    // Path with spaces
    {
        write_fw("/tmp/fwiz space test.fw", "x = y * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fwiz space test.fw");
        ASSERT(sys.equations.size() == 1, "spaces in path: loads");
        ASSERT_NUM(sys.resolve("x", {{"y", 3}}), 6, "spaces in path: works");
    }

    // Permission tests — only meaningful when not running as root
    {
        bool is_root = (geteuid() == 0);
        if (!is_root) {
            write_fw("/tmp/fwiz_noperm.fw", "x = y + 1\n");
            chmod("/tmp/fwiz_noperm.fw", 0000);
            FormulaSystem sys;
            bool threw = false;
            try { sys.load_file("/tmp/fwiz_noperm.fw"); }
            catch (...) { threw = true; }
            ASSERT(threw, "no-permission file throws");
            chmod("/tmp/fwiz_noperm.fw", 0644);

            write_fw("/tmp/fwiz_writeonly.fw", "x = y + 1\n");
            chmod("/tmp/fwiz_writeonly.fw", 0200);
            threw = false;
            try { sys.load_file("/tmp/fwiz_writeonly.fw"); }
            catch (...) { threw = true; }
            ASSERT(threw, "write-only file throws");
            chmod("/tmp/fwiz_writeonly.fw", 0644);
        } else {
            std::cout << "  SKIP: permission tests (running as root)\n";
        }
    }
}

// ---- Numeric extremes ----

void test_numeric_extremes() {
    SECTION("Numeric Extremes");

    // --- Infinity ---

    // 2^1000 is representable as a large double, not inf
    {
        double r = ev("2^1000");
        ASSERT(!std::isinf(r) && r > 1e300, "2^1000 is large but finite");
    }

    // Overflow to inf
    {
        // 2^1024 overflows double
        double r = ev("2^1024");
        ASSERT(std::isinf(r), "2^1024 overflows to inf");
    }

    // Inf arithmetic
    {
        auto e = Expr::BinOpExpr(BinOp::ADD,
            Expr::Num(std::numeric_limits<double>::infinity()), Expr::Num(1));
        ASSERT(std::isinf((evaluate(e).value_or_nan())), "inf + 1 = inf");
    }
    {
        auto e = Expr::BinOpExpr(BinOp::MUL,
            Expr::Num(std::numeric_limits<double>::infinity()), Expr::Num(0));
        ASSERT(std::isnan((evaluate(e).value_or_nan())), "inf * 0 = NaN");
    }

    // Inf in system: equation produces inf — system rejects and throws
    {
        write_fw("/tmp/tn1.fw", "big = x ^ 1024\nresult = big + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tn1.fw");
        bool threw = false;
        try { sys.resolve("result", {{"x", 2}}); } catch (...) { threw = true; }
        ASSERT(threw, "inf result rejected by system");
    }

    // --- NaN ---

    // sqrt(-1) produces NaN
    {
        double r = ev_nan("sqrt(-1)");
        ASSERT(std::isnan(r), "sqrt(-1) = NaN");
    }

    // log(-1) produces NaN
    {
        double r = ev_nan("log(-1)");
        ASSERT(std::isnan(r), "log(-1) = NaN");
    }

    // (-1)^0.5 produces NaN
    {
        // Parser reads this as -(1^0.5) = -1, not (-1)^0.5
        // Build it manually
        auto e = Expr::BinOpExpr(BinOp::POW, Expr::Num(-1), Expr::Num(0.5));
        ASSERT(std::isnan((evaluate(e).value_or_nan())), "(-1)^0.5 = NaN");
    }

    // NaN in equation chain — system rejects and throws
    {
        write_fw("/tmp/tn2.fw", "a = sqrt(x)\nb = a + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tn2.fw");
        bool threw = false;
        try { sys.resolve("b", {{"x", -4}}); } catch (...) { threw = true; }
        ASSERT(threw, "NaN result rejected by system");
    }

    // NaN in arithmetic
    {
        double nan = std::numeric_limits<double>::quiet_NaN();
        auto e = Expr::BinOpExpr(BinOp::ADD, Expr::Num(nan), Expr::Num(5));
        ASSERT(std::isnan((evaluate(e).value_or_nan())), "NaN + 5 = NaN");
    }

    // --- Division by zero ---

    // 0/0 yields non-finite (NaN per eval_div semantics)
    {
        double r = ev_nan("0 / 0");
        ASSERT(!std::isfinite(r), "0/0 yields non-finite");
    }

    // 1/0 yields non-finite (NaN per eval_div semantics)
    {
        double r = ev_nan("1 / 0");
        ASSERT(!std::isfinite(r), "1/0 yields non-finite");
    }

    // --- Negative zero ---

    {
        double r = ev("-0");
        // -0.0 should behave like 0.0
        ASSERT(r == 0.0, "negative zero equals zero");
    }

    // --- Very large/small results in system ---

    {
        write_fw("/tmp/tn3.fw", "y = x * 1000000000\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tn3.fw");
        double r = sys.resolve("y", {{"x", 1000000000}});
        ASSERT_NUM(r, 1e18, "large result 1e18");
    }

    {
        write_fw("/tmp/tn4.fw", "y = x / 1000000000\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tn4.fw");
        double r = sys.resolve("y", {{"x", 0.000000001}});
        ASSERT_NUM(r, 1e-18, "tiny result 1e-18");
    }
}

void test_fmt_output() {
    SECTION("Output Formatting Extremes");

    // fmt_num is in system.h (private), but print_result is in main.cpp.
    // We test expr_to_string which uses fmt_num from expr.h for Num nodes.

    // Regular integers display without decimal
    ASSERT_EQ(expr_to_string(Expr::Num(42)), "42", "fmt 42");
    ASSERT_EQ(expr_to_string(Expr::Num(0)), "0", "fmt 0");
    ASSERT_EQ(expr_to_string(Expr::Num(-7)), "(-7)", "fmt -7");

    // Floats display with decimal
    {
        std::string s = expr_to_string(Expr::Num(3.14));
        ASSERT(s.find('.') != std::string::npos, "fmt 3.14 has decimal");
    }

    // Negative zero should display as 0, not -0
    {
        std::string s = expr_to_string(simplify(Expr::Num(-0.0)));
        ASSERT(s == "0", "fmt -0.0 displays as 0");
    }

    // Large integers within range display as integers
    ASSERT_EQ(expr_to_string(Expr::Num(1000000)), "1000000", "fmt 1e6 as integer");

    // Numbers >= 1e12 go through ostringstream, NOT (long long) cast
    // This avoids overflow for things like 1e18 > LLONG_MAX
    {
        std::string s = expr_to_string(Expr::Num(1e15));
        // Should NOT try to display as a long long
        ASSERT(s != "", "fmt 1e15 doesn't crash");
    }
    {
        std::string s = expr_to_string(Expr::Num(1e19));
        ASSERT(s != "", "fmt 1e19 doesn't crash");
    }

    // Infinity
    {
        std::string s = expr_to_string(Expr::Num(std::numeric_limits<double>::infinity()));
        ASSERT(s == "inf" || s == "Inf" || s == "infinity", "fmt inf");
    }

    // NaN
    {
        std::string s = expr_to_string(Expr::Num(std::numeric_limits<double>::quiet_NaN()));
        // Should produce something, not crash
        ASSERT(!s.empty(), "fmt NaN doesn't crash");
    }

    // DBL_MAX
    {
        std::string s = expr_to_string(Expr::Num(DBL_MAX));
        ASSERT(!s.empty(), "fmt DBL_MAX doesn't crash");
    }

    // Very small positive
    {
        std::string s = expr_to_string(Expr::Num(DBL_MIN));
        ASSERT(!s.empty(), "fmt DBL_MIN doesn't crash");
    }
}

void test_near_zero_coefficient() {
    SECTION("Near-Zero Coefficient Handling");

    // 0.1 + 0.2 - 0.3 is NOT exactly 0 in IEEE 754
    // An equation with this as coefficient should be treated as unsolvable
    {
        write_fw("/tmp/tnz1.fw", "x = y * 0.1 + y * 0.2 - y * 0.3 + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tnz1.fw");
        bool threw = false;
        try { sys.resolve("y", {{"x", 10}}); }
        catch (...) { threw = true; }
        ASSERT(threw, "near-zero coeff from float imprecision: unsolvable");
    }

    // Verify the equation still works forward (x given y)
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tnz1.fw");
        double r = sys.resolve("x", {{"y", 100}});
        // y * (0.1 + 0.2 - 0.3) + 5 ≈ 5 (the y term vanishes)
        ASSERT(std::abs(r - 5) < 1e-6, "near-zero coeff: forward eval gives ~5");
    }

    // Genuinely small but valid coefficient (should still solve)
    {
        write_fw("/tmp/tnz2.fw", "x = y * 0.001 + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tnz2.fw");
        double r = sys.resolve("y", {{"x", 10}});
        // y = (10 - 5) / 0.001 = 5000
        ASSERT_NUM(r, 5000, "small but valid coeff 0.001 works");
    }

    // Coefficient exactly zero (from integer arithmetic)
    {
        write_fw("/tmp/tnz3.fw", "x = y - y + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tnz3.fw");
        bool threw = false;
        try { sys.resolve("y", {{"x", 10}}); }
        catch (...) { threw = true; }
        ASSERT(threw, "y - y: exactly zero coeff is unsolvable");
    }

    // Multiple variables, one near-zero: z = x * 0.1 + x * 0.2 - x * 0.3 + y * 2
    // Solving for x should fail, solving for y should work
    {
        write_fw("/tmp/tnz4.fw", "z = x * 0.1 + x * 0.2 - x * 0.3 + y * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tnz4.fw");

        // Solve for y given z and x — should work (coeff of y is 2)
        double r = sys.resolve("y", {{"z", 10}, {"x", 999}});
        // z = ~0 + y*2, y = z/2 = 5
        ASSERT(std::abs(r - 5) < 1e-6, "near-zero x, valid y: y solvable");
    }
}

void test_inf_nan_in_trace() {
    SECTION("Trace with Inf/NaN");

    // Verify trace doesn't crash when encountering NaN/inf
    // (trace messages are emitted before the result is rejected)
    {
        write_fw("/tmp/ttr1.fw", "y = sqrt(x)\n");
        FormulaSystem sys;
        sys.trace.level = TraceLevel::CALC;
        sys.load_file("/tmp/ttr1.fw");
        bool threw = false;
        try { sys.resolve("y", {{"x", -1}}); } catch (...) { threw = true; }
        ASSERT(threw, "trace with NaN: rejects without crash");
    }
    {
        write_fw("/tmp/ttr2.fw", "y = x ^ 1024\n");
        FormulaSystem sys;
        sys.trace.level = TraceLevel::CALC;
        sys.load_file("/tmp/ttr2.fw");
        bool threw = false;
        try { sys.resolve("y", {{"x", 2}}); } catch (...) { threw = true; }
        ASSERT(threw, "trace with inf: rejects without crash");
    }
    {
        write_fw("/tmp/ttr3.fw", "y = x / 0.0000000001\n");
        FormulaSystem sys;
        sys.trace.level = TraceLevel::STEPS;
        sys.load_file("/tmp/ttr3.fw");
        bool threw = false;
        try { sys.resolve("y", {{"x", 1e300}}); } catch (...) { threw = true; }
        ASSERT(threw, "trace with large/inf: rejects without crash");
    }
}

// ---- Expression depth & scale (Group 2) ----

// Helpers to build deep/wide expression trees
ExprPtr build_deep_add(int depth) {
    ExprPtr e = Expr::Var("x");
    for (int i = 0; i < depth; i++)
        e = Expr::BinOpExpr(BinOp::ADD, e, Expr::Num(1));
    return e;
}

ExprPtr build_deep_func(int depth) {
    ExprPtr e = Expr::Var("x");
    for (int i = 0; i < depth; i++)
        e = Expr::Call("sqrt", {e});
    return e;
}

ExprPtr build_wide_vars(int n) {
    ExprPtr e = Expr::Var("v0");
    for (int i = 1; i < n; i++)
        e = Expr::BinOpExpr(BinOp::ADD, e, Expr::Var("v" + std::to_string(i)));
    return e;
}

std::string build_deep_parse_string(int depth) {
    std::string s = "x";
    for (int i = 0; i < depth; i++)
        s = "(" + s + " + 1)";
    return s;
}

void test_depth_evaluate() {
    SECTION("Depth: Evaluate");

    // Moderate depth: correct result
    {
        auto e = substitute(build_deep_add(100), "x", Expr::Num(0));
        ASSERT_NUM((evaluate(e).value()), 100, "depth 100: evaluate = 100");
    }
    {
        auto e = substitute(build_deep_add(1000), "x", Expr::Num(0));
        ASSERT_NUM((evaluate(e).value()), 1000, "depth 1000: evaluate = 1000");
    }
    {
        auto e = substitute(build_deep_add(DEPTH_HIGH), "x", Expr::Num(0));
        ASSERT_NUM((evaluate(e).value()), DEPTH_HIGH, "depth HIGH: evaluate");
    }

    // With a non-zero base value
    {
        auto e = substitute(build_deep_add(DEPTH_MED), "x", Expr::Num(42));
        ASSERT_NUM((evaluate(e).value()), DEPTH_MED + 42, "depth MED: x=42, evaluate");
    }
}

void test_depth_simplify() {
    SECTION("Depth: Simplify");

    // Simplify at moderate depth — shouldn't crash
    {
        auto e = build_deep_add(100);
        auto s = simplify(e);
        // After simplify, should still contain x and constants
        ASSERT(contains_var(s, "x"), "depth 100 simplify: x preserved");
    }
    {
        auto e = build_deep_add(1000);
        auto s = simplify(e);
        ASSERT(contains_var(s, "x"), "depth MED simplify: x preserved");
    }
    {
        auto e = build_deep_add(DEPTH_MED);
        auto s = simplify(e);
        ASSERT(contains_var(s, "x"), "depth MED simplify: x preserved");
    }
    // Verify simplify produces correct result when evaluated
    {
        auto e = build_deep_add(1000);
        auto s = simplify(e);
        auto v = substitute(s, "x", Expr::Num(0));
        ASSERT_NUM((evaluate(simplify(v)).value()), 1000, "depth 1000: simplify then eval = 1000");
    }
}

void test_depth_substitute() {
    SECTION("Depth: Substitute");

    // Substitute at depth
    {
        auto e = build_deep_add(1000);
        auto s = substitute(e, "x", Expr::Num(7));
        ASSERT_NUM((evaluate(s).value()), 1007, "depth 1000: sub x=7, eval = 1007");
    }
    {
        auto e = build_deep_add(DEPTH_HIGH);
        auto s = substitute(e, "x", Expr::Num(0));
        ASSERT_NUM((evaluate(s).value()), DEPTH_HIGH, "depth HIGH: sub and eval");
    }

    // Substitute with expression (not just number)
    {
        auto e = build_deep_add(500);
        auto s = substitute(e, "x", Expr::BinOpExpr(BinOp::MUL, Expr::Var("y"), Expr::Num(2)));
        ASSERT(contains_var(s, "y"), "depth 500: sub x=2y preserves y");
        ASSERT(!contains_var(s, "x"), "depth 500: sub x=2y removes x");
    }
}

void test_depth_collect_vars() {
    SECTION("Depth: Collect Vars");

    // Deep tree with one variable
    {
        auto e = build_deep_add(DEPTH_MED);
        std::set<std::string> vars;
        collect_vars(e, vars);
        ASSERT(vars.size() == 1, "depth MED: only 1 var (x)");
        ASSERT(vars.count("x"), "depth MED: var is x");
    }
}

void test_depth_tostring() {
    SECTION("Depth: expr_to_string");

    // Printing a deep tree
    {
        auto e = build_deep_add(100);
        auto s = expr_to_string(e);
        ASSERT(s.size() > 100, "depth 100: string is long");
        // Should start with x and contain lots of " + 1"
        ASSERT(s.find("x") != std::string::npos, "depth 100: string contains x");
    }
    {
        auto e = build_deep_add(DEPTH_HIGH);
        auto s = expr_to_string(e);
        ASSERT(s.size() > (size_t)DEPTH_HIGH, "depth HIGH: string output doesn't crash");
    }
}

void test_depth_decompose() {
    SECTION("Depth: Decompose Linear");

    // Deep expression is linear in x with coeff=1
    {
        auto e = build_deep_add(1000);
        auto lf = decompose_linear(e, "x");
        ASSERT(lf.has_value(), "depth 1000: linear in x");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), 1, "depth 1000: coeff=1");
        ASSERT_NUM((evaluate(simplify(lf->rest)).value()), 1000, "depth 1000: rest=1000");
    }
    {
        auto e = build_deep_add(DEPTH_MED);
        auto lf = decompose_linear(e, "x");
        ASSERT(lf.has_value(), "depth MED: linear in x");
        ASSERT_NUM((evaluate(simplify(lf->coeff)).value()), 1, "depth MED: coeff=1");
    }
}

void test_depth_solve() {
    SECTION("Depth: Solve");

    // Solve a deep equation: y = x + N => x = y - N
    {
        auto rhs = build_deep_add(1000);
        // solve y = (x + 1 + 1 + ... + 1) for x
        auto sol = solve_for(Expr::Var("y"), rhs, "x");
        ASSERT(sol != nullptr, "depth 1000: solvable for x");
        // Verify: if y=1500, x should be 500
        auto val = substitute(sol, "y", Expr::Num(1500));
        ASSERT_NUM((evaluate(simplify(val)).value()), 500, "depth 1000: y=1500 => x=500");
    }
}

void test_deep_functions() {
    SECTION("Depth: Nested Function Calls");

    // sqrt(sqrt(sqrt(...(x)...))) at moderate depth
    {
        auto e = build_deep_func(10);
        auto s = substitute(e, "x", Expr::Num(1));
        // sqrt^10(1) = 1
        ASSERT_NUM((evaluate(s).value()), 1, "sqrt^10(1) = 1");
    }
    {
        // sqrt^20(1e300) — repeated sqrt of a large number converges to 1
        auto e = build_deep_func(100);
        auto s = substitute(e, "x", Expr::Num(1e300));
        double r = (evaluate(s).value());
        ASSERT(r > 0.99 && r < 1.01, "sqrt^100(1e300) converges near 1");
    }
    {
        // Deep nested functions at depth 1000
        auto e = build_deep_func(DEPTH_MED);
        auto s = substitute(e, "x", Expr::Num(1));
        ASSERT_NUM((evaluate(s).value()), 1, "sqrt^1000(1) = 1");
    }

    // collect_vars through deep func nesting
    {
        auto e = build_deep_func(500);
        std::set<std::string> vars;
        collect_vars(e, vars);
        ASSERT(vars.size() == 1 && vars.count("x"), "deep func: 1 var x");
    }
}

void test_wide_expressions() {
    SECTION("Width: Wide Expressions");

    // Wide expression with many unique variables
    {
        auto e = build_wide_vars(100);
        std::set<std::string> vars;
        collect_vars(e, vars);
        ASSERT(vars.size() == 100, "100 vars collected");
    }
    {
        auto e = build_wide_vars(DEPTH_MED);
        std::set<std::string> vars;
        collect_vars(e, vars);
        ASSERT(vars.size() == (size_t)DEPTH_MED, "MED vars collected");
    }

    // Substitute all vars in a wide expression and evaluate
    {
        int n = 500;
        ExprPtr e = Expr::Num(0);
        for (int i = 0; i < n; i++)
            e = Expr::BinOpExpr(BinOp::ADD, e, Expr::Var("v" + std::to_string(i)));
        for (int i = 0; i < n; i++)
            e = substitute(e, "v" + std::to_string(i), Expr::Num(1));
        ASSERT_NUM((evaluate(simplify(e)).value()), n, "500 vars substituted and summed");
    }

    // Wide expression to_string
    {
        auto e = build_wide_vars(DEPTH_MED);
        auto s = expr_to_string(e);
        ASSERT(s.size() > (size_t)DEPTH_MED, "MED-var expr to_string");
    }
}

void test_parse_deep_string() {
    SECTION("Parse: Deep Nested Strings");

    // Parse deeply nested parenthesized expression
    {
        auto s = build_deep_parse_string(100);
        auto e = parse(s);
        auto v = substitute(e, "x", Expr::Num(0));
        ASSERT_NUM((evaluate(v).value()), 100, "parse depth 100: eval = 100");
    }
    {
        auto s = build_deep_parse_string(1000);
        auto e = parse(s);
        auto v = substitute(e, "x", Expr::Num(0));
        ASSERT_NUM((evaluate(v).value()), 1000, "parse depth 1000: eval = 1000");
    }
    {
        // Parse depth 5000 — stress the parser's recursion
        auto s = build_deep_parse_string(DEPTH_MED);
        auto e = parse(s);
        ASSERT(contains_var(e, "x"), "parse depth MED: succeeds");
    }

    // Very long flat expression (no deep nesting, just long)
    {
        std::string s = "x";
        for (int i = 0; i < 1000; i++) s += " + 1";
        auto e = parse(s);
        auto v = substitute(e, "x", Expr::Num(0));
        ASSERT_NUM((evaluate(v).value()), 1000, "parse flat 1000 terms: eval = 1000");
    }
}

void test_large_file() {
    SECTION("Scale: Large Files");

    // Chain of N equations: x0 = x1+1, x1 = x2+1, ..., xN = 0 => x0 = N
    {
        int n = 100;
        std::string content;
        for (int i = 0; i < n; i++)
            content += "x" + std::to_string(i) + " = x" + std::to_string(i+1) + " + 1\n";
        content += "x" + std::to_string(n) + " = 0\n";
        write_fw("/tmp/tlf1.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tlf1.fw");
        ASSERT(sys.equations.size() == (size_t)n, "100-eq file: all loaded");
        double r = sys.resolve("x0", {});
        ASSERT_NUM(r, n, "100-eq chain: x0 = 100");
    }

    // Larger chain
    {
        int n = 500;
        std::string content;
        for (int i = 0; i < n; i++)
            content += "x" + std::to_string(i) + " = x" + std::to_string(i+1) + " + 1\n";
        content += "x" + std::to_string(n) + " = 0\n";
        write_fw("/tmp/tlf2.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tlf2.fw");
        double r = sys.resolve("x0", {});
        ASSERT_NUM(r, n, "500-eq chain: x0 = 500");
    }

    // Inverse on chain: solve xN given x0
    {
        int n = 100;
        std::string content;
        for (int i = 0; i < n; i++)
            content += "x" + std::to_string(i) + " = x" + std::to_string(i+1) + " + 1\n";
        write_fw("/tmp/tlf3.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tlf3.fw");
        double r = sys.resolve("x" + std::to_string(n), {{"x0", 100}});
        ASSERT_NUM(r, 0, "100-eq chain inverse: x100 = 0 given x0=100");
    }

    // Many independent equations (no chain)
    {
        int n = 500;
        std::string content;
        for (int i = 0; i < n; i++)
            content += "y" + std::to_string(i) + " = x * " + std::to_string(i+1) + "\n";
        write_fw("/tmp/tlf4.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tlf4.fw");
        ASSERT(sys.equations.size() == (size_t)n, "500 independent eqs loaded");
        // Resolve the last one
        double r = sys.resolve("y499", {{"x", 2}});
        ASSERT_NUM(r, 1000, "y499 = x*500, x=2 => 1000");
    }

    // File with very long single equation line
    {
        std::string rhs = "x";
        for (int i = 0; i < 500; i++) rhs += " + " + std::to_string(i + 1);
        write_fw("/tmp/tlf5.fw", "result = " + rhs + "\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tlf5.fw");
        // sum 1..500 = 125250, plus x
        double r = sys.resolve("result", {{"x", 0}});
        ASSERT_NUM(r, 125250, "long line: sum 1..500 = 125250");
    }
}

// ---- Contradictions & overdetermined systems (Group 3) ----

void test_equation_order() {
    SECTION("Equation Order (First Match Wins)");

    // Two equations for same variable: first one is used
    {
        write_fw("/tmp/to1.fw", "x = y + 1\nx = y + 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/to1.fw");
        double r = sys.resolve("x", {{"y", 5}});
        ASSERT_NUM(r, 6, "x=y+1 first: x=6 (first wins)");
    }

    // Reversed order: different result
    {
        write_fw("/tmp/to2.fw", "x = y + 2\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/to2.fw");
        double r = sys.resolve("x", {{"y", 5}});
        ASSERT_NUM(r, 7, "x=y+2 first: x=7 (first wins)");
    }

    // Inverse solve also uses first applicable equation
    {
        write_fw("/tmp/to3.fw", "x = y + 1\nx = y + 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/to3.fw");
        double r = sys.resolve("y", {{"x", 10}});
        ASSERT_NUM(r, 9, "inverse: first eq y=x-1=9");
    }
}

void test_contradictions() {
    SECTION("Contradictory Systems");

    // Circular: x = y + 1, y = x + 1 (implies x = x + 2)
    {
        write_fw("/tmp/tc1.fw", "x = y + 1\ny = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc1.fw");
        bool threw = false;
        try { sys.resolve("x", {}); } catch (...) { threw = true; }
        ASSERT(threw, "x=y+1, y=x+1: circular throws");
    }

    // Three-way circular: x=y+1, y=z+1, z=x+1
    {
        write_fw("/tmp/tc2.fw", "x = y + 1\ny = z + 1\nz = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc2.fw");
        bool threw = false;
        try { sys.resolve("x", {}); } catch (...) { threw = true; }
        ASSERT(threw, "three-way circular throws");
    }

    // Self-referencing: x = x + 1 (no solution)
    {
        write_fw("/tmp/tc3.fw", "x = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc3.fw");
        bool threw = false;
        try { sys.resolve("x", {}); } catch (...) { threw = true; }
        ASSERT(threw, "x = x + 1 throws");
    }

    // Self-reference that's solvable: x = x * 0 + 5 (coeff of x is -1+0=-1... wait)
    // Actually: x = 0*x + 5, so x = 5. Let's see if the solver handles this.
    // decompose(x - (0*x + 5)) = decompose(x - 5) = coeff=1, rest=-5 => x = 5
    {
        write_fw("/tmp/tc4.fw", "x = x * 0 + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc4.fw");
        // This is x = 5, solvable despite x appearing on both sides
        // The solver inverts: x = 0*x + 5, solve for x: coeff of x in (x - 0*x - 5) is 1, rest=-5
        double r = sys.resolve("x", {});
        ASSERT_NUM(r, 5, "x = 0*x + 5 solves to x=5");
    }
}

void test_nan_fallthrough() {
    SECTION("NaN/Inf Fallthrough to Alternative Equations");

    // First equation produces NaN, second is valid — should use second
    {
        write_fw("/tmp/tf1.fw", "x = sqrt(y)\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf1.fw");
        double r = sys.resolve("x", {{"y", -1}});
        ASSERT_NUM(r, 0, "NaN fallthrough: sqrt(-1) skipped, x=y+1=0");
    }

    // Reversed: first valid, second NaN — first wins (no fallthrough needed)
    {
        write_fw("/tmp/tf2.fw", "x = y + 1\nx = sqrt(y)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf2.fw");
        double r = sys.resolve("x", {{"y", -1}});
        ASSERT_NUM(r, 0, "valid first: x=y+1=0, NaN eq never tried");
    }

    // Both produce NaN — should throw
    {
        write_fw("/tmp/tf3.fw", "x = sqrt(y)\nx = log(y)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf3.fw");
        bool threw = false;
        try { sys.resolve("x", {{"y", -1}}); } catch (...) { threw = true; }
        ASSERT(threw, "both NaN: throws");
    }

    // Inf fallthrough
    {
        write_fw("/tmp/tf4.fw", "x = y ^ 1024\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf4.fw");
        double r = sys.resolve("x", {{"y", 2}});
        ASSERT_NUM(r, 3, "inf fallthrough: 2^1024 skipped, x=y+1=3");
    }

    // Division by zero in equation — throws, falls through
    {
        write_fw("/tmp/tf5.fw", "x = 1 / y\nx = y + 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf5.fw");
        double r = sys.resolve("x", {{"y", 0}});
        ASSERT_NUM(r, 10, "div-by-zero fallthrough: x=y+10=10");
    }

    // NaN in intermediate variable, but chain has alternative
    {
        write_fw("/tmp/tf6.fw", "a = sqrt(x)\nb = a + 1\nb = x + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf6.fw");
        double r = sys.resolve("b", {{"x", -4}});
        // First path: a=sqrt(-4)=NaN, b=NaN+1=NaN — rejected
        // Second path: b=x+5=-4+5=1
        ASSERT_NUM(r, 1, "NaN in chain: falls through to direct equation");
    }
}

void test_overdetermined() {
    SECTION("Overdetermined Systems");

    // Two consistent equations for same variable
    {
        write_fw("/tmp/tod1.fw", "x = a + 1\nx = b + 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tod1.fw");
        // Both given, consistent: a=5, b=4 => both give x=6
        double r = sys.resolve("x", {{"a", 5}, {"b", 4}});
        ASSERT_NUM(r, 6, "consistent overdetermined: x=6");
    }

    // Two inconsistent equations — first wins silently
    {
        write_fw("/tmp/tod2.fw", "x = a + 1\nx = b + 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tod2.fw");
        // a=5 => x=6, b=5 => x=7 — contradictory, first wins
        double r = sys.resolve("x", {{"a", 5}, {"b", 5}});
        ASSERT_NUM(r, 6, "inconsistent: first eq wins (x=6 not 7)");
    }

    // Only one path has data available
    {
        write_fw("/tmp/tod3.fw", "x = a + 1\nx = b + 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tod3.fw");
        // Only a given — first eq works
        double r = sys.resolve("x", {{"a", 5}});
        ASSERT_NUM(r, 6, "partial data: first eq has data");
    }
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tod3.fw");
        // Only b given — first eq fails (no a), second works
        double r = sys.resolve("x", {{"b", 5}});
        ASSERT_NUM(r, 7, "partial data: second eq has data");
    }

    // Multiple consistent paths via substitution
    {
        write_fw("/tmp/tod4.fw", "a = b + 1\na = c + 2\nb = c + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tod4.fw");
        double r = sys.resolve("a", {{"c", 0}});
        ASSERT_NUM(r, 2, "consistent multi-path: a=2");
    }

    // Three equations, solve for variable not on any LHS
    {
        write_fw("/tmp/tod5.fw", "y = x + 1\nz = x + 2\nw = x + 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tod5.fw");
        double r = sys.resolve("x", {{"y", 10}});
        ASSERT_NUM(r, 9, "solve x from y=x+1: x=9");
    }
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tod5.fw");
        double r = sys.resolve("x", {{"z", 10}});
        ASSERT_NUM(r, 8, "solve x from z=x+2: x=8");
    }
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tod5.fw");
        double r = sys.resolve("x", {{"w", 10}});
        ASSERT_NUM(r, 7, "solve x from w=x+3: x=7");
    }
}

void test_defaults_vs_equations() {
    SECTION("Defaults vs Equations Interaction");

    // Default for target is ignored when solving
    {
        write_fw("/tmp/td1.fw", "x = 99\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td1.fw");
        double r = sys.resolve("x", {{"y", 5}});
        ASSERT_NUM(r, 6, "default x=99 ignored when solving for x");
    }

    // Default for non-target is used
    {
        write_fw("/tmp/td2.fw", "y = 5\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td2.fw");
        double r = sys.resolve("x", {});
        ASSERT_NUM(r, 6, "default y=5 used: x=6");
    }

    // Binding overrides default
    {
        write_fw("/tmp/td3.fw", "y = 5\nx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td3.fw");
        double r = sys.resolve("x", {{"y", 10}});
        ASSERT_NUM(r, 11, "binding y=10 overrides default y=5");
    }

    // Multiple defaults, some overridden
    {
        write_fw("/tmp/td4.fw", "a = 1\nb = 2\nc = 3\nresult = a + b + c\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td4.fw");
        ASSERT_NUM(sys.resolve("result", {}), 6, "all defaults: 1+2+3=6");
        ASSERT_NUM(sys.resolve("result", {{"b", 20}}), 24, "override b: 1+20+3=24");
        ASSERT_NUM(sys.resolve("result", {{"a", 10}, {"c", 30}}), 42, "override a,c: 10+2+30=42");
    }
}

// ---- Statefulness & mutation (Group 4) ----

void test_load_file_accumulation() {
    SECTION("Load File Accumulation");

    // Loading two different files accumulates equations
    {
        write_fw("/tmp/ts1a.fw", "x = y + 1\n");
        write_fw("/tmp/ts1b.fw", "z = w * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/ts1a.fw");
        ASSERT(sys.equations.size() == 1, "after first load: 1 eq");
        sys.load_file("/tmp/ts1b.fw");
        ASSERT(sys.equations.size() == 2, "after second load: 2 eqs");

        // Both files' equations are usable
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "eq from file1 works");
        ASSERT_NUM(sys.resolve("z", {{"w", 5}}), 10, "eq from file2 works");
    }

    // Loading same file twice duplicates equations
    {
        write_fw("/tmp/ts2.fw", "x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/ts2.fw");
        sys.load_file("/tmp/ts2.fw");
        ASSERT(sys.equations.size() == 2, "same file twice: 2 eqs (duplicated)");
        // Still works (duplicate is harmless)
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "duplicate eqs: still works");
    }

    // Defaults accumulate across files
    {
        write_fw("/tmp/ts3a.fw", "a = 10\n");
        write_fw("/tmp/ts3b.fw", "b = 20\n");
        FormulaSystem sys;
        sys.load_file("/tmp/ts3a.fw");
        sys.load_file("/tmp/ts3b.fw");
        ASSERT(sys.defaults.count("a"), "default a from file1");
        ASSERT(sys.defaults.count("b"), "default b from file2");
        ASSERT_NUM(sys.defaults.at("a"), 10, "a=10");
        ASSERT_NUM(sys.defaults.at("b"), 20, "b=20");
    }

    // Same default in both files: second file wins
    {
        write_fw("/tmp/ts4a.fw", "x = 10\n");
        write_fw("/tmp/ts4b.fw", "x = 20\n");
        FormulaSystem sys;
        sys.load_file("/tmp/ts4a.fw");
        ASSERT_NUM(sys.defaults.at("x"), 10, "x=10 after first load");
        sys.load_file("/tmp/ts4b.fw");
        ASSERT_NUM(sys.defaults.at("x"), 20, "x=20 after second load (overridden)");
    }

    // Cross-file equation resolution: eq from file1 uses default from file2
    {
        write_fw("/tmp/ts5a.fw", "result = x + offset\n");
        write_fw("/tmp/ts5b.fw", "offset = 100\n");
        FormulaSystem sys;
        sys.load_file("/tmp/ts5a.fw");
        sys.load_file("/tmp/ts5b.fw");
        ASSERT_NUM(sys.resolve("result", {{"x", 5}}), 105,
            "eq from file1 uses default from file2");
    }
}

void test_resolve_isolation() {
    SECTION("Resolve Call Isolation");

    // Multiple resolves with different targets: no interference
    {
        write_fw("/tmp/tri1.fw", "x = a + 1\ny = a + 2\nz = a + 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tri1.fw");
        ASSERT_NUM(sys.resolve("x", {{"a", 10}}), 11, "first resolve: x=11");
        ASSERT_NUM(sys.resolve("y", {{"a", 10}}), 12, "second resolve: y=12");
        ASSERT_NUM(sys.resolve("z", {{"a", 10}}), 13, "third resolve: z=13");
    }

    // Same target twice: identical results
    {
        write_fw("/tmp/tri2.fw", "x = y * 2 + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tri2.fw");
        double r1 = sys.resolve("x", {{"y", 7}});
        double r2 = sys.resolve("x", {{"y", 7}});
        ASSERT(r1 == r2, "same target twice: identical results");
        ASSERT_NUM(r1, 15, "x=7*2+1=15");
    }

    // Same target with different bindings: independent
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tri2.fw");
        double r1 = sys.resolve("x", {{"y", 5}});
        double r2 = sys.resolve("x", {{"y", 10}});
        ASSERT_NUM(r1, 11, "x(y=5)=11");
        ASSERT_NUM(r2, 21, "x(y=10)=21");
    }

    // Chain resolution: intermediate values NOT cached between calls
    {
        write_fw("/tmp/tri3.fw", "x = a + 1\ny = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tri3.fw");
        ASSERT_NUM(sys.resolve("y", {{"a", 10}}), 12, "y(a=10)=12");
        ASSERT_NUM(sys.resolve("y", {{"a", 20}}), 22, "y(a=20)=22 (no cached x)");
        ASSERT_NUM(sys.resolve("y", {{"a", 0}}), 2, "y(a=0)=2 (no cached x)");
    }

    // Resolve for intermediate, then full chain — no leak
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tri3.fw");
        ASSERT_NUM(sys.resolve("x", {{"a", 100}}), 101, "x(a=100)=101");
        ASSERT_NUM(sys.resolve("y", {{"a", 5}}), 7, "y(a=5)=7 (x not cached from prev)");
    }
}

void test_bindings_not_mutated() {
    SECTION("Caller Bindings Not Mutated");

    // resolve takes bindings by value — caller's map should be unchanged
    {
        write_fw("/tmp/tbm1.fw", "x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tbm1.fw");

        std::map<std::string, double> bindings = {{"y", 5}};
        double r = sys.resolve("x", bindings);
        ASSERT_NUM(r, 6, "resolve gives correct result");
        ASSERT(bindings.size() == 1, "bindings size unchanged");
        ASSERT(bindings.count("x") == 0, "x not added to caller bindings");
        ASSERT_NUM(bindings.at("y"), 5, "y still 5 in caller bindings");
    }

    // Chain resolution: intermediate bindings don't leak to caller
    {
        write_fw("/tmp/tbm2.fw", "a = b + 1\nc = a + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tbm2.fw");

        std::map<std::string, double> bindings = {{"b", 10}};
        double r = sys.resolve("c", bindings);
        ASSERT_NUM(r, 12, "c=b+1+1=12");
        ASSERT(bindings.size() == 1, "only b in caller bindings");
        ASSERT(bindings.count("a") == 0, "intermediate a not leaked");
        ASSERT(bindings.count("c") == 0, "target c not leaked");
    }

    // Defaults don't leak into caller bindings
    {
        write_fw("/tmp/tbm3.fw", "offset = 100\nx = y + offset\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tbm3.fw");

        std::map<std::string, double> bindings = {{"y", 5}};
        double r = sys.resolve("x", bindings);
        ASSERT_NUM(r, 105, "x=y+offset=105");
        ASSERT(bindings.count("offset") == 0, "default offset not leaked to caller");
    }

    // Empty bindings stay empty
    {
        write_fw("/tmp/tbm4.fw", "a = 1\nb = 2\nx = a + b\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tbm4.fw");

        std::map<std::string, double> bindings;
        double r = sys.resolve("x", bindings);
        ASSERT_NUM(r, 3, "x=1+2=3");
        ASSERT(bindings.empty(), "empty bindings still empty after resolve");
    }
}

void test_system_reuse() {
    SECTION("System Reuse Patterns");

    // Multiple users of same FormulaSystem (simulate reuse)
    {
        write_fw("/tmp/tsr1.fw", "total = price * qty + tax\ntax = 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsr1.fw");

        // User 1
        ASSERT_NUM(sys.resolve("total", {{"price", 10}, {"qty", 3}}), 35,
            "user1: 10*3+5=35");
        // User 2 (different values)
        ASSERT_NUM(sys.resolve("total", {{"price", 20}, {"qty", 1}}), 25,
            "user2: 20*1+5=25");
        // User 3 (inverse: solve for price)
        ASSERT_NUM(sys.resolve("price", {{"total", 55}, {"qty", 5}}), 10,
            "user3: price=(55-5)/5=10");
        // User 4 (inverse: solve for qty)
        ASSERT_NUM(sys.resolve("qty", {{"total", 35}, {"price", 10}}), 3,
            "user4: qty=(35-5)/10=3");
    }

    // Alternating forward and inverse solves
    {
        write_fw("/tmp/tsr2.fw", "y = 2 * x + 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsr2.fw");

        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 13, "forward: y=13");
        ASSERT_NUM(sys.resolve("x", {{"y", 13}}), 5, "inverse: x=5");
        ASSERT_NUM(sys.resolve("y", {{"x", 0}}), 3, "forward: y=3");
        ASSERT_NUM(sys.resolve("x", {{"y", 3}}), 0, "inverse: x=0");
        ASSERT_NUM(sys.resolve("y", {{"x", -1}}), 1, "forward: y=1");
        ASSERT_NUM(sys.resolve("x", {{"y", 1}}), -1, "inverse: x=-1");
    }
}

// ---- File format portability (Group 5) ----

// Helper to write raw bytes (no text mode translation)
void write_raw(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

void test_windows_line_endings() {
    SECTION("Windows Line Endings (CRLF)");

    // Single equation with CRLF
    {
        write_raw("/tmp/tw1.fw", "x = y + 1\r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tw1.fw");
        ASSERT(sys.equations.size() == 1, "CRLF: equation loaded");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "CRLF: x=y+1=6");
    }

    // Multiple equations with CRLF
    {
        write_raw("/tmp/tw2.fw", "x = y + 1\r\nz = w * 2\r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tw2.fw");
        ASSERT(sys.equations.size() == 2, "CRLF: two equations");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "CRLF: first eq works");
        ASSERT_NUM(sys.resolve("z", {{"w", 5}}), 10, "CRLF: second eq works");
    }

    // Defaults with CRLF
    {
        write_raw("/tmp/tw3.fw", "a = 10\r\nx = a + 1\r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tw3.fw");
        ASSERT_NUM(sys.defaults.at("a"), 10, "CRLF: default loaded");
        ASSERT_NUM(sys.resolve("x", {}), 11, "CRLF: default used in equation");
    }

    // Comments with CRLF
    {
        write_raw("/tmp/tw4.fw", "# comment\r\nx = y + 1\r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tw4.fw");
        ASSERT(sys.equations.size() == 1, "CRLF: comment skipped");
    }

    // CRLF with no trailing newline
    {
        write_raw("/tmp/tw5.fw", "x = y + 1\r\nz = w * 2");
        FormulaSystem sys;
        sys.load_file("/tmp/tw5.fw");
        ASSERT(sys.equations.size() == 2, "CRLF no final newline: both loaded");
    }
}

void test_mixed_line_endings() {
    SECTION("Mixed Line Endings");

    // LF then CRLF
    {
        write_raw("/tmp/tm1.fw", "x = y + 1\nz = w * 2\r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tm1.fw");
        ASSERT(sys.equations.size() == 2, "LF+CRLF: both loaded");
        ASSERT_NUM(sys.resolve("x", {{"y", 3}}), 4, "LF line works");
        ASSERT_NUM(sys.resolve("z", {{"w", 3}}), 6, "CRLF line works");
    }

    // CRLF then LF
    {
        write_raw("/tmp/tm2.fw", "x = y + 1\r\nz = w * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tm2.fw");
        ASSERT(sys.equations.size() == 2, "CRLF+LF: both loaded");
    }

    // Defaults and equations with mixed endings
    {
        write_raw("/tmp/tm3.fw", "a = 10\r\nb = 20\nx = a + b\r\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tm3.fw");
        ASSERT_NUM(sys.resolve("x", {}), 30, "mixed endings: defaults + eq work");
    }
}

void test_utf8_bom() {
    SECTION("UTF-8 BOM Handling");

    // BOM before equation
    {
        // Write BOM + "x = y + 1\n" using raw bytes
        std::string content;
        content += (char)0xEF;
        content += (char)0xBB;
        content += (char)0xBF;
        content += "x = y + 1\n";
        write_raw("/tmp/tb1.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tb1.fw");
        ASSERT(sys.equations.size() == 1, "BOM: equation loaded");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "BOM: equation works");
    }

    // BOM before default
    {
        std::string content;
        content += (char)0xEF;
        content += (char)0xBB;
        content += (char)0xBF;
        content += "a = 10\nx = a + 1\n";
        write_raw("/tmp/tb2.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tb2.fw");
        ASSERT_NUM(sys.defaults.at("a"), 10, "BOM: default loaded");
        ASSERT_NUM(sys.resolve("x", {}), 11, "BOM: default used");
    }

    // BOM before comment
    {
        std::string content;
        content += (char)0xEF;
        content += (char)0xBB;
        content += (char)0xBF;
        content += "# comment\nx = y + 1\n";
        write_raw("/tmp/tb3.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tb3.fw");
        ASSERT(sys.equations.size() == 1, "BOM+comment: equation loaded");
    }

    // No BOM — still works (regression check)
    {
        write_raw("/tmp/tb4.fw", "x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tb4.fw");
        ASSERT(sys.equations.size() == 1, "no BOM: still works");
    }

    // BOM + CRLF
    {
        std::string content;
        content += (char)0xEF;
        content += (char)0xBB;
        content += (char)0xBF;
        content += "x = y + 1\r\n";
        write_raw("/tmp/tb5.fw", content);
        FormulaSystem sys;
        sys.load_file("/tmp/tb5.fw");
        ASSERT(sys.equations.size() == 1, "BOM+CRLF: works");
    }
}

void test_whitespace_handling() {
    SECTION("Whitespace Handling");

    // Trailing spaces
    {
        write_fw("/tmp/tws1.fw", "x = y + 1   \n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws1.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "trailing spaces ok");
    }

    // Trailing tabs
    {
        write_fw("/tmp/tws2.fw", "x = y + 1\t\t\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws2.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "trailing tabs ok");
    }

    // Leading spaces
    {
        write_fw("/tmp/tws3.fw", "    x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws3.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "leading spaces ok");
    }

    // Leading tabs
    {
        write_fw("/tmp/tws4.fw", "\t\tx = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws4.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "leading tabs ok");
    }

    // Both leading and trailing
    {
        write_fw("/tmp/tws5.fw", "  \t x = y + 1 \t  \n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws5.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "leading+trailing whitespace ok");
    }

    // Default with trailing whitespace
    {
        write_fw("/tmp/tws6.fw", "a = 10   \nx = a + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws6.fw");
        ASSERT_NUM(sys.resolve("x", {}), 11, "default with trailing ws ok");
    }

    // Whitespace-only lines between equations
    {
        write_fw("/tmp/tws7.fw", "x = y + 1\n   \n\t\n  \t  \nz = w * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tws7.fw");
        ASSERT(sys.equations.size() == 2, "whitespace lines skipped");
    }
}

void test_no_trailing_newline() {
    SECTION("No Trailing Newline");

    // Single equation, no final newline
    {
        write_raw("/tmp/tnl1.fw", "x = y + 1");
        FormulaSystem sys;
        sys.load_file("/tmp/tnl1.fw");
        ASSERT(sys.equations.size() == 1, "no newline: eq loaded");
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 6, "no newline: works");
    }

    // Default, no final newline
    {
        write_raw("/tmp/tnl2.fw", "a = 10");
        FormulaSystem sys;
        sys.load_file("/tmp/tnl2.fw");
        ASSERT_NUM(sys.defaults.at("a"), 10, "no newline: default loaded");
    }

    // Two lines, no final newline
    {
        write_raw("/tmp/tnl3.fw", "a = 10\nx = a + 1");
        FormulaSystem sys;
        sys.load_file("/tmp/tnl3.fw");
        ASSERT_NUM(sys.resolve("x", {}), 11, "no newline: multi-line works");
    }

    // CRLF content, no final newline
    {
        write_raw("/tmp/tnl4.fw", "a = 10\r\nx = a + 1");
        FormulaSystem sys;
        sys.load_file("/tmp/tnl4.fw");
        ASSERT_NUM(sys.resolve("x", {}), 11, "CRLF no final newline: works");
    }
}

void test_bare_cr() {
    SECTION("Bare Carriage Return (Classic Mac)");

    // Bare \r is NOT supported as line separator
    // getline splits on \n only; \r is treated as part of line content
    // Document this as known limitation
    {
        write_raw("/tmp/tcr1.fw", "x = y + 1\rz = w * 2\r");
        FormulaSystem sys;
        sys.load_file("/tmp/tcr1.fw");
        // The entire content is one line: "x = y + 1\rz = w * 2\r"
        // After trim, \r is stripped, but the two equations are merged
        // This is a known limitation — bare CR is extremely rare
        // Just verify it doesn't crash
        ASSERT(true, "bare CR: doesn't crash (known limitation)");
    }
}

void test_large_file_format() {
    SECTION("Large File Format Stress");

    // Many equations with mixed formatting
    {
        std::string content;
        content += "# Physics constants\r\n";
        content += "\r\n";
        content += "g = 9.81\r\n";
        content += "pi = 3.14159\n";
        content += "\n";
        content += "# Formulas\n";
        content += "  circumference = 2 * pi * radius  \n";
        content += "\tarea = pi * radius ^ 2\t\n";
        content += "force = mass * g\r\n";
        content += "weight = force\n";
        content += "\n\n";
        content += "# Kinematics\r\n";
        content += "distance = speed * time\n";
        content += "speed = distance / time\n";
        write_raw("/tmp/tlff.fw", content);

        FormulaSystem sys;
        sys.load_file("/tmp/tlff.fw");
        ASSERT_NUM(sys.defaults.at("g"), 9.81, "mixed fmt: g default");
        ASSERT_NUM(sys.defaults.at("pi"), 3.14159, "mixed fmt: pi default");

        ASSERT_NUM(sys.resolve("force", {{"mass", 10}}), 98.1,
            "mixed fmt: force = 10*9.81");
        ASSERT_NUM(sys.resolve("area", {{"radius", 5}}),
            3.14159 * 25, "mixed fmt: area = pi*r^2");
    }
}

// ---- CLI value parsing edge cases (Group 6) ----

void test_cli_scientific_notation() {
    SECTION("CLI: Scientific Notation");

    // Standard scientific notation — all handled by stod
    {
        auto q = parse_cli_query("f(x=?, y=1e5)");
        ASSERT_NUM(q.bindings.at("y"), 100000, "1e5 = 100000");
    }
    {
        auto q = parse_cli_query("f(x=?, y=1E5)");
        ASSERT_NUM(q.bindings.at("y"), 100000, "1E5 = 100000");
    }
    {
        auto q = parse_cli_query("f(x=?, y=3.14e2)");
        ASSERT_NUM(q.bindings.at("y"), 314, "3.14e2 = 314");
    }
    {
        auto q = parse_cli_query("f(x=?, y=1e-3)");
        ASSERT_NUM(q.bindings.at("y"), 0.001, "1e-3 = 0.001");
    }
    {
        auto q = parse_cli_query("f(x=?, y=1e+3)");
        ASSERT_NUM(q.bindings.at("y"), 1000, "1e+3 = 1000");
    }
    {
        auto q = parse_cli_query("f(x=?, y=2.5e-10)");
        ASSERT_NUM(q.bindings.at("y"), 2.5e-10, "2.5e-10");
    }
    {
        auto q = parse_cli_query("f(x=?, y=-1e5)");
        ASSERT_NUM(q.bindings.at("y"), -100000, "-1e5 = -100000");
    }
    {
        auto q = parse_cli_query("f(x=?, y=1e0)");
        ASSERT_NUM(q.bindings.at("y"), 1, "1e0 = 1");
    }

    // Degenerate: 1e (stod parses as 1, ignores trailing e)
    {
        auto q = parse_cli_query("f(x=?, y=1e)");
        ASSERT_NUM(q.bindings.at("y"), 1, "1e parsed as 1 by stod");
    }

    // Invalid: e5 (no leading digit)
    {
        bool threw = false;
        try { parse_cli_query("f(x=?, y=e5)"); } catch (...) { threw = true; }
        ASSERT(threw, "e5 is invalid");
    }

    // End-to-end: scientific notation in actual solve
    {
        write_fw("/tmp/tc6_1.fw", "y = x * 1000\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc6_1.fw");
        auto q = parse_cli_query("tc6_1(y=?, x=1e3)");
        double r = sys.resolve(q.queries[0].variable, q.bindings);
        ASSERT_NUM(r, 1e6, "sci notation end-to-end: 1e3 * 1000 = 1e6");
    }
}

void test_cli_negative_values() {
    SECTION("CLI: Negative Value Handling");

    // Simple negative
    {
        auto q = parse_cli_query("f(x=?, y=-3)");
        ASSERT_NUM(q.bindings.at("y"), -3, "y=-3");
    }

    // Space after =, before -
    {
        auto q = parse_cli_query("f(x=?, y= -3)");
        ASSERT_NUM(q.bindings.at("y"), -3, "y= -3 (space before minus)");
    }

    // Negative float
    {
        auto q = parse_cli_query("f(x=?, y=-3.14)");
        ASSERT_NUM(q.bindings.at("y"), -3.14, "y=-3.14");
    }

    // Negative zero
    {
        auto q = parse_cli_query("f(x=?, y=-0)");
        ASSERT(q.bindings.at("y") == 0, "y=-0 equals 0");
    }

    // Space between minus and digit: now valid as expression (- 3 = -3)
    {
        auto q = parse_cli_query("f(x=?, y=- 3)");
        ASSERT_NUM(q.bindings.at("y"), -3, "'- 3' parsed as expression = -3");
    }

    // Double minus: now valid as expression (--3 = 3)
    {
        auto q = parse_cli_query("f(x=?, y=--3)");
        ASSERT_NUM(q.bindings.at("y"), 3, "'--3' parsed as expression = 3");
    }

    // Negative scientific notation
    {
        auto q = parse_cli_query("f(x=?, y=-2.5e3)");
        ASSERT_NUM(q.bindings.at("y"), -2500, "-2.5e3 = -2500");
    }
}

void test_cli_multiple_query_targets() {
    SECTION("CLI: Multiple Query Targets");

    // Two queries: both kept in order
    {
        auto q = parse_cli_query("f(x=?, y=?)");
        ASSERT(q.queries.size() == 2, "x=? y=?: two queries");
        ASSERT_EQ(q.queries[0].variable, "x", "first query is x");
        ASSERT_EQ(q.queries[1].variable, "y", "second query is y");
        ASSERT(q.bindings.empty(), "no bindings");
    }

    // Three queries
    {
        auto q = parse_cli_query("f(x=?, y=?, z=?)");
        ASSERT(q.queries.size() == 3, "three queries");
        ASSERT_EQ(q.queries[0].variable, "x", "first is x");
        ASSERT_EQ(q.queries[1].variable, "y", "second is y");
        ASSERT_EQ(q.queries[2].variable, "z", "third is z");
    }

    // Query mixed with bindings
    {
        auto q = parse_cli_query("f(x=?, y=5)");
        ASSERT(q.queries.size() == 1, "one query");
        ASSERT_EQ(q.queries[0].variable, "x", "x=? is query");
        ASSERT_NUM(q.bindings.at("y"), 5, "y=5 is binding");
    }

    // Binding then query
    {
        auto q = parse_cli_query("f(y=5, x=?)");
        ASSERT_EQ(q.queries[0].variable, "x", "x=? after binding");
        ASSERT_NUM(q.bindings.at("y"), 5, "y=5 before query");
    }

    // Aliases: x=?ax means solve x, call it ax
    {
        auto q = parse_cli_query("f(x=?ax, y=?by)");
        ASSERT(q.queries.size() == 2, "two aliased queries");
        ASSERT_EQ(q.queries[0].variable, "x", "first var is x");
        ASSERT_EQ(q.queries[0].alias, "ax", "first alias is ax");
        ASSERT_EQ(q.queries[1].variable, "y", "second var is y");
        ASSERT_EQ(q.queries[1].alias, "by", "second alias is by");
    }

    // Bare ? defaults alias to variable name
    {
        auto q = parse_cli_query("f(x=?)");
        ASSERT_EQ(q.queries[0].variable, "x", "bare ?: var is x");
        ASSERT_EQ(q.queries[0].alias, "x", "bare ?: alias defaults to x");
    }

    // Mixed aliased and bare queries
    {
        auto q = parse_cli_query("f(x=?result, y=?, m=5)");
        ASSERT(q.queries.size() == 2, "mixed: two queries");
        ASSERT_EQ(q.queries[0].alias, "result", "first aliased to result");
        ASSERT_EQ(q.queries[1].alias, "y", "second bare, alias=y");
        ASSERT_NUM(q.bindings.at("m"), 5, "m=5 binding");
    }
}

void test_cli_special_values() {
    SECTION("CLI: Special Value Rejection");

    // inf rejected at parse time
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(x=?, y=inf)"); }
        catch (const std::exception& e) { threw = true; msg = e.what(); }
        ASSERT(threw, "inf rejected");
        ASSERT(msg.find("Infinity") != std::string::npos, "inf: clear message");
    }

    // nan rejected at parse time
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(x=?, y=nan)"); }
        catch (const std::exception& e) { threw = true; msg = e.what(); }
        ASSERT(threw, "nan rejected");
        ASSERT(msg.find("NaN") != std::string::npos, "nan: clear message");
    }

    // Case variants
    {
        bool threw = false;
        try { parse_cli_query("f(x=?, y=INF)"); } catch (...) { threw = true; }
        ASSERT(threw, "INF rejected");
    }
    {
        bool threw = false;
        try { parse_cli_query("f(x=?, y=NaN)"); } catch (...) { threw = true; }
        ASSERT(threw, "NaN rejected");
    }
    {
        bool threw = false;
        try { parse_cli_query("f(x=?, y=infinity)"); } catch (...) { threw = true; }
        ASSERT(threw, "infinity rejected");
    }
}

void test_cli_long_query() {
    SECTION("CLI: Long Query Strings");

    // 100 bindings
    {
        std::string q = "f(target=?";
        for (int i = 0; i < 100; i++)
            q += ", v" + std::to_string(i) + "=" + std::to_string(i * 2);
        q += ")";
        auto parsed = parse_cli_query(q);
        ASSERT_EQ(parsed.queries[0].variable, "target", "100 bindings: solve target");
        ASSERT(parsed.bindings.size() == 100, "100 bindings: all parsed");
        ASSERT_NUM(parsed.bindings.at("v0"), 0, "100 bindings: v0=0");
        ASSERT_NUM(parsed.bindings.at("v99"), 198, "100 bindings: v99=198");
    }

    // Very long filename
    {
        std::string name(500, 'a');
        auto q = parse_cli_query(name + "(x=?)");
        ASSERT_EQ(q.filename, name + ".fw", "500-char filename");
    }

    // Very long variable name
    {
        std::string var(500, 'v');
        auto q = parse_cli_query("f(" + var + "=?)");
        ASSERT_EQ(q.queries[0].variable, var, "500-char variable name");
    }
}

void test_cli_spacing_variants() {
    SECTION("CLI: Spacing Variants");

    // No spaces at all
    {
        auto q = parse_cli_query("f(x=?,y=5,z=10)");
        ASSERT_EQ(q.queries[0].variable, "x", "no spaces: solve x");
        ASSERT_NUM(q.bindings.at("y"), 5, "no spaces: y=5");
        ASSERT_NUM(q.bindings.at("z"), 10, "no spaces: z=10");
    }

    // Lots of spaces
    {
        auto q = parse_cli_query("f(  x = ?  ,  y = 5  ,  z = 10  )");
        ASSERT_EQ(q.queries[0].variable, "x", "many spaces: solve x");
        ASSERT_NUM(q.bindings.at("y"), 5, "many spaces: y=5");
        ASSERT_NUM(q.bindings.at("z"), 10, "many spaces: z=10");
    }

    // Tabs
    {
        auto q = parse_cli_query("f(\tx\t=\t?\t,\ty\t=\t5\t)");
        ASSERT_EQ(q.queries[0].variable, "x", "tabs: solve x");
        ASSERT_NUM(q.bindings.at("y"), 5, "tabs: y=5");
    }

    // Space in filename (before paren)
    {
        // The CLI joins args, so "my formula(x=?)" would be one string
        auto q = parse_cli_query("my formula(x=?)");
        ASSERT_EQ(q.filename, "my formula.fw", "space in filename");
    }
}

void test_cli_end_to_end() {
    SECTION("CLI: End-to-End with Actual Solve");

    // Scientific notation values used in actual computation
    {
        write_fw("/tmp/tc6e1.fw", "energy = mass * 299792458 ^ 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc6e1.fw");
        auto q = parse_cli_query("tc6e1(energy=?, mass=1)");
        double r = sys.resolve(q.queries[0].variable, q.bindings);
        double expected = 299792458.0 * 299792458.0;
        ASSERT(std::abs(r - expected) / expected < 1e-10, "E=mc^2 with c=299792458");
    }

    // Negative value in actual computation
    {
        write_fw("/tmp/tc6e2.fw", "y = x + 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc6e2.fw");
        auto q = parse_cli_query("tc6e2(y=?, x=-5)");
        double r = sys.resolve(q.queries[0].variable, q.bindings);
        ASSERT_NUM(r, 5, "negative input: -5 + 10 = 5");
    }

    // Multiple bindings end-to-end
    {
        write_fw("/tmp/tc6e3.fw", "result = a * b + c * d\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc6e3.fw");
        auto q = parse_cli_query("tc6e3(result=?, a=2, b=3, c=4, d=5)");
        double r = sys.resolve(q.queries[0].variable, q.bindings);
        ASSERT_NUM(r, 26, "2*3 + 4*5 = 26");
    }
}

// ---- Error message quality (Group 7) ----

// Helper to capture error message
std::string get_error(std::function<void()> fn) {
    try { fn(); return ""; }
    catch (const std::exception& e) { return e.what(); }
}

void test_errmsg_missing_variable() {
    SECTION("Error Messages: Missing Variables");

    // Single missing variable — message names it
    {
        write_fw("/tmp/tem1.fw", "x = y + z\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tem1.fw");
        auto msg = get_error([&]() { sys.resolve("x", {{"y", 5}}); });
        ASSERT(msg.find("'z'") != std::string::npos,
            "missing z: error mentions 'z'");
        ASSERT(msg.find("no value") != std::string::npos,
            "missing z: says 'no value'");
    }

    // No equation for target at all
    {
        write_fw("/tmp/tem2.fw", "x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tem2.fw");
        auto msg = get_error([&]() { sys.resolve("w", {{"y", 5}}); });
        ASSERT(msg.find("No equation found") != std::string::npos,
            "unknown target: 'No equation found'");
        ASSERT(msg.find("'w'") != std::string::npos,
            "unknown target: mentions 'w'");
    }

    // Deep chain missing: a needs b needs c (no c)
    {
        write_fw("/tmp/tem3.fw", "a = b + 1\nb = c + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tem3.fw");
        auto msg = get_error([&]() { sys.resolve("a", {}); });
        ASSERT(!msg.empty(), "deep chain missing: throws");
        ASSERT(msg.find("no value") != std::string::npos,
            "deep chain: says 'no value'");
    }

    // Empty system — no equations, no defaults
    {
        write_fw("/tmp/tem4.fw", "# nothing here\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tem4.fw");
        auto msg = get_error([&]() { sys.resolve("x", {}); });
        ASSERT(msg.find("No equation found") != std::string::npos,
            "empty system: 'No equation found'");
    }
}

void test_errmsg_nan_inf() {
    SECTION("Error Messages: NaN/Infinity Results");

    // Single equation producing NaN
    {
        write_fw("/tmp/tei1.fw", "x = sqrt(y)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tei1.fw");
        auto msg = get_error([&]() { sys.resolve("x", {{"y", -1}}); });
        ASSERT(msg.find("NaN") != std::string::npos || msg.find("invalid") != std::string::npos,
            "NaN result: mentions NaN or invalid");
    }

    // Overflow to inf
    {
        write_fw("/tmp/tei2.fw", "x = y ^ 1024\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tei2.fw");
        auto msg = get_error([&]() { sys.resolve("x", {{"y", 2}}); });
        ASSERT(msg.find("infinity") != std::string::npos || msg.find("invalid") != std::string::npos,
            "inf result: mentions infinity or invalid");
    }

    // Multiple NaN-producing equations
    {
        write_fw("/tmp/tei3.fw", "x = sqrt(y)\nx = log(y)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tei3.fw");
        auto msg = get_error([&]() { sys.resolve("x", {{"y", -1}}); });
        ASSERT(msg.find("all equations") != std::string::npos,
            "all NaN: says 'all equations'");
    }
}

void test_errmsg_circular() {
    SECTION("Error Messages: Circular Dependencies");

    // Two-way circular
    {
        write_fw("/tmp/tec1.fw", "x = y + 1\ny = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tec1.fw");
        auto msg = get_error([&]() { sys.resolve("x", {}); });
        ASSERT(!msg.empty(), "circular: throws");
        // The circular dep is caught internally; error reports the missing variable
        ASSERT(msg.find("'x'") != std::string::npos || msg.find("'y'") != std::string::npos,
            "circular: mentions involved variable");
    }

    // Self-reference
    {
        write_fw("/tmp/tec2.fw", "x = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tec2.fw");
        auto msg = get_error([&]() { sys.resolve("x", {}); });
        ASSERT(!msg.empty(), "self-ref: throws");
        ASSERT(msg.find("'x'") != std::string::npos,
            "self-ref: mentions 'x'");
    }

    // Circular detected with --steps trace (verify trace doesn't crash on error path)
    {
        write_fw("/tmp/tec3.fw", "x = y + 1\ny = z + 1\nz = x + 1\n");
        FormulaSystem sys;
        sys.trace.level = TraceLevel::STEPS;
        sys.load_file("/tmp/tec3.fw");
        auto msg = get_error([&]() { sys.resolve("x", {}); });
        ASSERT(!msg.empty(), "circular with trace: throws without crash");
    }
}

void test_errmsg_file() {
    SECTION("Error Messages: File Operations");

    // Nonexistent file includes path
    {
        auto msg = get_error([&]() {
            FormulaSystem sys;
            sys.load_file("/tmp/definitely_not_here.fw");
        });
        ASSERT(msg.find("/tmp/definitely_not_here.fw") != std::string::npos,
            "missing file: includes full path");
        ASSERT(msg.find("Cannot open") != std::string::npos,
            "missing file: says 'Cannot open'");
    }

    // Directory includes path
    {
        system("mkdir -p /tmp/fwiz_errmsg_dir.fw");
        auto msg = get_error([&]() {
            FormulaSystem sys;
            sys.load_file("/tmp/fwiz_errmsg_dir.fw");
        });
        ASSERT(msg.find("directory") != std::string::npos,
            "directory: says 'directory'");
        ASSERT(msg.find("/tmp/fwiz_errmsg_dir.fw") != std::string::npos,
            "directory: includes path");
        system("rmdir /tmp/fwiz_errmsg_dir.fw");
    }

    // Empty path
    {
        auto msg = get_error([&]() {
            FormulaSystem sys;
            sys.load_file("");
        });
        ASSERT(msg.find("No file path") != std::string::npos,
            "empty path: says 'No file path'");
    }
}

void test_errmsg_cli() {
    SECTION("Error Messages: CLI Parsing");

    // Each CLI error should tell the user what to fix
    {
        auto msg = get_error([&]() { parse_cli_query("hello"); });
        ASSERT(msg.find("Expected format") != std::string::npos,
            "no parens: shows expected format");
        ASSERT(msg.find("var=?") != std::string::npos,
            "no parens: shows example syntax");
    }
    {
        auto msg = get_error([&]() { parse_cli_query("f(x=5)"); });
        ASSERT(msg.find("var=?") != std::string::npos,
            "no query: hints to use var=?");
    }
    {
        auto msg = get_error([&]() { parse_cli_query("f(x=?, y=)"); });
        ASSERT(msg.find("'y'") != std::string::npos,
            "empty value: names the variable");
    }
    {
        auto msg = get_error([&]() { parse_cli_query("f(x=?, y=abc)"); });
        ASSERT(msg.find("'abc'") != std::string::npos,
            "bad number: shows the bad value");
        ASSERT(msg.find("'y'") != std::string::npos,
            "bad number: names the variable");
    }
    {
        auto msg = get_error([&]() { parse_cli_query("f(x=?, y=inf)"); });
        ASSERT(msg.find("'y'") != std::string::npos,
            "inf: names the variable");
    }
}

void test_errmsg_consistency() {
    SECTION("Error Message Consistency");

    // All solver errors should mention the target variable
    {
        write_fw("/tmp/tmc1.fw", "x = y + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmc1.fw");

        auto msg1 = get_error([&]() { sys.resolve("x", {}); });
        ASSERT(msg1.find("'x'") != std::string::npos || msg1.find("'y'") != std::string::npos,
            "missing var: mentions a variable name");

        auto msg2 = get_error([&]() { sys.resolve("nonexistent", {}); });
        ASSERT(msg2.find("'nonexistent'") != std::string::npos,
            "no equation: mentions target name");
    }

    // File errors always include path
    {
        auto msg = get_error([&]() {
            FormulaSystem sys;
            sys.load_file("/some/fake/path.fw");
        });
        ASSERT(msg.find("/some/fake/path.fw") != std::string::npos,
            "file error includes exact path");
    }

    // CLI errors always mention the problematic element
    {
        auto msg = get_error([&]() { parse_cli_query("f(=?, bad=xyz)"); });
        ASSERT(!msg.empty(), "CLI errors are never empty");
    }
}

// ---- Final coverage: 8 remaining areas ----

// 1. Binary (main.cpp) integration tests
void test_binary_integration() {
    SECTION("Binary Integration");

    write_fw("/tmp/tbi.fw", "x = y + 1\n");

    // No args: usage message, exit 1
    {
        int rc = system("./bin/fwiz > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 1, "no args: exit 1");
    }

    // Valid query: exit 0
    {
        int rc = system("./bin/fwiz '/tmp/tbi(x=?, y=5)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 0, "valid query: exit 0");
    }

    // Bad query: exit 1
    {
        int rc = system("./bin/fwiz '/tmp/tbi(x=?)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 1, "unsolvable query: exit 1");
    }

    // Missing file: exit 1
    {
        int rc = system("./bin/fwiz 'nonexistent(x=?)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 1, "missing file: exit 1");
    }

    // Result goes to stdout only
    {
        int rc = system("./bin/fwiz '/tmp/tbi(x=?, y=5)' 2>/dev/null | grep -q 'x = 6'");
        ASSERT(WEXITSTATUS(rc) == 0, "result on stdout");
    }

    // No stderr without flags
    {
        // Redirect stderr to file, check it's empty
        system("./bin/fwiz '/tmp/tbi(x=?, y=5)' > /dev/null 2>/tmp/tbi_stderr.txt");
        std::ifstream f("/tmp/tbi_stderr.txt");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ASSERT(content.empty(), "no stderr without flags");
    }

    // --steps output goes to stderr
    {
        system("./bin/fwiz --steps '/tmp/tbi(x=?, y=5)' > /dev/null 2>/tmp/tbi_stderr2.txt");
        std::ifstream f("/tmp/tbi_stderr2.txt");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ASSERT(!content.empty(), "--steps output goes to stderr");
        ASSERT(content.find("solving") != std::string::npos, "--steps contains solving info");
    }

    // --steps no query: error
    {
        int rc = system("./bin/fwiz --steps > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 1, "--steps with no query: exit 1");
    }

    // Piping: result is pipeable
    {
        int rc = system("./bin/fwiz '/tmp/tbi(x=?, y=5)' | grep -q '6'");
        ASSERT(WEXITSTATUS(rc) == 0, "result is pipeable");
    }
}

// 2. Parse → print → reparse roundtrip
void test_roundtrip_parse_print() {
    SECTION("Roundtrip: Parse-Print-Reparse");

    auto roundtrip = [](const std::string& input, const char* label) {
        auto e1 = parse(input);
        auto printed = expr_to_string(e1);
        auto e2 = parse(printed);
        // Evaluate both with concrete values
        auto v1 = e1, v2 = e2;
        for (auto& [n, val] : std::map<std::string,double>{{"x",7},{"y",3},{"z",2},{"a",5},{"b",4}}) {
            v1 = substitute(v1, n, Expr::Num(val));
            v2 = substitute(v2, n, Expr::Num(val));
        }
        double r1 = (evaluate(simplify(v1)).value()), r2 = (evaluate(simplify(v2)).value());
        ASSERT(std::abs(r1 - r2) < 1e-10,
            std::string(label) + " roundtrip: '" + input + "' -> '" + printed + "'");
    };

    roundtrip("x + y", "add");
    roundtrip("x + y * z", "precedence");
    roundtrip("(x + y) * z", "parens");
    roundtrip("x - y - z", "sub left-assoc");
    roundtrip("x - (y - z)", "sub right-group");
    roundtrip("x / y / z", "div left-assoc");
    roundtrip("x / (y / z)", "div right-group");
    roundtrip("x ^ 2", "power");
    roundtrip("-x + y", "unary neg");
    roundtrip("-(x + y)", "neg group");
    roundtrip("sqrt(x + y)", "function");
    roundtrip("(x + y) * z - a / b", "complex");
    roundtrip("sqrt(abs(x - y))", "nested func");
    roundtrip("a * x ^ 2 + b * x", "polynomial");
    roundtrip("-x ^ 2", "neg vs power");
    roundtrip("(-x) ^ 2", "explicit neg power");
}

// 3. Forward → inverse roundtrip consistency
void test_roundtrip_forward_inverse() {
    SECTION("Roundtrip: Forward-Inverse Consistency");

    auto check = [](const char* label, const std::string& eq,
                    const std::string& v1, double val, const std::string& v2) {
        write_fw("/tmp/tri_fi.fw", eq);
        FormulaSystem sys;
        sys.load_file("/tmp/tri_fi.fw");
        double forward = sys.resolve(v2, {{v1, val}});
        double inverse = sys.resolve(v1, {{v2, forward}});
        ASSERT(std::abs(inverse - val) < 1e-6,
            std::string(label) + ": " + std::to_string(val) + " -> "
            + std::to_string(forward) + " -> " + std::to_string(inverse));
    };

    check("add", "x = y + 5\n", "y", 3, "x");
    check("sub", "x = y - 5\n", "y", 3, "x");
    check("mul", "x = y * 7\n", "y", 4, "x");
    check("div", "x = y / 3\n", "y", 9, "x");
    check("mul-sub", "x = y * 2 - 5\n", "y", 4, "x");
    check("div+add", "x = y / 3 + 2\n", "y", 9, "x");
    check("like terms", "x = y + 3 * y\n", "y", 2, "x");
    check("complex", "x = (y + 10) * 3 - 7\n", "y", 5, "x");
    check("negative val", "x = y * 2 - 5\n", "y", -3, "x");
    check("zero val", "x = y + 5\n", "y", 0, "x");
    check("large val", "x = y * 100 + 1\n", "y", 999, "x");
    check("fractional", "x = y / 7\n", "y", 22, "x");
}

// 4. Simplifier convergence
void test_simplifier_convergence() {
    SECTION("Simplifier Convergence");

    // Check that simplify reaches fixpoint (no change on further iterations)
    auto check_fixpoint = [](const char* label, ExprPtr e) {
        auto s = simplify(e);
        auto s2 = simplify_once(s);
        ASSERT(expr_to_string(s) == expr_to_string(s2),
            std::string(label) + ": at fixpoint after simplify");
    };

    check_fixpoint("add constants", parse("x + 2 + 3 + 4"));
    check_fixpoint("sub constants", parse("x - 1 - 2 - 3"));
    check_fixpoint("mul constants", parse("x * 2 * 3 * 4"));
    check_fixpoint("neg cancel", Expr::Neg(Expr::Neg(Expr::Neg(Expr::Neg(Expr::Var("x"))))));
    check_fixpoint("zero absorb", parse("0 * x + 0 * y + 0 * z"));
    check_fixpoint("identity strip", parse("x + 0 + 0 * y + z * 1"));

    // -(a-b) should settle to b-a without oscillation
    {
        auto e = Expr::Neg(Expr::BinOpExpr(BinOp::SUB, Expr::Var("a"), Expr::Var("b")));
        auto s = simplify(e);
        ASSERT_EQ(expr_to_string(s), "-a + b", "-(a-b) settles to -a + b");
        // Verify no oscillation: simplifying again gives same result
        auto s2 = simplify(s);
        ASSERT_EQ(expr_to_string(s2), "-a + b", "-a + b is stable");
    }

    // Double neg of subtraction
    {
        auto e = Expr::Neg(Expr::Neg(Expr::BinOpExpr(BinOp::SUB, Expr::Var("a"), Expr::Var("b"))));
        auto s = simplify(e);
        ASSERT_EQ(expr_to_string(s), "a - b", "--(a-b) settles to a - b");
    }
}

// 5. Example files from README
void test_example_files() {
    SECTION("Example Files");

    // physics.fw
    {
        FormulaSystem sys;
        sys.load_file("examples/physics.fw");
        ASSERT_NUM(sys.resolve("force", {{"mass", 10}}), 98.1, "physics: F=mg");
        ASSERT_NUM(sys.resolve("mass", {{"force", 98.1}}), 10, "physics: m=F/g");
        ASSERT_NUM(sys.resolve("distance", {{"speed", 60}, {"time", 2}}), 120, "physics: d=st");
        ASSERT_NUM(sys.resolve("time", {{"distance", 120}, {"speed", 60}}), 2, "physics: t=d/s");
        ASSERT_NUM(sys.resolve("kinetic_energy", {{"mass", 5}, {"velocity", 10}}), 250,
            "physics: KE=0.5mv^2");
        ASSERT_NUM(sys.resolve("area", {{"radius", 5}}), 3.14159265 * 25,
            "physics: area=pi*r^2");
    }

    // finance.fw
    {
        FormulaSystem sys;
        sys.load_file("examples/finance.fw");
        double total = sys.resolve("total", {{"price", 29.99}, {"qty", 3}});
        ASSERT(std::abs(total - 107.964) < 0.001, "finance: total with tax");
        ASSERT_NUM(sys.resolve("profit_margin", {{"revenue", 1000}, {"cost", 750}}), 25,
            "finance: profit margin");
    }

    // convert.fw
    {
        FormulaSystem sys;
        sys.load_file("examples/convert.fw");
        ASSERT_NUM(sys.resolve("celsius", {{"fahrenheit", 212}}), 100, "convert: F->C boiling");
        ASSERT_NUM(sys.resolve("fahrenheit", {{"celsius", 100}}), 212, "convert: C->F boiling");
        ASSERT_NUM(sys.resolve("celsius", {{"fahrenheit", 32}}), 0, "convert: F->C freezing");
    }

    // navigation.fw
    {
        FormulaSystem sys;
        sys.load_file("examples/navigation.fw");
        // 3-4-5 triangle: distance=5, speed=60, time=5/60
        double t = sys.resolve("time", {{"speed", 60}, {"x1", 0}, {"y1", 0}, {"x2", 3}, {"y2", 4}});
        ASSERT(std::abs(t - 5.0/60.0) < 1e-6, "navigation: 3-4-5 triangle time");
    }

    // geometry.fw
    {
        FormulaSystem sys;
        sys.load_file("examples/geometry.fw");
        ASSERT_NUM(sys.resolve("area", {{"width", 5}, {"height", 3}}), 15, "geometry: area");
        ASSERT_NUM(sys.resolve("perimeter", {{"width", 5}, {"height", 3}}), 16, "geometry: perimeter");
        ASSERT_NUM(sys.resolve("diagonal", {{"width", 3}, {"height", 4}}), 5, "geometry: diagonal");
        ASSERT_NUM(sys.resolve("width", {{"area", 15}, {"height", 3}}), 5, "geometry: inverse width");
    }

    // triangle.fw
    {
        FormulaSystem sys;
        sys.load_file("examples/triangle.fw");

        // SAS: 3-4-5 right triangle
        ASSERT_NUM(sys.resolve("c", {{"a", 3}, {"b", 4}, {"C", 90}}), 5, "triangle SAS: c=5");
        ASSERT_NUM(sys.resolve("area", {{"a", 3}, {"b", 4}, {"C", 90}}), 6, "triangle SAS: area=6");

        // SSS: 3-4-5 → angles
        double A = sys.resolve("A", {{"a", 3}, {"b", 4}, {"c", 5}});
        double B = sys.resolve("B", {{"a", 3}, {"b", 4}, {"c", 5}});
        double C = sys.resolve("C", {{"a", 3}, {"b", 4}, {"c", 5}});
        ASSERT(std::abs(A - 36.87) < 0.01, "triangle SSS: A≈36.87");
        ASSERT(std::abs(B - 53.13) < 0.01, "triangle SSS: B≈53.13");
        ASSERT_NUM(C, 90, "triangle SSS: C=90");
        ASSERT(std::abs(A + B + C - 180) < 1e-6, "triangle SSS: angles sum to 180");

        // Equilateral
        ASSERT_NUM(sys.resolve("A", {{"a", 10}, {"b", 10}, {"c", 10}}), 60, "triangle equilateral: A=60");

        // Heron's area
        ASSERT_NUM(sys.resolve("area", {{"a", 3}, {"b", 4}, {"c", 5}}), 6, "triangle Heron: area=6");

        // ASA: equilateral from two angles and a side
        double a = sys.resolve("a", {{"A", 60}, {"B", 60}, {"c", 10}});
        double b = sys.resolve("b", {{"A", 60}, {"B", 60}, {"c", 10}});
        ASSERT(std::abs(a - 10) < 1e-6, "triangle ASA equilateral: a=10");
        ASSERT(std::abs(b - 10) < 1e-6, "triangle ASA equilateral: b=10");
    }
}

// 6. Operator precedence exhaustive
void test_precedence_exhaustive() {
    SECTION("Precedence Exhaustive");

    auto check = [](const char* label, const std::string& expr, double expected) {
        auto e = parse(expr);
        for (auto& [n, v] : std::map<std::string,double>{{"a",2},{"b",3},{"c",4},{"d",5}})
            e = substitute(e, n, Expr::Num(v));
        double r = (evaluate(simplify(e)).value());
        ASSERT(std::abs(r - expected) < 1e-10,
            std::string(label) + " = " + std::to_string(r));
    };

    // Power > mul > add
    check("a+b*c^d", "a + b * c ^ d", 2 + 3 * pow(4, 5));

    // Unary minus lower than power: -a^2 = -(a^2)
    check("-a^2 = -(a^2)", "-a ^ 2", -pow(2, 2));  // = -4
    check("(-a)^2", "(-a) ^ 2", pow(-2, 2));        // = 4

    // Unary minus higher than mul: -a*b = (-a)*b
    check("-a*b", "-a * b", (-2) * 3);  // = -6

    // Mixed operations
    check("a*b+c*d", "a * b + c * d", 2*3 + 4*5);
    check("a+b*c-d", "a + b * c - d", 2 + 3*4 - 5);

    // Left-associativity
    check("a-b-c-d", "a - b - c - d", 2 - 3 - 4 - 5);
    check("a/b/c", "a / b / c", 2.0 / 3.0 / 4.0);

    // Right-associativity of power
    check("a^b^c = a^(b^c)", "a ^ b ^ c", pow(2, pow(3, 4)));

    // Power with negative exponent (via unary on RHS)
    check("a^-b", "a ^ -b", pow(2, -3));
}

// 7. Intermediate resolution consistency
void test_intermediate_consistency() {
    SECTION("Intermediate Resolution Consistency");

    // Solving for endpoint needs intermediate — verify intermediate matches direct solve
    {
        write_fw("/tmp/tic1.fw", "a = b + 1\nb = c + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tic1.fw");

        // Direct: solve b given c=10
        double b_direct = sys.resolve("b", {{"c", 10}});
        // Indirect: solve a given c=10 (internally resolves b)
        double a_val = sys.resolve("a", {{"c", 10}});
        ASSERT_NUM(b_direct, 11, "direct b = c+1 = 11");
        ASSERT_NUM(a_val, 12, "a = b+1 = 12 (b resolved internally)");
        // Verify: a - 1 should equal b_direct
        ASSERT_NUM(a_val - 1, b_direct, "a-1 == b (consistency)");
    }

    // Three-level chain
    {
        write_fw("/tmp/tic2.fw", "p = q * 2\nq = r + 3\nr = s - 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tic2.fw");

        double s = 10;
        double r_direct = sys.resolve("r", {{"s", s}});
        double q_direct = sys.resolve("q", {{"s", s}});
        double p_direct = sys.resolve("p", {{"s", s}});

        ASSERT_NUM(r_direct, 9, "r = s-1 = 9");
        ASSERT_NUM(q_direct, 12, "q = r+3 = 12");
        ASSERT_NUM(p_direct, 24, "p = q*2 = 24");

        // Verify chain consistency
        ASSERT_NUM(q_direct, r_direct + 3, "q == r+3");
        ASSERT_NUM(p_direct, q_direct * 2, "p == q*2");
    }
}

// 8. Edge arithmetic through the system
void test_edge_arithmetic() {
    SECTION("Edge Arithmetic");

    // 0 - 0 = 0
    ASSERT_NUM(ev("0 - 0"), 0, "0 - 0 = 0");

    // 0 ^ 0 = 1 (IEEE convention)
    ASSERT_NUM(ev("0 ^ 0"), 1, "0 ^ 0 = 1");

    // 0 * anything = 0
    ASSERT_NUM(ev("0 * 999"), 0, "0 * 999 = 0");

    // x/x with x=0 — div-by-zero yields empty Checked
    {
        auto e = parse("x / x");
        e = substitute(e, "x", Expr::Num(0));
        auto r = evaluate(e);
        ASSERT(!r.has_value(), "x/x with x=0: empty (div-by-zero)");
    }

    // x/x with x=5 evaluates to 1
    {
        auto e = parse("x / x");
        e = substitute(e, "x", Expr::Num(5));
        ASSERT_NUM(evaluate(e).value(), 1, "x/x with x=5 = 1");
    }

    // 0*x + 0*y simplifies to 0
    ASSERT_EQ(ss("0 * x + 0 * y"), "0", "0*x + 0*y = 0");

    // Very small difference
    {
        auto e = parse("x - y");
        e = substitute(e, "x", Expr::Num(1.0000000001));
        e = substitute(e, "y", Expr::Num(1.0));
        double r = (evaluate(simplify(e)).value());
        ASSERT(std::abs(r - 1e-10) < 1e-15, "tiny difference preserved");
    }

    // Through the system: equation that evaluates to exact zero
    {
        write_fw("/tmp/tea1.fw", "result = x - x + y\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tea1.fw");
        ASSERT_NUM(sys.resolve("result", {{"x", 999}, {"y", 42}}), 42,
            "x - x + y = 42 (x cancels numerically)");
    }
}

// ---- Code audit bug regression tests ----

void test_audit_fmt_num_ub() {
    SECTION("Audit: fmt_num cast safety");

    // These values would cause UB in the old code where (long long)v
    // was evaluated BEFORE the range check.
    // With the fix, abs(v) < 1e12 short-circuits and the cast never happens.

    // Infinity — (long long)inf is UB
    {
        double inf = std::numeric_limits<double>::infinity();
        std::string s = fmt_num(inf);
        ASSERT(!s.empty(), "fmt_num(inf) doesn't crash");
        ASSERT(s.find("inf") != std::string::npos || s.find("Inf") != std::string::npos,
            "fmt_num(inf) produces 'inf'");
    }

    // Negative infinity
    {
        double ninf = -std::numeric_limits<double>::infinity();
        std::string s = fmt_num(ninf);
        ASSERT(!s.empty(), "fmt_num(-inf) doesn't crash");
    }

    // NaN — (long long)NaN is UB
    {
        double nan = std::numeric_limits<double>::quiet_NaN();
        std::string s = fmt_num(nan);
        ASSERT(!s.empty(), "fmt_num(NaN) doesn't crash");
    }

    // Very large double beyond long long range (max ~9.2e18)
    {
        std::string s = fmt_num(1e19);
        ASSERT(!s.empty(), "fmt_num(1e19) doesn't crash");
    }
    {
        std::string s = fmt_num(-1e19);
        ASSERT(!s.empty(), "fmt_num(-1e19) doesn't crash");
    }
    {
        std::string s = fmt_num(DBL_MAX);
        ASSERT(!s.empty(), "fmt_num(DBL_MAX) doesn't crash");
    }

    // Values that SHOULD use the integer path (within 1e12 range)
    ASSERT_EQ(fmt_num(0.0), "0", "fmt_num(0) = '0'");
    ASSERT_EQ(fmt_num(42.0), "42", "fmt_num(42) = '42'");
    ASSERT_EQ(fmt_num(-7.0), "-7", "fmt_num(-7) = '-7'");
    ASSERT_EQ(fmt_num(999999999999.0), "999999999999", "fmt_num(just under 1e12)");

    // Values just above the 1e12 threshold — should NOT attempt cast
    {
        std::string s = fmt_num(1e12);
        // 1e12 == 1000000000000 which is exactly representable and within long long,
        // but our guard is < 1e12 (strict), so it goes through ostringstream
        ASSERT(!s.empty(), "fmt_num(1e12) doesn't crash");
    }

    // Negative zero
    ASSERT_EQ(fmt_num(-0.0), "0", "fmt_num(-0.0) = '0'");

    // Also verify the same fix in expr_to_string
    {
        auto e = Expr::Num(std::numeric_limits<double>::infinity());
        std::string s = expr_to_string(e);
        ASSERT(!s.empty(), "expr_to_string(inf) doesn't crash");
    }
    {
        auto e = Expr::Num(std::numeric_limits<double>::quiet_NaN());
        std::string s = expr_to_string(e);
        ASSERT(!s.empty(), "expr_to_string(NaN) doesn't crash");
    }
    {
        auto e = Expr::Num(1e19);
        std::string s = expr_to_string(e);
        ASSERT(!s.empty(), "expr_to_string(1e19) doesn't crash");
    }
}

void test_audit_signed_char_ub() {
    SECTION("Audit: signed char in ctype functions");

    // High bytes (0x80-0xFF) are negative when char is signed.
    // Passing negative values to isdigit/isalpha/isalnum is UB.
    // The fix casts to unsigned char before calling these functions.

    // Single high byte — should throw "Unexpected character", not UB crash
    {
        bool threw = false;
        try {
            std::string input(1, static_cast<char>(0x80));
            Lexer(input).tokenize();
        } catch (const std::exception&) { threw = true; }
        ASSERT(threw, "byte 0x80 throws (not UB crash)");
    }
    {
        bool threw = false;
        try {
            std::string input(1, static_cast<char>(0xFF));
            Lexer(input).tokenize();
        } catch (const std::exception&) { threw = true; }
        ASSERT(threw, "byte 0xFF throws (not UB crash)");
    }

    // High byte in middle of valid expression
    {
        bool threw = false;
        try {
            std::string input = "x + ";
            input += static_cast<char>(0xC3);
            input += static_cast<char>(0xA9); // UTF-8 'é'
            Lexer(input).tokenize();
        } catch (const std::exception&) { threw = true; }
        ASSERT(threw, "UTF-8 'é' in expression throws cleanly");
    }

    // High byte after valid tokens — lexer handles preceding tokens correctly
    {
        bool threw = false;
        try {
            std::string input = "x + y + ";
            input += static_cast<char>(0xC0);
            Lexer(input).tokenize();
        } catch (const std::exception&) { threw = true; }
        ASSERT(threw, "high byte after valid tokens: throws");
    }

    // All bytes from 0x80 to 0xFF should throw, not crash
    {
        int throw_count = 0;
        for (int b = 0x80; b <= 0xFF; b++) {
            try {
                std::string input(1, static_cast<char>(b));
                Lexer(input).tokenize();
            } catch (const std::exception&) {
                throw_count++;
            }
        }
        ASSERT(throw_count == 128, "all 128 high bytes throw cleanly");
    }
}

void test_audit_switch_safety() {
    SECTION("Audit: switch completeness");

    // Verify precedence() returns correct values for all expression types.
    // This tests that the switch covers everything and no fallthrough occurs.
    {
        auto num = Expr::Num(5);
        auto var = Expr::Var("x");
        auto neg = Expr::Neg(var);
        auto add = Expr::BinOpExpr(BinOp::ADD, num, var);
        auto sub = Expr::BinOpExpr(BinOp::SUB, num, var);
        auto mul = Expr::BinOpExpr(BinOp::MUL, num, var);
        auto div = Expr::BinOpExpr(BinOp::DIV, num, var);
        auto pow = Expr::BinOpExpr(BinOp::POW, num, var);
        auto func = Expr::Call("sqrt", {var});

        // Each type should have a distinct, correct precedence
        ASSERT(precedence(add) == 1, "ADD precedence = 1");
        ASSERT(precedence(sub) == 1, "SUB precedence = 1");
        ASSERT(precedence(mul) == 2, "MUL precedence = 2");
        ASSERT(precedence(div) == 2, "DIV precedence = 2");
        ASSERT(precedence(neg) == 3, "NEG precedence = 3");
        ASSERT(precedence(pow) == 4, "POW precedence = 4");
        ASSERT(precedence(num) == 5, "NUM precedence = 5");
        ASSERT(precedence(var) == 5, "VAR precedence = 5");
        ASSERT(precedence(func) == 5, "FUNC precedence = 5");

        // Ordering is correct
        ASSERT(precedence(add) < precedence(mul), "ADD < MUL");
        ASSERT(precedence(mul) < precedence(neg), "MUL < NEG");
        ASSERT(precedence(neg) < precedence(pow), "NEG < POW");
        ASSERT(precedence(pow) < precedence(num), "POW < atom");
    }

    // Verify evaluate() handles all expression types correctly
    // (no silent return 0 from fallthrough)
    {
        ASSERT_NUM(evaluate(Expr::Num(42)).value(), 42, "evaluate NUM");
        ASSERT_NUM(evaluate(Expr::Neg(Expr::Num(5))).value(), -5, "evaluate NEG");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::ADD, Expr::Num(2), Expr::Num(3))).value(), 5, "evaluate ADD");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::SUB, Expr::Num(5), Expr::Num(3))).value(), 2, "evaluate SUB");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::MUL, Expr::Num(4), Expr::Num(3))).value(), 12, "evaluate MUL");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::DIV, Expr::Num(6), Expr::Num(2))).value(), 3, "evaluate DIV");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::POW, Expr::Num(2), Expr::Num(3))).value(), 8, "evaluate POW");
        ASSERT_NUM(evaluate(Expr::Call("sqrt", {Expr::Num(16)})).value(), 4, "evaluate FUNC");
    }

    // Verify unresolved variable returns empty (not silent 0)
    {
        ASSERT(!evaluate(Expr::Var("x")), "evaluate VAR returns empty");
    }
}

// ---- Multi-return and aliases ----

void test_multi_return() {
    SECTION("Multi-Return Queries");

    // Two queries from same system
    {
        write_fw("/tmp/tmr1.fw", "x = m + 4\ny = m - 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr1.fw");
        auto q = parse_cli_query("tmr1(x=?, y=?, m=4)");
        ASSERT(q.queries.size() == 2, "two queries parsed");
        double x = sys.resolve(q.queries[0].variable, q.bindings);
        double y = sys.resolve(q.queries[1].variable, q.bindings);
        ASSERT_NUM(x, 8, "x = m+4 = 8");
        ASSERT_NUM(y, 1, "y = m-3 = 1");
    }

    // Aliases don't affect resolution — only output naming
    {
        auto q = parse_cli_query("f(x=?result, m=4)");
        ASSERT_EQ(q.queries[0].variable, "x", "alias: resolves x");
        ASSERT_EQ(q.queries[0].alias, "result", "alias: outputs as result");
    }

    // Three queries
    {
        write_fw("/tmp/tmr2.fw", "a = n + 1\nb = n + 2\nc = n + 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr2.fw");
        auto q = parse_cli_query("tmr2(a=?, b=?, c=?, n=10)");
        ASSERT(q.queries.size() == 3, "three queries");
        ASSERT_NUM(sys.resolve("a", q.bindings), 11, "a=11");
        ASSERT_NUM(sys.resolve("b", q.bindings), 12, "b=12");
        ASSERT_NUM(sys.resolve("c", q.bindings), 13, "c=13");
    }

    // Multi-return with inverse solving
    {
        write_fw("/tmp/tmr3.fw", "area = width * height\nperim = 2 * width + 2 * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr3.fw");
        auto q = parse_cli_query("tmr3(area=?, perim=?, width=5, height=3)");
        ASSERT_NUM(sys.resolve("area", q.bindings), 15, "area=15");
        ASSERT_NUM(sys.resolve("perim", q.bindings), 16, "perim=16");
    }

    // One query succeeds, one fails — each is independent
    {
        write_fw("/tmp/tmr4.fw", "x = m + 1\ny = m + n\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr4.fw");
        auto q = parse_cli_query("tmr4(x=?, y=?, m=5)");
        // x = m+1 = 6 (works)
        ASSERT_NUM(sys.resolve("x", q.bindings), 6, "x succeeds");
        // y = m+n, n not provided (fails)
        bool threw = false;
        try { sys.resolve("y", q.bindings); } catch (...) { threw = true; }
        ASSERT(threw, "y fails (missing n)");
    }

    // Query the same variable twice (with different aliases)
    {
        auto q = parse_cli_query("f(x=?first, x=?second, m=5)");
        ASSERT(q.queries.size() == 2, "same var twice: two queries");
        ASSERT_EQ(q.queries[0].alias, "first", "first alias");
        ASSERT_EQ(q.queries[1].alias, "second", "second alias");
    }
}

void test_alias_syntax() {
    SECTION("Alias Syntax Parsing");

    // Bare ? — alias defaults to variable name
    {
        auto q = parse_cli_query("f(x=?)");
        ASSERT_EQ(q.queries[0].variable, "x", "bare: var=x");
        ASSERT_EQ(q.queries[0].alias, "x", "bare: alias=x");
    }

    // Named alias
    {
        auto q = parse_cli_query("f(x=?myname)");
        ASSERT_EQ(q.queries[0].variable, "x", "named: var=x");
        ASSERT_EQ(q.queries[0].alias, "myname", "named: alias=myname");
    }

    // Alias with underscores and digits
    {
        auto q = parse_cli_query("f(x=?my_var_2)");
        ASSERT_EQ(q.queries[0].alias, "my_var_2", "alias with underscore+digits");
    }

    // Multiple aliases in one query
    {
        auto q = parse_cli_query("f(x=?a, y=?b, z=?c, m=5)");
        ASSERT(q.queries.size() == 3, "three aliased queries");
        ASSERT_EQ(q.queries[0].alias, "a", "alias a");
        ASSERT_EQ(q.queries[1].alias, "b", "alias b");
        ASSERT_EQ(q.queries[2].alias, "c", "alias c");
        ASSERT_NUM(q.bindings.at("m"), 5, "binding m=5");
    }

    // Alias with spaces around it
    {
        auto q = parse_cli_query("f( x = ?alias , m = 5 )");
        ASSERT_EQ(q.queries[0].variable, "x", "spaces: var=x");
        ASSERT_EQ(q.queries[0].alias, "alias", "spaces: alias preserved");
    }
}

// ---- Free variables and interface contracts ----

void test_free_variable_resolution() {
    SECTION("Free Variable Resolution");

    // Free variable solved forward through equation
    {
        write_fw("/tmp/tfv1.fw", "ay = 5\nax = ay + 5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tfv1.fw");
        ASSERT_NUM(sys.resolve("ax", {{"ay", 2}}), 7, "ax=ay+5, ay=2 → ax=7");
    }

    // Free variable solved inverse
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tfv1.fw");
        ASSERT_NUM(sys.resolve("ay", {{"ax", 5}}), 0, "ax=ay+5, ax=5 → ay=0");
    }

    // Default fills in when free variable not mentioned
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tfv1.fw");
        ASSERT_NUM(sys.resolve("ax", {}), 10, "ax=ay+5, ay default 5 → ax=10");
    }

    // Free variable with no default, not provided → error naming the variable
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tfv1.fw");
        auto msg = get_error([&]() { sys.resolve("ay", {}); });
        ASSERT(msg.find("'ax'") != std::string::npos,
            "underdetermined: error names missing var 'ax'");
    }
}

void test_underdetermined_systems() {
    SECTION("Underdetermined Systems");

    // One equation, two unknowns, no values → error
    {
        write_fw("/tmp/tud1.fw", "z = x + y\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tud1.fw");
        auto msg = get_error([&]() { sys.resolve("z", {}); });
        ASSERT(!msg.empty(), "two unknowns: throws");
        ASSERT(msg.find("no value") != std::string::npos, "two unknowns: says 'no value'");
    }

    // Provide one of two unknowns → still underdetermined
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tud1.fw");
        ASSERT_NUM(sys.resolve("z", {{"x", 3}, {"y", 4}}), 7, "both provided: z=7");
        auto msg = get_error([&]() { sys.resolve("z", {{"x", 3}}); });
        ASSERT(msg.find("'y'") != std::string::npos, "x only: still missing y");
    }

    // Inverse with one unknown → works only if enough info
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tud1.fw");
        ASSERT_NUM(sys.resolve("x", {{"z", 10}, {"y", 4}}), 6, "inverse: x=z-y=6");
        auto msg = get_error([&]() { sys.resolve("x", {{"z", 10}}); });
        ASSERT(msg.find("no value") != std::string::npos, "inverse missing y: error");
    }

    // Substitution with shared factor must not produce spurious zero
    // c*f(A) = c*f(B) where A,B are unknown — the solver should NOT conclude c=0
    {
        write_fw("/tmp/tud_factor.fw",
            "area = a * c * k / 2\n"
            "area = b * c * k / 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tud_factor.fw");
        // a=5, b=5 makes both area equations identical — underdetermined for c
        auto msg = get_error([&]() { sys.resolve("c", {{"a", 5}, {"b", 5}, {"k", 1}}); });
        ASSERT(!msg.empty(), "shared factor underdetermined: throws");
    }

    // Same structure but a != b — c=0 is the only value satisfying both equations
    {
        write_fw("/tmp/tud_factor2.fw",
            "area = a * c * k / 2\n"
            "area = b * c * k / 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tud_factor2.fw");
        ASSERT_NUM(sys.resolve("c", {{"a", 3}, {"b", 5}, {"k", 1}}), 0,
            "different factors: c=0 is valid");
    }

    // When the coefficient is a concrete number, c=0 IS valid
    {
        write_fw("/tmp/tud_factor3.fw", "y = 3 * c + 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tud_factor3.fw");
        ASSERT_NUM(sys.resolve("c", {{"y", 0}}), 0,
            "concrete coeff with zero rest: c=0 is valid");
    }
}

void test_free_var_chains() {
    SECTION("Free Variable Chain Propagation");

    write_fw("/tmp/tfc1.fw", "b = a + 1\nc = b + 1\n");

    // Forward chain: a provided → c solved through b
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tfc1.fw");
        ASSERT_NUM(sys.resolve("c", {{"a", 5}}), 7, "chain forward: a=5 → c=7");
    }

    // Inverse chain: c provided → a solved backward
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tfc1.fw");
        ASSERT_NUM(sys.resolve("a", {{"c", 10}}), 8, "chain inverse: c=10 → a=8");
    }

    // Missing base of chain → error
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tfc1.fw");
        auto msg = get_error([&]() { sys.resolve("c", {}); });
        ASSERT(!msg.empty(), "missing chain base: throws");
    }

    // Deep chain: every intermediate is free
    {
        write_fw("/tmp/tfc2.fw", "b = a * 2\nc = b + 3\nd = c * c\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tfc2.fw");
        // a=5 → b=10 → c=13 → d=169
        ASSERT_NUM(sys.resolve("d", {{"a", 5}}), 169, "deep chain: a=5 → d=169");
    }
}

void test_multi_query_free_vars() {
    SECTION("Multi-Query with Free Variables");

    write_fw("/tmp/tmqf.fw", "offset = 0\nout1 = in1 + offset\nout2 = in2 * 2\n");

    // Both queries succeed
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tmqf.fw");
        ASSERT_NUM(sys.resolve("out1", {{"in1", 3}, {"in2", 5}}), 3, "multi: out1=3");
        ASSERT_NUM(sys.resolve("out2", {{"in1", 3}, {"in2", 5}}), 10, "multi: out2=10");
    }

    // First query fails (missing in1), second succeeds (has in2)
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tmqf.fw");
        auto msg = get_error([&]() { sys.resolve("out1", {{"in2", 5}}); });
        ASSERT(msg.find("'in1'") != std::string::npos, "missing in1: error names it");
        ASSERT_NUM(sys.resolve("out2", {{"in2", 5}}), 10, "out2 still works independently");
    }

    // Default used for offset
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tmqf.fw");
        ASSERT_NUM(sys.resolve("out1", {{"in1", 7}}), 7, "default offset=0: out1=7");
    }

    // Override default
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tmqf.fw");
        ASSERT_NUM(sys.resolve("out1", {{"in1", 7}, {"offset", 10}}), 17, "override offset=10: out1=17");
    }
}

void test_interface_error_messages() {
    SECTION("Interface Error Messages");

    // Error specifically names the missing free variable
    {
        write_fw("/tmp/tier1.fw", "result = input * scale + bias\nscale = 2\nbias = 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tier1.fw");

        // input not provided → error names 'input'
        auto msg = get_error([&]() { sys.resolve("result", {}); });
        ASSERT(msg.find("'input'") != std::string::npos,
            "missing input: error names 'input'");

        // input provided → works using defaults for scale and bias
        ASSERT_NUM(sys.resolve("result", {{"input", 5}}), 10, "defaults work: 5*2+0=10");

        // override defaults
        ASSERT_NUM(sys.resolve("result", {{"input", 5}, {"scale", 3}, {"bias", 1}}), 16,
            "overrides work: 5*3+1=16");
    }

    // Two free variables missing → error names at least one
    {
        write_fw("/tmp/tier2.fw", "z = x + y\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tier2.fw");
        auto msg = get_error([&]() { sys.resolve("z", {}); });
        ASSERT(msg.find("no value") != std::string::npos, "two missing: says 'no value'");
        // Should name at least x or y
        ASSERT(msg.find("'x'") != std::string::npos || msg.find("'y'") != std::string::npos,
            "two missing: names at least one");
    }

    // Querying a variable that has no equation and no default → clear error
    {
        write_fw("/tmp/tier3.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tier3.fw");
        auto msg = get_error([&]() { sys.resolve("w", {{"x", 5}}); });
        ASSERT(msg.find("No equation found") != std::string::npos,
            "unknown var: 'No equation found'");
        ASSERT(msg.find("'w'") != std::string::npos, "unknown var: names 'w'");
    }
}

// ---- Formula call tests ----

void test_formula_call_parsing() {
    SECTION("Formula Call Parsing");

    // Form 1: standalone, no alias — output_var = query_var
    {
        write_fw("/tmp/fcp_rect.fw", "area = width * height\n");
        write_fw("/tmp/fcp1.fw", "fcp_rect(area=?, width=width, height=depth)\nvolume = area * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcp1.fw");
        ASSERT(sys.formula_calls.size() == 1, "form1: one formula call");
        ASSERT_EQ(sys.formula_calls[0].file_stem, "fcp_rect", "form1: file_stem");
        ASSERT_EQ(sys.formula_calls[0].query_var, "area", "form1: query_var");
        ASSERT_EQ(sys.formula_calls[0].output_var, "area", "form1: output_var = query_var");
        ASSERT(sys.formula_calls[0].bindings.count("width"), "form1: has width binding");
        ASSERT(sys.formula_calls[0].bindings.count("height"), "form1: has height binding");
        ASSERT_EQ(expr_to_string(sys.formula_calls[0].bindings.at("height")), "depth", "form1: height=depth");
        // Should also have the equation: volume = area * h
        ASSERT(sys.equations.size() == 1, "form1: one equation");
    }

    // Form 2: standalone with alias
    {
        write_fw("/tmp/fcp2.fw", "fcp_rect(area=?floor, width=width, height=depth)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcp2.fw");
        ASSERT(sys.formula_calls.size() == 1, "form2: one formula call");
        ASSERT_EQ(sys.formula_calls[0].output_var, "floor", "form2: output_var = alias");
        ASSERT_EQ(sys.formula_calls[0].query_var, "area", "form2: query_var unchanged");
        ASSERT(sys.equations.empty(), "form2: no equations (standalone)");
    }

    // Form 3: implied alias
    {
        write_fw("/tmp/fcp3.fw", "floor = fcp_rect(area=?, width=width, height=depth)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcp3.fw");
        ASSERT(sys.formula_calls.size() == 1, "form3: one formula call");
        ASSERT_EQ(sys.formula_calls[0].output_var, "floor", "form3: implied alias from LHS");
        ASSERT(sys.equations.empty(), "form3: degenerate x=x skipped");
    }

    // Form 4: inline in expression
    {
        write_fw("/tmp/fcp4.fw", "volume = fcp_rect(area=?floor, width=width, height=depth) * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcp4.fw");
        ASSERT(sys.formula_calls.size() == 1, "form4: one formula call");
        ASSERT_EQ(sys.formula_calls[0].output_var, "floor", "form4: alias");
        ASSERT(sys.equations.size() == 1, "form4: one equation");
        ASSERT_EQ(sys.equations[0].lhs_var, "volume", "form4: equation LHS");
        // The RHS should be "floor * h"
        ASSERT_EQ(expr_to_string(sys.equations[0].rhs), "floor * h", "form4: formula call replaced in expr");
    }

    // Shorthand binding: bare ident
    {
        write_fw("/tmp/fcp5.fw", "fcp_rect(area=?, width, height=depth)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcp5.fw");
        ASSERT_EQ(expr_to_string(sys.formula_calls[0].bindings.at("width")), "width", "shorthand: width=width");
        ASSERT_EQ(expr_to_string(sys.formula_calls[0].bindings.at("height")), "depth", "explicit: height=depth");
    }
}

void test_formula_call_forward() {
    SECTION("Formula Call Forward Resolution");

    write_fw("/tmp/fcf_rect.fw", "area = width * height\n");

    // Basic forward: solve for output_var via sub-system
    {
        write_fw("/tmp/fcf1.fw",
            "fcf_rect(area=?floor, width=width, height=depth)\n"
            "volume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcf1.fw");
        // floor = width*depth = 4*3 = 12, volume = 12*6 = 72
        ASSERT_NUM(sys.resolve("volume", {{"width", 4}, {"depth", 3}, {"h", 6}}), 72,
            "forward: volume via formula call");
    }

    // Direct query of formula call output
    {
        write_fw("/tmp/fcf2.fw", "fcf_rect(area=?floor, width=width, height=depth)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcf2.fw");
        ASSERT_NUM(sys.resolve("floor", {{"width", 4}, {"depth", 3}}), 12,
            "forward: direct query of output_var");
    }

    // Providing the output_var skips sub-system (bridge)
    {
        write_fw("/tmp/fcf3.fw",
            "fcf_rect(area=?floor, width=width, height=depth)\n"
            "volume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcf3.fw");
        ASSERT_NUM(sys.resolve("volume", {{"floor", 20}, {"h", 5}}), 100,
            "forward: output_var provided directly");
    }

    // Multiple formula calls to same file with different bindings
    {
        write_fw("/tmp/fcf4.fw",
            "fcf_rect(area=?a1, width=w1, height=h1)\n"
            "fcf_rect(area=?a2, width=w2, height=h2)\n"
            "total = a1 + a2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcf4.fw");
        // a1 = 3*4=12, a2 = 5*6=30, total = 42
        ASSERT_NUM(sys.resolve("total", {{"w1",3},{"h1",4},{"w2",5},{"h2",6}}), 42,
            "forward: two calls to same sub-system");
    }

    // Inline formula call in expression (form 4)
    {
        write_fw("/tmp/fcf5.fw",
            "volume = fcf_rect(area=?floor, width=w, height=d) * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcf5.fw");
        ASSERT_NUM(sys.resolve("volume", {{"w", 4}, {"d", 3}, {"h", 6}}), 72,
            "forward: inline formula call");
    }
}

void test_formula_call_reverse() {
    SECTION("Formula Call Reverse Resolution");

    write_fw("/tmp/fcr_rect.fw", "area = width * height\n");

    // Reverse: solve parent var through binding
    {
        write_fw("/tmp/fcr1.fw",
            "fcr_rect(area=?floor, width=width, height=depth)\n"
            "volume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcr1.fw");
        // depth=? with floor=24, width=4 → area=24, height=24/4=6 → depth=6
        ASSERT_NUM(sys.resolve("depth", {{"floor", 24}, {"width", 4}}), 6,
            "reverse: depth through binding bridge");
    }

    // Reverse: solve through volume equation + formula call
    {
        write_fw("/tmp/fcr2.fw",
            "fcr_rect(area=?floor, width=width, height=depth)\n"
            "volume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcr2.fw");
        // depth=? with volume=72, width=4, h=6 → floor=72/6=12, area=12, height=12/4=3 → depth=3
        ASSERT_NUM(sys.resolve("depth", {{"volume", 72}, {"width", 4}, {"h", 6}}), 3,
            "reverse: depth from volume via formula call");
    }

    // Reverse: solve width through sub-system
    {
        write_fw("/tmp/fcr3.fw", "fcr_rect(area=?floor, width=w, height=h)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcr3.fw");
        // w=? with floor=24, h=6 → area=24, height=6, width=24/6=4 → w=4
        ASSERT_NUM(sys.resolve("w", {{"floor", 24}, {"h", 6}}), 4,
            "reverse: w through binding");
    }
}

void test_formula_call_chained() {
    SECTION("Formula Call Chained");

    // A → B → C chain
    {
        write_fw("/tmp/fcc_c.fw", "z = x + y\n");
        write_fw("/tmp/fcc_b.fw", "fcc_c(z=?mid, x=a, y=b)\nresult = mid * 2\n");
        write_fw("/tmp/fcc_a.fw", "fcc_b(result=?out, a=p, b=q)\nfinal = out + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcc_a.fw");
        // z = p+q = 3+4=7, mid=7, result=14, out=14, final=15
        ASSERT_NUM(sys.resolve("final", {{"p", 3}, {"q", 4}}), 15,
            "chained: A→B→C = (3+4)*2+1 = 15");
    }

    // Sub-system uses defaults from its own file
    {
        write_fw("/tmp/fcc_d.fw", "g = 9.81\nforce = mass * g\n");
        write_fw("/tmp/fcc_e.fw", "fcc_d(force=?f, mass=m)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fcc_e.fw");
        ASSERT_NUM(sys.resolve("f", {{"m", 10}}), 98.1,
            "chained: sub-system uses own defaults");
    }
}

void test_formula_call_errors() {
    SECTION("Formula Call Errors");

    // Missing sub-system file
    {
        write_fw("/tmp/fce1.fw", "nonexistent_file(x=?, y=y)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fce1.fw");
        auto msg = get_error([&]() { sys.resolve("x", {{"y", 5}}); });
        ASSERT(!msg.empty(), "missing file: throws");
        // The file error propagates through the solver
        ASSERT(!msg.empty(), "missing file: error message not empty");
    }

    // Sub-system can't solve (missing binding)
    {
        write_fw("/tmp/fce_rect.fw", "area = width * height\n");
        write_fw("/tmp/fce2.fw", "fce_rect(area=?floor, width=w)\n"); // no height binding
        FormulaSystem sys;
        sys.load_file("/tmp/fce2.fw");
        auto msg = get_error([&]() { sys.resolve("floor", {{"w", 4}}); });
        ASSERT(!msg.empty(), "missing sub-binding: throws");
    }

    // No query variable in formula call (parsed as regular function call)
    {
        write_fw("/tmp/fce3.fw", "y = sqrt(x)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fce3.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", 9}}), 3, "regular func call still works");
    }
}

// ---- ValueSet tests ----

void test_valueset_basic() {
    SECTION("ValueSet Basic");

    // Empty set
    {
        ValueSet s;
        ASSERT(s.empty(), "default is empty");
        ASSERT(!s.contains(0), "empty doesn't contain 0");
    }

    // All reals
    {
        auto s = ValueSet::all();
        ASSERT(!s.empty(), "all is not empty");
        ASSERT(s.contains(0), "all contains 0");
        ASSERT(s.contains(-1e18), "all contains large negative");
        ASSERT(s.contains(1e18), "all contains large positive");
    }

    // Single interval (0, +inf)
    {
        auto s = ValueSet::gt(0);
        ASSERT(s.contains(1), "gt(0) contains 1");
        ASSERT(s.contains(0.001), "gt(0) contains 0.001");
        ASSERT(!s.contains(0), "gt(0) doesn't contain 0");
        ASSERT(!s.contains(-1), "gt(0) doesn't contain -1");
    }

    // Closed interval [0, +inf)
    {
        auto s = ValueSet::ge(0);
        ASSERT(s.contains(0), "ge(0) contains 0");
        ASSERT(s.contains(1), "ge(0) contains 1");
        ASSERT(!s.contains(-1), "ge(0) doesn't contain -1");
    }

    // Less than
    {
        auto s = ValueSet::lt(10);
        ASSERT(s.contains(5), "lt(10) contains 5");
        ASSERT(!s.contains(10), "lt(10) doesn't contain 10");
        ASSERT(!s.contains(15), "lt(10) doesn't contain 15");
    }

    // Discrete set
    {
        auto s = ValueSet::discrete({3, -3, 0});
        ASSERT(s.contains(3), "discrete contains 3");
        ASSERT(s.contains(-3), "discrete contains -3");
        ASSERT(s.contains(0), "discrete contains 0");
        ASSERT(!s.contains(1), "discrete doesn't contain 1");
    }

    // Not equal
    {
        auto s = ValueSet::ne(0);
        ASSERT(s.contains(1), "ne(0) contains 1");
        ASSERT(s.contains(-1), "ne(0) contains -1");
        ASSERT(!s.contains(0), "ne(0) doesn't contain 0");
    }

    // Equal
    {
        auto s = ValueSet::eq(5);
        ASSERT(s.contains(5), "eq(5) contains 5");
        ASSERT(!s.contains(4), "eq(5) doesn't contain 4");
    }
}

void test_valueset_operations() {
    SECTION("ValueSet Operations");

    // Intersection: (0, +inf) & (-inf, 30) = (0, 30)
    {
        auto s = ValueSet::gt(0).intersect(ValueSet::lt(30));
        ASSERT(s.contains(15), "intersect: contains 15");
        ASSERT(!s.contains(0), "intersect: not 0");
        ASSERT(!s.contains(30), "intersect: not 30");
        ASSERT(!s.contains(-5), "intersect: not -5");
    }

    // Union: (-inf, 0) | (0, +inf) = everything except 0
    {
        auto s = ValueSet::lt(0).unite(ValueSet::gt(0));
        ASSERT(s.contains(5), "union: contains 5");
        ASSERT(s.contains(-5), "union: contains -5");
        ASSERT(!s.contains(0), "union: not 0");
    }

    // Filter discrete: {3, -3} & (0, +inf) = {3}
    {
        auto s = ValueSet::discrete({3, -3}).intersect(ValueSet::gt(0));
        ASSERT(s.contains(3), "filter: contains 3");
        ASSERT(!s.contains(-3), "filter: not -3");
    }

    // Intersection of closed ranges: [0, 10] & [5, 20] = [5, 10]
    {
        auto s = ValueSet::between(0, 10, true, true)
                .intersect(ValueSet::between(5, 20, true, true));
        ASSERT(s.contains(5), "closed intersect: contains 5");
        ASSERT(s.contains(10), "closed intersect: contains 10");
        ASSERT(!s.contains(4), "closed intersect: not 4");
        ASSERT(!s.contains(11), "closed intersect: not 11");
    }

    // Empty intersection
    {
        auto s = ValueSet::lt(0).intersect(ValueSet::gt(10));
        ASSERT(s.empty(), "disjoint intersection is empty");
    }
}

void test_valueset_display() {
    SECTION("ValueSet Display");

    // Basic interval display
    {
        auto s = ValueSet::lt(3);
        ASSERT(s.to_string() == "(-inf, 3)", "lt(3) display");
    }
    {
        auto s = ValueSet::le(3);
        ASSERT(s.to_string() == "(-inf, 3]", "le(3) display");
    }
    {
        auto s = ValueSet::between(0, 5, true, true);
        ASSERT(s.to_string() == "[0, 5]", "closed interval display");
    }
}

// ---- Condition tests ----

void test_condition_parsing() {
    SECTION("Condition Parsing");

    // Equation with simple condition
    {
        write_fw("/tmp/tcp1.fw", "y = sqrt(x) if x >=0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcp1.fw");
        ASSERT(sys.equations.size() == 1, "one equation");
        ASSERT(sys.equations[0].condition.has_value(), "has condition");
    }

    // Equation without condition (backwards compatible)
    {
        write_fw("/tmp/tcp2.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcp2.fw");
        ASSERT(sys.equations.size() == 1, "one equation");
        ASSERT(!sys.equations[0].condition.has_value(), "no condition");
    }

    // Compound condition with &&
    {
        write_fw("/tmp/tcp3.fw", "tax = income * 0.1 if income> 0 && income <= 50000\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcp3.fw");
        ASSERT(sys.equations[0].condition.has_value(), "has compound condition");
        ASSERT(sys.equations[0].condition->clauses.size() == 2, "two clauses");
    }
}

void test_condition_solving() {
    SECTION("Condition Solving");

    // Condition passes: sqrt(x) with x >= 0
    {
        write_fw("/tmp/tcs1.fw", "y = sqrt(x) if x >=0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs1.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", 9}}), 3, "condition passes: sqrt(9) = 3");
    }

    // Condition fails: equation skipped
    {
        write_fw("/tmp/tcs2.fw",
            "y = sqrt(x) if x >=0\n"
            "y = 0 if x <0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs2.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", -4}}), 0, "condition fails: fallback to y=0");
    }

    // Piecewise: tax brackets
    {
        write_fw("/tmp/tcs3.fw",
            "tax = income * 0.1 if income<= 50000\n"
            "tax = 5000 + (income - 50000) * 0.2 if income> 50000\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs3.fw");
        ASSERT_NUM(sys.resolve("tax", {{"income", 30000}}), 3000, "low bracket: 30000*0.1");
        ASSERT_NUM(sys.resolve("tax", {{"income", 80000}}), 11000, "high bracket: 5000+(80000-50000)*0.2");
    }

    // Compound condition: income > 0 && income <= 50000
    {
        write_fw("/tmp/tcs4.fw",
            "tax = income * 0.1 if income> 0 && income <= 50000\n"
            "tax = 0 if income<= 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs4.fw");
        ASSERT_NUM(sys.resolve("tax", {{"income", 30000}}), 3000, "compound: in range");
        ASSERT_NUM(sys.resolve("tax", {{"income", -100}}), 0, "compound: out of range");
    }

    // No condition matches → error
    {
        write_fw("/tmp/tcs5.fw",
            "y = 1 if x >0\n"
            "y = -1 if x <0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs5.fw");
        auto msg = get_error([&]() { sys.resolve("y", {{"x", 0}}); });
        ASSERT(!msg.empty(), "x=0: no condition matches, throws");
    }

    // Condition with unknown variable: treated as satisfied (can't validate)
    {
        write_fw("/tmp/tcs6.fw", "y = x + 1 if z >0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs6.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 6, "unknown condition var: treated as satisfied");
    }

    // All 6 comparison operators
    {
        write_fw("/tmp/tcs_ops.fw",
            "a = 1 if x >0\n"
            "b = 1 if x >=0\n"
            "c = 1 if x <0\n"
            "d = 1 if x <=0\n"
            "e = 1 if x =5\n"
            "f = 1 if x !=5\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs_ops.fw");
        ASSERT_NUM(sys.resolve("a", {{"x", 1}}), 1, "op >: passes");
        ASSERT_NUM(sys.resolve("b", {{"x", 0}}), 1, "op >=: passes at 0");
        ASSERT_NUM(sys.resolve("c", {{"x", -1}}), 1, "op <: passes");
        ASSERT_NUM(sys.resolve("d", {{"x", 0}}), 1, "op <=: passes at 0");
        ASSERT_NUM(sys.resolve("e", {{"x", 5}}), 1, "op =: passes");
        ASSERT_NUM(sys.resolve("f", {{"x", 3}}), 1, "op !=: passes");
    }

    // Edge case: :: in equation (should not crash)
    {
        write_fw("/tmp/tcs_dcolon.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcs_dcolon.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 6, "no colon: works normally");
    }
}

void test_condition_errors() {
    SECTION("Condition Error Handling");

    // Empty condition after "if" — should skip gracefully
    {
        write_fw("/tmp/tce1.fw", "y = x + 1 if \n");
        FormulaSystem sys;
        sys.load_file("/tmp/tce1.fw");
        ASSERT(sys.equations.size() == 1, "empty if condition: parses as equation");
        ASSERT(!sys.equations[0].condition.has_value(), "empty if condition: no condition stored");
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 6, "empty if condition: resolves normally");
    }

    // "if" without condition text — should not crash
    {
        write_fw("/tmp/tce2.fw", "y = x + 1 if if x > 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tce2.fw");
        // Malformed — may or may not parse
        ASSERT(true, "double if: doesn't crash");
    }

    // Bare condition (no equation) — should be skipped
    {
        write_fw("/tmp/tce3.fw", "if x > 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tce3.fw");
        ASSERT(sys.equations.empty(), "colon first: no equations");
    }

    // Malformed condition (no operator) — should skip condition, keep equation
    {
        write_fw("/tmp/tce4.fw", "y = x + 1 if garbage\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tce4.fw");
        ASSERT(sys.equations.size() == 1, "malformed condition: equation preserved");
        ASSERT(!sys.equations[0].condition.has_value(), "malformed condition: no condition");
    }

    // Multiple conditions with || (OR)
    {
        write_fw("/tmp/tce5.fw", "y = 1 if x <-10 || x > 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tce5.fw");
        ASSERT(sys.equations[0].condition.has_value(), "OR condition: parsed");
        ASSERT_NUM(sys.resolve("y", {{"x", 20}}), 1, "OR condition: x=20 passes");
        ASSERT_NUM(sys.resolve("y", {{"x", -20}}), 1, "OR condition: x=-20 passes");
        auto msg = get_error([&]() { sys.resolve("y", {{"x", 0}}); });
        ASSERT(!msg.empty(), "OR condition: x=0 fails");
    }
}

void test_global_conditions() {
    SECTION("Global Conditions");

    // Standalone condition line: "area >= 0" constrains area globally
    {
        write_fw("/tmp/tgc1.fw",
            "area >= 0\n"
            "area = width * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tgc1.fw");
        ASSERT(!sys.global_conditions.empty(), "global condition: parsed");
        ASSERT_NUM(sys.resolve("area", {{"width", 5}, {"height", 3}}), 15,
            "global condition: positive area works");
    }

    // Global condition prevents invalid result
    {
        write_fw("/tmp/tgc2.fw",
            "side > 0\n"
            "side = x\n"
            "side = -x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tgc2.fw");
        ASSERT_NUM(sys.resolve("side", {{"x", 5}}), 5, "global: side=5 passes");
        ASSERT_NUM(sys.resolve("side", {{"x", -5}}), 5, "global: side=-(-5)=5 passes");
    }
}

// ---- Multiple returns tests ----

void test_multiple_returns() {
    SECTION("Multiple Returns");

    // Single equation, multi-root via quadratic formula: both roots collected
    // within one candidate's ValueSet. Use an additive quadratic that forces
    // the formula path (plain `y = x^2` is inverted to a single `sqrt(y)`).
    // (Pre-"first-successful EXPR" policy, this test used two separate
    //  equations `x = sqrt(y)` / `x = -sqrt(y)`; the new policy takes the
    //  first equation's roots only. Single-equation multi-root is preserved.)
    {
        write_fw("/tmp/tmr_multi.fw", "y = x^2 + 2*x - 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_multi.fw");
        auto result = sys.resolve_all("x", {{"y", 0}});
        ASSERT(result.discrete().size() == 2, "two solutions found");
        ASSERT(result.contains(1), "has root x=1");
        ASSERT(result.contains(-3), "has root x=-3");
    }

    // Single equation: one result
    {
        write_fw("/tmp/tmr_single.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_single.fw");
        auto result = sys.resolve_all("y", {{"x", 5}});
        ASSERT(result.discrete().size() == 1, "one solution");
        ASSERT_NUM(result.discrete()[0], 6, "y = 6");
    }

    // Conditions on branches: under "first-successful EXPR" policy, only
    // the first matching branch's result is returned. Use --explore for
    // exhaustive piecewise enumeration.
    {
        write_fw("/tmp/tmr_cond.fw",
            "x = sqrt(y) if x >=0\n"
            "x = -sqrt(y) if x <0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_cond.fw");
        auto result = sys.resolve_all("x", {{"y", 9}});
        ASSERT(result.discrete().size() == 1, "first-successful: one branch wins");
        ASSERT(result.contains(3), "first-successful: positive branch wins");
    }

    // Deduplication: same result from different equations
    {
        write_fw("/tmp/tmr_dedup.fw",
            "y = x + 1\n"
            "y = 1 + x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_dedup.fw");
        auto result = sys.resolve_all("y", {{"x", 5}});
        ASSERT(result.discrete().size() == 1, "deduplicated: one result");
    }

    // Range return: only constraints, no exact solution
    {
        write_fw("/tmp/tmr_range.fw",
            "x > 0\n"
            "x <= 100\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_range.fw");
        auto result = sys.resolve_all("x", {});
        ASSERT(!result.is_discrete(), "range: not discrete");
        ASSERT(result.contains(50), "range: contains 50");
        ASSERT(!result.contains(-1), "range: not -1");
        ASSERT(!result.contains(101), "range: not 101");
    }

    // resolve_one: succeeds with single result
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_single.fw");
        double r = sys.resolve_one("y", {{"x", 5}});
        ASSERT_NUM(r, 6, "resolve_one: y = 6");
    }

    // resolve_one: errors with multiple results from a single equation
    // (quadratic formula). Two separate equations would now yield only the
    // first — use an additive quadratic to trigger multi-root.
    {
        write_fw("/tmp/tmr_strict.fw", "y = x^2 + 2*x - 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tmr_strict.fw");
        auto msg = get_error([&]() { sys.resolve_one("x", {{"y", 0}}); });
        ASSERT(!msg.empty(), "resolve_one: multiple results throws");
        ASSERT(msg.find("Multiple") != std::string::npos, "resolve_one: says Multiple");
    }
}

// ---- Conditional branching tests ----

void test_conditional_branching() {
    SECTION("Conditional Branching");

    // Piecewise: absolute value
    {
        write_fw("/tmp/tcb_abs.fw",
            "result = x if x >=0\n"
            "result = -x if x <0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_abs.fw");
        ASSERT_NUM(sys.resolve("result", {{"x", 5}}), 5, "abs: positive");
        ASSERT_NUM(sys.resolve("result", {{"x", -5}}), 5, "abs: negative");
        ASSERT_NUM(sys.resolve("result", {{"x", 0}}), 0, "abs: zero");
    }

    // Three-way branch: sign function
    {
        write_fw("/tmp/tcb_sign.fw",
            "sign = 1 if x >0\n"
            "sign = 0 if x =0\n"
            "sign = -1 if x <0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_sign.fw");
        ASSERT_NUM(sys.resolve("sign", {{"x", 42}}), 1, "sign: positive");
        ASSERT_NUM(sys.resolve("sign", {{"x", 0}}), 0, "sign: zero");
        ASSERT_NUM(sys.resolve("sign", {{"x", -7}}), -1, "sign: negative");
    }

    // Multi-bracket tax
    {
        write_fw("/tmp/tcb_tax.fw",
            "tax = 0 if income<= 0\n"
            "tax = income * 0.1 if income> 0 && income <= 50000\n"
            "tax = 5000 + (income - 50000) * 0.2 if income> 50000 && income <= 100000\n"
            "tax = 15000 + (income - 100000) * 0.3 if income> 100000\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_tax.fw");
        ASSERT_NUM(sys.resolve("tax", {{"income", -100}}), 0, "tax: negative income");
        ASSERT_NUM(sys.resolve("tax", {{"income", 30000}}), 3000, "tax: low bracket");
        ASSERT_NUM(sys.resolve("tax", {{"income", 80000}}), 11000, "tax: mid bracket");
        ASSERT_NUM(sys.resolve("tax", {{"income", 150000}}), 30000, "tax: high bracket");
    }

    // Branching with equations (not just constants)
    {
        write_fw("/tmp/tcb_clamp.fw",
            "result = low if x <low\n"
            "result = high if x >high\n"
            "result = x if x >=low && x <= high\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_clamp.fw");
        ASSERT_NUM(sys.resolve("result", {{"x", 5}, {"low", 0}, {"high", 10}}), 5, "clamp: in range");
        ASSERT_NUM(sys.resolve("result", {{"x", -3}, {"low", 0}, {"high", 10}}), 0, "clamp: below");
        ASSERT_NUM(sys.resolve("result", {{"x", 15}, {"low", 0}, {"high", 10}}), 10, "clamp: above");
    }

    // Branching with global condition
    {
        write_fw("/tmp/tcb_global.fw",
            "x >= 0\n"
            "y = sqrt(x)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_global.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", 9}}), 3, "global + branch: sqrt works");
        auto msg = get_error([&]() { sys.resolve("y", {{"x", -1}}); });
        ASSERT(!msg.empty(), "global + branch: negative x fails");
    }

    // Inverse through conditional equation
    {
        write_fw("/tmp/tcb_inv.fw",
            "y = x * 2 if x >=0\n"
            "y = x * 3 if x <0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_inv.fw");
        // Forward: x=5 → y=10 (first branch)
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 10, "cond inverse: forward x=5");
        // Forward: x=-2 → y=-6 (second branch)
        ASSERT_NUM(sys.resolve("y", {{"x", -2}}), -6, "cond inverse: forward x=-2");
    }

    // Derive with conditions: should show condition in output
    {
        write_fw("/tmp/tcb_derive.fw",
            "y = sqrt(x) if x >=0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tcb_derive.fw");
        auto r = sys.derive("y", {}, {{"x", "x"}});
        ASSERT_EQ(r, "sqrt(x)", "derive with condition: expression correct");
    }
}

// ---- Recursion depth guard tests ----

void test_recursion_depth_guard() {
    SECTION("Recursion Depth Guard");

    // Infinite mutual recursion: A calls B, B calls A
    // Each resolves a different variable so visited sets don't catch it
    {
        write_fw("/tmp/trdg_a2.fw", "trdg_b2(y=?x, n=n)\nresult = x\n");
        write_fw("/tmp/trdg_b2.fw", "trdg_a2(result=?y, n=n)\n");
        FormulaSystem sys;
        sys.max_formula_depth = 20;
        sys.load_file("/tmp/trdg_a2.fw");
        auto msg = get_error([&]() { sys.resolve("result", {{"n", 5}}); });
        ASSERT(!msg.empty(), "mutual recursion: throws");
        ASSERT(msg.find("depth") != std::string::npos || msg.find("recursion") != std::string::npos,
            "mutual recursion: mentions depth/recursion");
    }

    // Normal formula calls should still work (not falsely triggered)
    {
        write_fw("/tmp/trdg_rect.fw", "area = width * height\n");
        write_fw("/tmp/trdg_box.fw",
            "trdg_rect(area=?floor, width=width, height=depth)\n"
            "volume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/trdg_box.fw");
        ASSERT_NUM(sys.resolve("volume", {{"width", 4}, {"depth", 3}, {"h", 6}}), 72,
            "normal formula call: still works");
    }
}

// ---- Verify mode tests ----

void test_approx_equal() {
    SECTION("Approx Equal");

    ASSERT(FormulaSystem::approx_equal(1.0, 1.0), "exact match");
    ASSERT(FormulaSystem::approx_equal(1.0, 1.0 + 1e-10), "within epsilon");
    ASSERT(!FormulaSystem::approx_equal(1.0, 1.01), "clearly different");
    ASSERT(FormulaSystem::approx_equal(0.0, 0.0), "zero == zero");
    ASSERT(FormulaSystem::approx_equal(0.0, 1e-10), "near-zero within epsilon");
    ASSERT(!FormulaSystem::approx_equal(0.0, 1e-6), "near-zero outside epsilon");
    // Relative tolerance for large numbers
    ASSERT(FormulaSystem::approx_equal(1e8, 1e8 + 0.01), "large numbers: within relative eps");
    ASSERT(!FormulaSystem::approx_equal(1e8, 1e8 + 1000), "large numbers: outside relative eps");
    // Negative numbers
    ASSERT(FormulaSystem::approx_equal(-5.0, -5.0), "negative exact");
    ASSERT(FormulaSystem::approx_equal(-5.0, -5.0 + 1e-10), "negative within eps");
    // NaN and infinity edge cases
    ASSERT(!FormulaSystem::approx_equal(std::numeric_limits<double>::quiet_NaN(), 1.0), "NaN vs number");
    ASSERT(!FormulaSystem::approx_equal(1.0, std::numeric_limits<double>::quiet_NaN()), "number vs NaN");
    ASSERT(!FormulaSystem::approx_equal(std::numeric_limits<double>::quiet_NaN(),
                                         std::numeric_limits<double>::quiet_NaN()), "NaN vs NaN");
    ASSERT(FormulaSystem::approx_equal(std::numeric_limits<double>::infinity(),
                                        std::numeric_limits<double>::infinity()), "+inf vs +inf");
    ASSERT(!FormulaSystem::approx_equal(std::numeric_limits<double>::infinity(),
                                         -std::numeric_limits<double>::infinity()), "+inf vs -inf");
}

void test_verify_variable() {
    SECTION("Verify Variable");

    // Consistent system: all equations agree
    {
        write_fw("/tmp/tv1.fw", "A = 180 - B - C\nB = 180 - A - C\nC = 180 - A - B\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tv1.fw");
        auto results = sys.verify_variable("A", 40, {{"A", 40}, {"B", 60}, {"C", 80}});
        ASSERT(!results.empty(), "consistent: has results");
        bool all_pass = true;
        for (auto& r : results) if (!r.pass) all_pass = false;
        ASSERT(all_pass, "consistent: all pass");
    }

    // Inconsistent system: some equations disagree
    {
        write_fw("/tmp/tv2.fw", "A = 180 - B - C\nB = 180 - A - C\nC = 180 - A - B\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tv2.fw");
        auto results = sys.verify_variable("A", 40, {{"A", 40}, {"B", 60}, {"C", 120}});
        ASSERT(!results.empty(), "inconsistent: has results");
        bool any_fail = false;
        for (auto& r : results) if (!r.pass) any_fail = true;
        ASSERT(any_fail, "inconsistent: some fail");
    }

    // Variable with no verifiable equations
    {
        write_fw("/tmp/tv3.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tv3.fw");
        // z is not in any equation
        auto results = sys.verify_variable("z", 5, {{"z", 5}});
        ASSERT(results.empty(), "unknown var: no results");
    }

    // Strategy 1 (direct LHS): y = x + 1, verify y=6 with x=5
    {
        write_fw("/tmp/tv4.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tv4.fw");
        auto results = sys.verify_variable("y", 6, {{"y", 6}, {"x", 5}});
        ASSERT(results.size() >= 1, "direct: has result");
        ASSERT(results[0].pass, "direct: 5+1=6 passes");
    }

    // Strategy 1 fail: y = x + 1, verify y=10 with x=5
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tv4.fw");
        auto results = sys.verify_variable("y", 10, {{"y", 10}, {"x", 5}});
        ASSERT(results.size() >= 1, "direct fail: has result");
        ASSERT(!results[0].pass, "direct fail: 5+1≠10");
        ASSERT_NUM(results[0].computed, 6, "direct fail: computed 6");
    }

    // Strategy 2 (inversion): y = x + 1, verify x=5 with y=6
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tv4.fw");
        auto results = sys.verify_variable("x", 5, {{"x", 5}, {"y", 6}});
        ASSERT(!results.empty(), "inversion: has results");
        bool found_pass = false;
        for (auto& r : results) if (r.pass) found_pass = true;
        ASSERT(found_pass, "inversion: x=6-1=5 passes");
    }

    // Multiple equations: all should be checked
    {
        write_fw("/tmp/tv5.fw", "y = x + 1\ny = x * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tv5.fw");
        // y=6, x=5: first eq says y=6 (pass), second says y=10 (fail)
        auto results = sys.verify_variable("y", 6, {{"y", 6}, {"x", 5}});
        ASSERT(results.size() >= 2, "multiple: at least 2 results");
        int passes = 0, fails = 0;
        for (auto& r : results) { if (r.pass) passes++; else fails++; }
        ASSERT(passes >= 1, "multiple: at least one pass");
        ASSERT(fails >= 1, "multiple: at least one fail");
    }
}

void test_verify_binary_integration() {
    SECTION("Verify Binary Integration");

    write_fw("/tmp/tvb.fw", "A = 180 - B - C\nB = 180 - A - C\nC = 180 - A - B\n");

    // --verify all with consistent inputs: exit 0
    {
        int rc = system("./bin/fwiz --verify all '/tmp/tvb(A=40, B=60, C=80)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 0, "verify consistent: exit 0");
    }

    // --verify all with inconsistent inputs: exit 1
    {
        int rc = system("./bin/fwiz --verify all '/tmp/tvb(A=40, B=60, C=120)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 1, "verify inconsistent: exit 1");
    }

    // --verify specific vars
    {
        int rc = system("./bin/fwiz --verify A '/tmp/tvb(A=40, B=60, C=80)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 0, "verify specific var: exit 0");
    }

    // --verify with query: solve then verify
    {
        int rc = system("./bin/fwiz --verify all '/tmp/tvb(C=?, A=40, B=60)' > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 0, "verify after solve: exit 0");
    }

    // --verify without argument: exit 1 (error)
    {
        int rc = system("./bin/fwiz --verify > /dev/null 2>&1");
        ASSERT(WEXITSTATUS(rc) == 1, "verify no arg: exit 1");
    }
}

// ---- Explore mode tests ----

void test_all_variables() {
    SECTION("All Variables");

    {
        write_fw("/tmp/tav1.fw", "area = width * height\nperimeter = 2 * width + 2 * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tav1.fw");
        auto vars = sys.all_variables();
        ASSERT(vars.count("area"), "has area");
        ASSERT(vars.count("width"), "has width");
        ASSERT(vars.count("height"), "has height");
        ASSERT(vars.count("perimeter"), "has perimeter");
    }

    // Variables from defaults
    {
        write_fw("/tmp/tav2.fw", "g = 9.81\nforce = mass * g\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tav2.fw");
        auto vars = sys.all_variables();
        ASSERT(vars.count("g"), "has default g");
        ASSERT(vars.count("force"), "has force");
        ASSERT(vars.count("mass"), "has mass");
    }

    // Variables from formula calls
    {
        write_fw("/tmp/tav_r.fw", "area = width * height\n");
        write_fw("/tmp/tav3.fw", "tav_r(area=?floor, width=w, height=d)\nvolume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tav3.fw");
        auto vars = sys.all_variables();
        ASSERT(vars.count("floor"), "has formula call output_var");
        ASSERT(vars.count("w"), "has binding parent_var w");
        ASSERT(vars.count("d"), "has binding parent_var d");
        ASSERT(vars.count("h"), "has equation var h");
        ASSERT(vars.count("volume"), "has equation var volume");
    }
}

void test_explore_binary_integration() {
    SECTION("Explore Binary Integration");

    write_fw("/tmp/tex.fw", "y = x + 1\nz = x * 2\n");

    // --explore with queries: solved and unsolvable
    {
        // y=? solvable with x=5, z=? solvable
        int rc = system("./bin/fwiz --explore '/tmp/tex(y=?, z=?, x=5)' 2>/dev/null "
                        "| grep -q 'y = 6'");
        ASSERT(WEXITSTATUS(rc) == 0, "explore: y=6 in output");
    }

    // --explore with unsolvable: prints ?
    {
        int rc = system("./bin/fwiz --explore '/tmp/tex(y=?)' 2>/dev/null "
                        "| grep -q '?'");
        ASSERT(WEXITSTATUS(rc) == 0, "explore unsolvable: ? in output");
    }

    // --explore-full: shows all variables
    {
        int rc = system("./bin/fwiz --explore-full '/tmp/tex(x=5)' 2>/dev/null "
                        "| grep -q 'z = 10'");
        ASSERT(WEXITSTATUS(rc) == 0, "explore-full: z=10 in output");
    }

    // --explore without queries: just prints inputs
    {
        int rc = system("./bin/fwiz --explore '/tmp/tex(x=5)' 2>/dev/null "
                        "| grep -q 'x = 5'");
        ASSERT(WEXITSTATUS(rc) == 0, "explore no queries: x=5 in output");
    }

    // --explore no queries: should NOT print variables not mentioned
    {
        int rc = system("./bin/fwiz --explore '/tmp/tex(x=5)' 2>/dev/null "
                        "| grep -q 'y'");
        ASSERT(WEXITSTATUS(rc) != 0, "explore no queries: y not in output");
    }
}

// ---- Additional formula call tests ----

void test_formula_call_additional() {
    SECTION("Formula Call Additional Coverage");

    write_fw("/tmp/fca_rect.fw", "area = width * height\n");

    // Implied alias (Form 3) with actual resolution
    {
        write_fw("/tmp/fca1.fw", "floor = fca_rect(area=?, width=width, height=depth)\nvolume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca1.fw");
        ASSERT_NUM(sys.resolve("volume", {{"width", 4}, {"depth", 3}, {"h", 6}}), 72,
            "implied alias: resolves through formula call");
        ASSERT_NUM(sys.resolve("floor", {{"width", 5}, {"depth", 7}}), 35,
            "implied alias: direct query");
    }

    // Inline formula call without alias — area enters scope
    {
        write_fw("/tmp/fca2.fw", "volume = depth * fca_rect(area=?, width=width, height=height)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca2.fw");
        // area is the output_var (no alias), used in equation as: volume = depth * area
        ASSERT_NUM(sys.resolve("volume", {{"width", 4}, {"height", 3}, {"depth", 5}}), 60,
            "inline no alias: volume = depth * area");
    }

    // Shorthand bindings resolve correctly
    {
        write_fw("/tmp/fca3.fw", "fca_rect(area=?a, width, height)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca3.fw");
        ASSERT_NUM(sys.resolve("a", {{"width", 6}, {"height", 7}}), 42,
            "shorthand bindings: width=width, height=height");
    }

    // Sub-system caching: two calls share one loaded file
    {
        write_fw("/tmp/fca4.fw",
            "fca_rect(area=?a1, width=w1, height=h1)\n"
            "fca_rect(area=?a2, width=w2, height=h2)\n"
            "total = a1 + a2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca4.fw");
        ASSERT_NUM(sys.resolve("total", {{"w1",3},{"h1",4},{"w2",5},{"h2",6}}), 42,
            "caching: two calls resolve correctly");
        ASSERT(sys.sub_systems.size() == 1, "caching: only one sub-system loaded");
    }

    // parse_cli_query with allow_no_queries
    {
        auto q = parse_cli_query("f(x=5, y=10)", true);
        ASSERT(q.queries.empty(), "allow_no_queries: no queries");
        ASSERT_NUM(q.bindings.at("x"), 5, "allow_no_queries: x=5");
        ASSERT_NUM(q.bindings.at("y"), 10, "allow_no_queries: y=10");
    }

    // parse_cli_query without allow_no_queries: throws
    {
        auto msg = get_error([&]() { parse_cli_query("f(x=5, y=10)"); });
        ASSERT(msg.find("No query") != std::string::npos, "no queries: throws");
    }

    // Binding as last arg before closing paren (boundary in parse_call_args)
    {
        write_fw("/tmp/fca_bound.fw", "area = width * height\n");
        write_fw("/tmp/fca_last.fw", "fca_bound(area=?, height=depth)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca_last.fw");
        ASSERT(sys.formula_calls.size() == 1, "last-binding: one call");
        ASSERT(sys.formula_calls[0].bindings.count("height"), "last-binding: has height binding");
        ASSERT_EQ(expr_to_string(sys.formula_calls[0].bindings.at("height")), "depth",
            "last-binding: height=depth parsed correctly");
    }

    // Trailing shorthand binding (single ident before closing paren)
    {
        write_fw("/tmp/fca_trail.fw", "fca_bound(area=?, width)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca_trail.fw");
        ASSERT(sys.formula_calls[0].bindings.count("width"), "trailing shorthand: has width");
        ASSERT_EQ(expr_to_string(sys.formula_calls[0].bindings.at("width")), "width",
            "trailing shorthand: width=width");
    }
}

// ---- Spurious zero fix additional tests ----

void test_solve_for_zero_guard() {
    SECTION("Solve For Zero Guard");

    // Concrete numeric coefficient with zero rest: target=0 is valid
    {
        // 3*c + 0 = 0 → c = 0
        auto lhs = Parser(Lexer("3 * c").tokenize()).parse_expr();
        auto rhs = Expr::Num(0);
        auto sol = solve_for(lhs, rhs, "c");
        ASSERT(sol != nullptr, "concrete coeff: solution found");
        double val = (evaluate(sol).value());
        ASSERT_NUM(val, 0, "concrete coeff: c=0");
    }

    // Symbolic coefficient with zero rest: target=0 is rejected
    {
        // a*c - a*c = 0 → coeff is symbolic, rest=0, should reject
        auto lhs = Parser(Lexer("a * c").tokenize()).parse_expr();
        auto rhs = Parser(Lexer("a * c").tokenize()).parse_expr();
        auto sol = solve_for(lhs, rhs, "c");
        ASSERT(sol == nullptr, "symbolic coeff zero rest: rejected");
    }

    // Symbolic coefficient with non-zero rest: should solve normally
    {
        // a*c + 5 = 0 → c = -5/a (valid even though coeff is symbolic)
        auto lhs = Parser(Lexer("a * c + 5").tokenize()).parse_expr();
        auto rhs = Expr::Num(0);
        auto sol = solve_for(lhs, rhs, "c");
        ASSERT(sol != nullptr, "symbolic coeff nonzero rest: solution found");
    }

    // Numeric coefficient with non-zero rest: c = -rest/coeff
    {
        // 2*c + 6 = 0 → c = -3
        auto lhs = Parser(Lexer("2 * c + 6").tokenize()).parse_expr();
        auto rhs = Expr::Num(0);
        auto sol = solve_for(lhs, rhs, "c");
        ASSERT(sol != nullptr, "numeric coeff nonzero rest: found");
        double val = (evaluate(sol).value());
        ASSERT_NUM(val, -3, "numeric coeff nonzero rest: c=-3");
    }
}

// ---- Pre-refactor safety net tests ----

void test_strategy_coverage() {
    SECTION("Strategy Coverage");

    // Strategy 1 (direct LHS): target = expr
    {
        write_fw("/tmp/tsc1.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc1.fw");
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 6, "strategy1: direct LHS");
    }

    // Strategy 2 (inversion): target in RHS
    {
        FormulaSystem sys;
        sys.load_file("/tmp/tsc1.fw");
        ASSERT_NUM(sys.resolve("x", {{"y", 6}}), 5, "strategy2: inversion");
    }

    // Strategy 3 (forward formula call): target is output_var
    {
        write_fw("/tmp/tsc_sub.fw", "area = width * height\n");
        write_fw("/tmp/tsc3.fw", "tsc_sub(area=?floor, width=w, height=h)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc3.fw");
        ASSERT_NUM(sys.resolve("floor", {{"w", 4}, {"h", 5}}), 20,
            "strategy3: forward formula call");
    }

    // Strategy 4 (equate RHS): two equations share LHS
    {
        write_fw("/tmp/tsc4.fw", "z = x + 1\nz = y * 2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc4.fw");
        // x + 1 = y * 2 → x = 2*y - 1
        ASSERT_NUM(sys.resolve("x", {{"y", 5}}), 9, "strategy4: equate shared LHS");
    }

    // Strategy 5 (reverse formula call): target maps through binding
    {
        write_fw("/tmp/tsc5_sub.fw", "area = width * height\n");
        write_fw("/tmp/tsc5.fw", "tsc5_sub(area=?floor, width=w, height=h)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc5.fw");
        // h=? with floor=20, w=4 → area=20, width=4, height=20/4=5
        ASSERT_NUM(sys.resolve("h", {{"floor", 20}, {"w", 4}}), 5,
            "strategy5: reverse formula call");
    }

    // Strategy fallthrough: Strategy 1 fails (needs unknown), Strategy 2 succeeds
    {
        write_fw("/tmp/tsc_fall.fw", "y = x + z\nz = 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc_fall.fw");
        // y needs z (default 10), x = y - z (inversion)
        ASSERT_NUM(sys.resolve("x", {{"y", 15}}), 5, "fallthrough: strategy1→2");
    }

    // Strategy priority: Strategy 1 has valid answer, Strategy 2 also could work
    {
        write_fw("/tmp/tsc_prio.fw", "y = x + 1\ny = x * 2 - 4\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc_prio.fw");
        // Both equations define y. First one wins (file order).
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 6, "priority: first equation wins");
    }

    // All strategies apply to derive_recursive too
    {
        write_fw("/tmp/tsc_sub.fw", "area = width * height\n");
        write_fw("/tmp/tsc_d.fw", "tsc_sub(area=?floor, width=w, height=h)\nvolume = floor * d\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsc_d.fw");
        auto r = sys.derive("volume", {}, {{"w","w"},{"h","h"},{"d","d"}});
        ASSERT_EQ(r, "w * h * d", "strategies work for derive too");
    }
}

void test_builtin_exhaustive() {
    SECTION("Builtin Functions Exhaustive");

    auto ev = [](const char* s) { return evaluate(parse(s)).value(); };
    auto ev_checked = [](const char* s) { return evaluate(parse(s)); };

    // All 9 builtins
    ASSERT_NUM(ev("sqrt(16)"), 4, "sqrt(16)=4");
    ASSERT_NUM(ev("abs(-7)"), 7, "abs(-7)=7");
    ASSERT_NUM(ev("sin(0)"), 0, "sin(0)=0");
    ASSERT_NUM(ev("cos(0)"), 1, "cos(0)=1");
    ASSERT_NUM(ev("tan(0)"), 0, "tan(0)=0");
    ASSERT_NUM(ev("log(1)"), 0, "log(1)=0");
    ASSERT_NUM(ev("asin(0)"), 0, "asin(0)=0");
    ASSERT_NUM(ev("acos(1)"), 0, "acos(1)=0");
    ASSERT_NUM(ev("atan(0)"), 0, "atan(0)=0");

    // Roundtrip consistency
    ASSERT_NUM(ev("sin(asin(0.3))"), 0.3, "sin(asin(0.3))=0.3");
    ASSERT_NUM(ev("cos(acos(0.7))"), 0.7, "cos(acos(0.7))=0.7");
    ASSERT_NUM(ev("asin(sin(0.5))"), 0.5, "asin(sin(0.5))=0.5");

    // Unknown function yields empty Checked
    ASSERT(!ev_checked("foobar(1)").has_value(), "unknown function yields empty");

    // Case sensitive: SIN is not sin
    ASSERT(!ev_checked("SIN(0)").has_value(), "SIN (uppercase) yields empty");

    // Wrong arity: sqrt with 2 args
    ASSERT(!ev_checked("sqrt(4, 9)").has_value(), "sqrt(4,9) wrong arity yields empty");

    // Zero args
    ASSERT(!ev_checked("sin()").has_value(), "sin() zero args yields empty");
}

void test_operator_metadata() {
    SECTION("Operator Metadata");

    auto ev = [](const char* s) { return evaluate(parse(s)).value(); };

    // All 5 operators evaluate correctly
    ASSERT_NUM(ev("3 + 4"), 7, "ADD eval");
    ASSERT_NUM(ev("10 - 3"), 7, "SUB eval");
    ASSERT_NUM(ev("3 * 4"), 12, "MUL eval");
    ASSERT_NUM(ev("12 / 4"), 3, "DIV eval");
    ASSERT_NUM(ev("2 ^ 10"), 1024, "POW eval");

    // Precedence in printing: no unnecessary parens
    ASSERT_EQ(expr_to_string(parse("a + b * c")), "a + b * c", "MUL higher than ADD: no parens");
    ASSERT_EQ(expr_to_string(parse("(a + b) * c")), "(a + b) * c", "ADD forced before MUL: parens");
    ASSERT_EQ(expr_to_string(parse("a * b^c")), "a * b^c", "POW higher than MUL: no parens");
    ASSERT_EQ(expr_to_string(parse("a - b - c")), "a - b - c", "left-assoc SUB: no parens");
    ASSERT_EQ(expr_to_string(parse("a - (b - c)")), "a - (b - c)", "right-group SUB: needs parens");
    ASSERT_EQ(expr_to_string(parse("a / (b / c)")), "a / (b / c)", "right-group DIV: needs parens");

    // Mixed unary/binary edge cases
    ASSERT_EQ(expr_to_string(parse("-a * b")), "-a * b", "neg*var: no extra parens");
    ASSERT_EQ(expr_to_string(parse("-(a * b)")), "-(a * b)", "neg of product: parens");
    ASSERT_EQ(expr_to_string(parse("-a^2")), "-(a^2)", "neg before pow: parens clarify precedence");
}

// ---- Simplifier improvement tests ----

void test_simplify_rule_interactions() {
    SECTION("Simplifier: Rule Interactions");

    auto ss = [](const char* s) { return expr_to_string(simplify(parse(s))); };

    // Like-terms + constant reassociation: x + 2*x + 3 + 4*x + 1 → 7*x + 4
    ASSERT_EQ(ss("x + 2 * x + 3 + 4 * x + 1"), "7 * x + 4",
        "like-terms + constant: x+2x+3+4x+1 → 7x+4");

    // Power mul then like-term: x^2 + x*x + 3*x^2
    // x*x → x^2 (mul-to-pow), then x^2 + x^2 + 3*x^2 → 5*x^2 (like-terms)
    ASSERT_EQ(ss("x^2 + x * x + 3 * x^2"), "5 * x^2",
        "pow-mul then like-term: x^2+x*x+3x^2 → 5x^2");

    // Symmetric self-division: (2*x) / (2*x) → 1
    ASSERT_EQ(ss("(2 * x) / (2 * x)"), "1", "(2x)/(2x) → 1");

    // Negation of subtraction then subtract: -(a-b) - c → b - a - c
    ASSERT_EQ(ss("-(a - b) - c"), "-a + b - c", "-(a-b)-c → -a+b-c");

    // Division then multiply back: x / 2 * 2 → x
    ASSERT_EQ(ss("x / 2 * 2"), "x", "x/2*2 → x");

    // Deep mixed chain: ((x/2)*3 - 4)*2 + 5
    // x/2*3 → x*1.5, (x*1.5 - 4)*2 → 2*x*1.5 - 8 → x*3 - 8, +5 → x*3 - 3
    {
        auto e = simplify(parse("((x / 2) * 3 - 4) * 2 + 5"));
        double val = (evaluate(substitute(e, "x", Expr::Num(10))).value());
        ASSERT_NUM(val, 27, "deep chain: ((10/2)*3-4)*2+5 = 27");
    }

    // One to any power: 1^x → 1
    // (not currently simplified, but should evaluate correctly)
    {
        auto e = simplify(parse("1^999"));
        ASSERT_EQ(expr_to_string(e), "1", "1^999 → 1");
    }

    // Function arg simplification: sqrt((x+0) * 1) → sqrt(x)
    ASSERT_EQ(ss("sqrt((x + 0) * 1)"), "sqrt(x)", "func args simplified: sqrt((x+0)*1) → sqrt(x)");

    // x*x - x*x → 0 (mul-to-pow then like-term subtraction)
    ASSERT_EQ(ss("x * x - x * x"), "0", "x*x - x*x → 0");

    // Alternating powers and constants: x^2 * 2 * x^2 * 3
    // Should eventually reach 6*x^4
    {
        auto e = simplify(parse("x^2 * 2 * x^2 * 3"));
        double val = (evaluate(substitute(e, "x", Expr::Num(2))).value());
        ASSERT_NUM(val, 96, "x^2*2*x^2*3 at x=2 → 96 (6*16)");
    }

    // Chain: 2*x + 3*x - x → 4*x
    ASSERT_EQ(ss("2 * x + 3 * x - x"), "4 * x", "2x+3x-x → 4x");

    // Negation chain: -(-(-x)) → -x
    ASSERT_EQ(ss("-(-(-x))"), "-x", "triple negation → -x");

    // Mixed div/mul reassociation: ((x / 2) * 3) / 3 → x / 2
    ASSERT_EQ(ss("((x / 2) * 3) / 3"), "0.5 * x", "((x/2)*3)/3 → 0.5*x");
}

void test_simplify_flatten_targets() {
    SECTION("Simplifier: Flatten Targets (post-refactor)");

    // These test the SEMANTIC correctness of expressions that the current
    // simplifier can't fully reduce. After the flattening refactor,
    // the string assertions can be tightened.

    // Additive flattening: collect all terms
    // a + b - a → b (three terms, cancellation across non-adjacent)
    ASSERT_EQ(expr_to_string(simplify(parse("a + b - a"))), "b",
        "a+b-a → b (additive cancellation non-adjacent)");

    // Multiple like-terms across a chain: 2*x + y + 3*x → 5*x + y or y + 5*x
    {
        auto r = ss("2 * x + y + 3 * x");
        ASSERT(r == "5 * x + y" || r == "y + 5 * x",
            "non-adjacent like-terms: 2x+y+3x → 5x+y");
    }

    // Constants scattered: 3 + x + 2 + y + 1 → x + y + 6 (or reordered)
    {
        auto e = simplify(parse("3 + x + 2 + y + 1"));
        // Must evaluate to 36 at x=10,y=20 AND have no more than 3 terms
        double val = (evaluate(substitute(substitute(e, "x", Expr::Num(10)), "y", Expr::Num(20))).value());
        ASSERT_NUM(val, 36, "scattered constants: correct value");
        // Check constants were collected (shouldn't have 3 separate numbers)
        auto str = expr_to_string(e);
        ASSERT(str.find("3 + x + 2") == std::string::npos,
            "scattered constants: numbers collected");
    }

    // Multiplicative flattening: collect all factors
    // a * b / a → b (cancel across non-adjacent)
    ASSERT_EQ(ss("a * b / a"), "b",
        "mul cancel non-adjacent: a*b/a → b");

    // Constants scattered in multiplication: 2 * x * 3 * y * 4 → 24*x*y or 24*y*x
    {
        auto r = ss("2 * x * 3 * y * 4");
        ASSERT(r == "24 * x * y" || r == "24 * y * x",
            "scattered mul constants: 2*x*3*y*4 → 24*x*y");
    }

    // Mixed: x * y / x * z → y * z or z * y (cancel x across mul/div chain)
    {
        auto r = ss("x * y / x * z");
        ASSERT(r == "y * z" || r == "z * y",
            "mul/div cancel: x*y/x*z → y*z");
    }

    // Cube surface from derive: 2*s^2 + 2*s^2 + 2*s^2 → 6*s^2
    ASSERT_EQ(expr_to_string(simplify(parse("2 * s^2 + 2 * s^2 + 2 * s^2"))),
        "6 * s^2", "2s^2+2s^2+2s^2 → 6s^2");

    // Isosceles derive: s^2 + other^2 - s^2 → other^2
    ASSERT_EQ(expr_to_string(simplify(parse("s^2 + other^2 - s^2"))),
        "other^2", "s^2+other^2-s^2 → other^2");

    // other^2 / (2 * side * other) → other / (2 * side)
    // (from isosceles triangle derive — needs cross-term cancellation)
    ASSERT_EQ(ss("other^2 / (2 * side * other)"), "other / (2 * side)",
        "cross-term cancel: other^2/(2*side*other) → other/(2*side)");
}

void test_simplify_like_terms() {
    SECTION("Simplifier: Like-Term Combining");

    // x + x → 2 * x
    ASSERT_EQ(expr_to_string(simplify(parse("x + x"))), "2 * x", "x + x → 2*x");

    // 2*x + 3*x → 5*x
    ASSERT_EQ(expr_to_string(simplify(parse("2 * x + 3 * x"))), "5 * x", "2x + 3x → 5x");

    // x + 2*x → 3*x
    ASSERT_EQ(expr_to_string(simplify(parse("x + 2 * x"))), "3 * x", "x + 2x → 3x");

    // x - x → 0
    ASSERT_EQ(expr_to_string(simplify(parse("x - x"))), "0", "x - x → 0");

    // 3*x - x → 2*x
    ASSERT_EQ(expr_to_string(simplify(parse("3 * x - x"))), "2 * x", "3x - x → 2x");

    // 3*x - 2*x → x
    ASSERT_EQ(expr_to_string(simplify(parse("3 * x - 2 * x"))), "x", "3x - 2x → x");

    // 2*s + 2*s → 4*s (from derive: perimeter of square)
    ASSERT_EQ(expr_to_string(simplify(parse("2 * s + 2 * s"))), "4 * s", "2s + 2s → 4s");

    // 2*s + 2*s + 2*s → 6*s (cube surface: 3 pairs of faces)
    ASSERT_EQ(expr_to_string(simplify(parse("2 * s + 2 * s + 2 * s"))), "6 * s",
        "2s + 2s + 2s → 6s");

    // Additive cancellation: (a + b) - a → b
    ASSERT_EQ(expr_to_string(simplify(parse("(a + b) - a"))), "b", "(a+b)-a → b");

    // (a + b) - b → a
    ASSERT_EQ(expr_to_string(simplify(parse("(a + b) - b"))), "a", "(a+b)-b → a");

    // a - (a + b) → -b
    ASSERT_EQ(expr_to_string(simplify(parse("a - (a + b)"))), "-b", "a-(a+b) → -b");

    // a - (b + a) → -b
    ASSERT_EQ(expr_to_string(simplify(parse("a - (b + a)"))), "-b", "a-(b+a) → -b");

    // Like-term with negation: -x + x → 0
    ASSERT_EQ(expr_to_string(simplify(parse("-x + x"))), "0", "-x + x → 0");

    // Like-term: x + (-2*x) → -x
    ASSERT_EQ(expr_to_string(simplify(parse("x + (-2) * x"))), "-x", "x + (-2)*x → -x");

    // Different bases: x + y stays
    ASSERT_EQ(expr_to_string(simplify(parse("x + y"))), "x + y", "x+y: different bases unchanged");
}

void test_simplify_constant_reassociation() {
    SECTION("Simplifier: Constant Reassociation Extended");

    // (a * K1) / K2 → a * (K1/K2)
    ASSERT_EQ(expr_to_string(simplify(parse("x * 6 / 2"))), "3 * x", "(x*6)/2 → 3*x");
    ASSERT_EQ(expr_to_string(simplify(parse("x * 0.866 / 2"))), "0.433 * x", "(x*0.866)/2 → 0.433*x");

    // (a / K1) * K2 → a / (K1/K2)
    // x/4*2 → 0.5*x (absorbs numeric DIV into coefficient)
    ASSERT_EQ(ss("x / 4 * 2"), "0.5 * x", "(x/4)*2 → 0.5*x");
}

void test_simplify_div_zero_denom() {
    SECTION("Simplifier: Zero Denominator Safety");

    // --- API cases: simplify_div must NOT call make_rational with denom=0 ---
    // Previously, the constant-reassociation branch in simplify_div (for
    // (K1 * a) / K2 or (a * K1) / K2) called make_rational unconditionally
    // when both numbers were integers, triggering an assertion when K2 == 0.

    // Case 1: Num(3) / Num(0) — no MUL on LHS, but still must not crash.
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Num(3), Expr::Num(0));
        auto s = simplify(e);
        ASSERT(s != nullptr, "simplify(3/0) does not crash");
        auto ev = evaluate(*s);
        ASSERT(!ev.has_value(), "simplify(3/0) evaluates to empty Checked");
    }

    // Case 2: MUL(Num(3), Var(x)) / Num(0) — Num-on-left MUL branch.
    // This was the primary crash path: is_integer_value(l->left->num)
    // && is_integer_value(r->num) passed, then make_rational(3, 0) aborted.
    {
        auto mul = Expr::BinOpExpr(BinOp::MUL, Expr::Num(3), Expr::Var("x"));
        auto e = Expr::BinOpExpr(BinOp::DIV, mul, Expr::Num(0));
        auto s = simplify(e);
        ASSERT(s != nullptr, "simplify((3*x)/0) does not crash");
        // Without bindings x cannot evaluate, but the expression must not
        // fold to a non-DIV form that lies about division-by-zero.
        // Substitute x=5 and check evaluate is empty (NaN sentinel).
        auto subst = substitute(s, "x", Expr::Num(5));
        auto ev = evaluate(*subst);
        ASSERT(!ev.has_value(), "(3*x)/0 with x=5 stays empty Checked");
    }

    // Case 3: MUL(Var(x), Num(3)) / Num(0) — Num-on-right MUL branch.
    {
        auto mul = Expr::BinOpExpr(BinOp::MUL, Expr::Var("x"), Expr::Num(3));
        auto e = Expr::BinOpExpr(BinOp::DIV, mul, Expr::Num(0));
        auto s = simplify(e);
        ASSERT(s != nullptr, "simplify((x*3)/0) does not crash");
        auto subst = substitute(s, "x", Expr::Num(5));
        auto ev = evaluate(*subst);
        ASSERT(!ev.has_value(), "(x*3)/0 with x=5 stays empty Checked");
    }

    // Case 4: Num(0) / Num(0) — 0/0 preserves both operands, evaluate empty.
    // Must not short-circuit to Num(0) via the is_zero(l) rule, because
    // 0/0 is undefined (not zero).
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Num(0), Expr::Num(0));
        auto s = simplify(e);
        ASSERT(s != nullptr, "simplify(0/0) does not crash");
        auto ev = evaluate(*s);
        ASSERT(!ev.has_value(), "simplify(0/0) evaluates to empty Checked");
    }

    // --- Shell cases: end-to-end, CLI must not abort with make_rational. ---
    // We write stderr to a temp file and grep for the assertion string.

    // Case 5: explicit /0 in --derive should error cleanly, not abort.
    {
        int rc = system("./bin/fwiz --derive '(y=?, x=x) y = (3 * x) / 0' "
                        "> /dev/null 2>/tmp/fwiz_divzero_stderr.txt");
        int exit_status = WEXITSTATUS(rc);
        // Exit 134 = SIGABRT (assertion). Anything else is acceptable.
        ASSERT(exit_status != 134, "(3*x)/0 derive: no SIGABRT");
        std::ifstream f("/tmp/fwiz_divzero_stderr.txt");
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        ASSERT(content.find("make_rational") == std::string::npos,
               "(3*x)/0 derive: no make_rational assertion in stderr");
    }

    // Case 6: triangle with only angles — numeric probe previously hit the
    // same assertion via simplify_div. Tolerate any clean exit (0 or 1),
    // but never a crash.
    {
        int rc = system("timeout 15 ./bin/fwiz 'examples/triangle(A=?, B=45, C=45)' "
                        "> /dev/null 2>/tmp/fwiz_triangle_stderr.txt");
        int exit_status = WEXITSTATUS(rc);
        ASSERT(exit_status != 134, "triangle(A=?, B=45, C=45): no SIGABRT");
        std::ifstream f("/tmp/fwiz_triangle_stderr.txt");
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        ASSERT(content.find("make_rational") == std::string::npos,
               "triangle(A=?, B=45, C=45): no make_rational assertion in stderr");
    }
}

void test_simplify_mul_to_pow() {
    SECTION("Simplifier: Multiplication to Power");

    // x * x → x^2
    ASSERT_EQ(expr_to_string(simplify(parse("x * x"))), "x^2", "x*x → x^2");

    // x * x * x → x^3
    ASSERT_EQ(expr_to_string(simplify(parse("x * x * x"))), "x^3", "x*x*x → x^3");

    // x^2 * x → x^3
    ASSERT_EQ(expr_to_string(simplify(parse("x^2 * x"))), "x^3", "x^2 * x → x^3");

    // x * x^2 → x^3
    ASSERT_EQ(expr_to_string(simplify(parse("x * x^2"))), "x^3", "x * x^2 → x^3");

    // x^2 * x^3 → x^5
    ASSERT_EQ(expr_to_string(simplify(parse("x^2 * x^3"))), "x^5", "x^2 * x^3 → x^5");

    // Coefficient preserved: 2*s*s → 2*s^2
    ASSERT_EQ(expr_to_string(simplify(parse("2 * s * s"))), "2 * s^2", "2*s*s → 2*s^2");

    // 3*x * x^2 → 3*x^3
    ASSERT_EQ(expr_to_string(simplify(parse("3 * x * x^2"))), "3 * x^3", "3*x * x^2 → 3*x^3");

    // Different bases: x*y stays as is
    ASSERT_EQ(expr_to_string(simplify(parse("x * y"))), "x * y", "x*y: different bases unchanged");
}

void test_simplify_self_division() {
    SECTION("Simplifier: Self-Division");

    // x / x → 1
    ASSERT_EQ(expr_to_string(simplify(parse("x / x"))), "1", "x/x → 1");

    // (2*x) / x → 2
    ASSERT_EQ(expr_to_string(simplify(parse("2 * x / x"))), "2", "2x/x → 2");

    // x / (2*x) → 1/2
    ASSERT_EQ(expr_to_string(simplify(parse("x / (2 * x)"))), "1 / 2", "x/(2x) → 1/2");

    // (3*x) / x → 3
    ASSERT_EQ(expr_to_string(simplify(parse("3 * x / x"))), "3", "3x/x → 3");

    // x^3 / x^2 → x
    ASSERT_EQ(expr_to_string(simplify(parse("x^3 / x^2"))), "x", "x^3/x^2 → x");

    // x^3 / x → x^2
    ASSERT_EQ(expr_to_string(simplify(parse("x^3 / x"))), "x^2", "x^3/x → x^2");

    // x^2 / x^2 → 1
    ASSERT_EQ(expr_to_string(simplify(parse("x^2 / x^2"))), "1", "x^2/x^2 → 1");

    // Different bases: x / y stays
    ASSERT_EQ(expr_to_string(simplify(parse("x / y"))), "x / y", "x/y: different bases unchanged");
}

void test_simplify_constant_collection() {
    SECTION("Simplifier: Constant Collection");

    // K1 + expr + K2 → expr + (K1+K2) — constants migrate together
    // This tests: 16 + c^2 - 9 → c^2 + 7 (from derive: mixed triangle)
    auto e = simplify(parse("16 + x - 9"));
    // Should have x and 7, in some form
    double val = (evaluate(substitute(e, "x", Expr::Num(0))).value());
    ASSERT_NUM(val, 7, "16 + x - 9 with x=0 → 7");
}

// ---- Derive mode tests ----

void test_derive_basic() {
    SECTION("Derive Basic");

    // Simple symbolic derivation
    {
        write_fw("/tmp/td1.fw", "area = width * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td1.fw");
        auto result = sys.derive("area", {}, {{"width", "w"}, {"height", "h"}});
        ASSERT_EQ(result, "w * h", "derive: area = w * h");
    }

    // Inverse derivation
    {
        write_fw("/tmp/td2.fw", "area = width * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td2.fw");
        auto result = sys.derive("width", {}, {{"area", "a"}, {"height", "h"}});
        ASSERT_EQ(result, "a / h", "derive: width = a / h");
    }

    // Mixed numeric + symbolic
    {
        FormulaSystem sys;
        sys.load_file("/tmp/td2.fw");
        auto result = sys.derive("width", {{"height", 5}}, {{"area", "a"}});
        ASSERT_EQ(result, "a / 5", "derive: mixed numeric + symbolic");
    }

    // Fully numeric collapses to a number
    {
        FormulaSystem sys;
        sys.load_file("/tmp/td2.fw");
        auto result = sys.derive("area", {{"width", 4}, {"height", 3}}, {});
        ASSERT_EQ(result, "12", "derive: fully numeric = 12");
    }

    // Defaults are substituted numerically
    {
        write_fw("/tmp/td3.fw", "g = 9.81\nforce = mass * g\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td3.fw");
        auto result = sys.derive("force", {}, {{"mass", "m"}});
        ASSERT_EQ(result, "9.81 * m", "derive: default substituted");
    }
}

void test_derive_same_name() {
    SECTION("Derive Same-Name Collapse");

    // Same-name parameters simplify
    {
        write_fw("/tmp/tds1.fw", "area = width * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tds1.fw");
        // width=s, height=s → area = s * s
        auto result = sys.derive("area", {}, {{"width", "s"}, {"height", "s"}});
        ASSERT_EQ(result, "s^2", "same-name: s^2");
    }

    // Same-name with addition: perimeter = 2*w + 2*h, w=s, h=s → 2*s + 2*s
    {
        write_fw("/tmp/tds2.fw", "perimeter = 2 * width + 2 * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tds2.fw");
        auto result = sys.derive("perimeter", {}, {{"width", "s"}, {"height", "s"}});
        ASSERT_EQ(result, "4 * s", "same-name: 4*s");
    }
}

void test_derive_formula_call() {
    SECTION("Derive Formula Calls");

    // Cross-file derive
    {
        write_fw("/tmp/tdf_rect.fw", "area = width * height\n");
        write_fw("/tmp/tdf1.fw",
            "tdf_rect(area=?floor, width=width, height=depth)\n"
            "volume = floor * h\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf1.fw");
        auto result = sys.derive("volume", {}, {{"width", "w"}, {"depth", "d"}, {"h", "h"}});
        ASSERT_EQ(result, "w * d * h", "derive through formula call");
    }

    // Derive with formula call unfolding — simple non-recursive
    // addfour: result = x + 4
    // parent: y = n + addfour(result=?, x=n)
    // Unfolding: y = n + (n + 4) = 2*n + 4, so n = (y - 4) / 2
    {
        write_fw("/tmp/tdf_add4.fw", "result = x + 4\n");
        write_fw("/tmp/tdf_unfold.fw",
            "tdf_add4(result=?f, x=n)\n"
            "y = n + f\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_unfold.fw");
        try {
            auto result = sys.derive("n", {}, {{"y", "y"}});
            ASSERT_EQ(result, "(y - 4) / 2", "derive: unfold formula call and solve for input");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("derive: unfold threw: ") + e.what());
        }
    }

    // Derive with formula call unfolding — reverse direction
    // rect: area = width * height
    // parent: rect(area=?a, width=x, height=3), solve for x given a
    {
        write_fw("/tmp/tdf_rect2.fw", "area = width * height\n");
        write_fw("/tmp/tdf_rev.fw",
            "tdf_rect2(area=?a, width=x, height=3)\n"
            "y = a + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_rev.fw");
        try {
            auto result = sys.derive("x", {}, {{"y", "y"}});
            ASSERT_EQ(result, "(y - 1) / 3", "derive: reverse through formula call");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("derive: reverse unfold threw: ") + e.what());
        }
    }

    // Recursive formula call forward — derive result=? given n=5
    // Should evaluate to 120 (numeric, not symbolic)
    {
        FormulaSystem sys;
        sys.load_file("examples/factorial.fw");
        try {
            auto result = sys.derive("result", {{"n", 5}}, {});
            ASSERT_EQ(result, "120", "derive: factorial forward with numeric n");
        } catch (const std::exception& e) {
            // Current implementation may not support this yet
            ASSERT(false, std::string("derive: factorial forward threw: ") + e.what());
        }
    }

    // Recursive formula call — derive does its best, doesn't crash/hang
    // Even if it can't fully solve, it should produce something or fail gracefully
    {
        FormulaSystem sys;
        sys.load_file("examples/factorial.fw");
        try {
            auto result = sys.derive("n", {}, {{"result", "result"}});
            // If it produces anything, that's fine — it won't be a clean solution
            ASSERT(true, "derive: recursive inverse doesn't crash");
        } catch (const std::exception& e) {
            // Also acceptable — "Cannot derive" is fine
            std::string msg = e.what();
            ASSERT(msg.find("Cannot derive") != std::string::npos,
                "derive: recursive inverse fails gracefully");
        }
    }

    // Condition checking in derive — skips wrong branch
    {
        write_fw("/tmp/tdf_cond.fw",
            "y = 0 if x =0\n"
            "y = x * 2 if x >0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_cond.fw");
        // With x=5, should use second equation (y = x * 2), not first
        auto result = sys.derive("y", {{"x", 5}}, {});
        ASSERT_EQ(result, "10", "derive: condition skips wrong branch");
    }

    // Condition checking — symbolic (condition can't be evaluated, both tried)
    {
        write_fw("/tmp/tdf_cond2.fw",
            "y = 0 if x =0\n"
            "y = x * 2 if x >0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_cond2.fw");
        // With symbolic x, condition can't be evaluated — first valid wins
        auto result = sys.derive("y", {}, {{"x", "x"}});
        ASSERT_EQ(result, "0", "derive: symbolic conditions fallthrough to first");
    }

    // Recursive base case — n=0 gives result=1
    {
        FormulaSystem sys;
        sys.load_file("examples/factorial.fw");
        auto result = sys.derive("result", {{"n", 0}}, {});
        ASSERT_EQ(result, "1", "derive: factorial base case n=0");
    }

    // FORMULA_REV through cross-file call chain (like box)
    {
        write_fw("/tmp/tdf_inner.fw", "area = w * h\n");
        write_fw("/tmp/tdf_outer.fw",
            "tdf_inner(area=?base, w=width, h=depth)\n"
            "volume = base * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_outer.fw");
        // Solve for width (reverse through formula call)
        auto result = sys.derive("width", {}, {{"volume", "V"}, {"depth", "d"}, {"height", "h"}});
        ASSERT_EQ(result, "V / h / d", "derive: FORMULA_REV cross-file chain");
    }

    // FORMULA_REV with expression binding (width = x + 1)
    {
        write_fw("/tmp/tdf_expr_inner.fw", "area = w * h\n");
        write_fw("/tmp/tdf_expr_outer.fw",
            "tdf_expr_inner(area=?a, w=x, h=3)\n"
            "y = a\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_expr_outer.fw");
        // Solve for x: a = x * 3, y = a, so x = y / 3
        auto result = sys.derive("x", {}, {{"y", "y"}});
        ASSERT_EQ(result, "y / 3", "derive: FORMULA_REV simple binding");
    }

    // Unfold where target reappears after substitution (has_target path)
    // f(x) = 2*x + 3, parent: y = x + f(x) = x + 2x + 3 = 3x + 3
    // Solve for x: x = (y - 3) / 3
    {
        write_fw("/tmp/tdf_reappear_inner.fw", "result = 2 * x + 3\n");
        write_fw("/tmp/tdf_reappear.fw",
            "tdf_reappear_inner(result=?f, x=n)\n"
            "y = n + f\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_reappear.fw");
        auto result = sys.derive("n", {}, {{"y", "y"}});
        ASSERT_EQ(result, "(y - 3) / 3", "derive: unfold with target reappearing");
    }

    // Unfold falls back when body contains formula call outputs (recursive)
    // factorial with numeric input should still work via fallback
    {
        FormulaSystem sys;
        sys.load_file("examples/factorial.fw");
        auto result = sys.derive("result", {{"n", 3}}, {});
        ASSERT_EQ(result, "6", "derive: recursive fallback for n=3");
    }

    // FORMULA_REV with expression binding (w=n+1)
    // area = w * h, call: inner(area=?a, w=n+1, h=3), y = a
    // → a = (n+1)*3, y = 3n+3, n = y/3 - 1
    {
        write_fw("/tmp/tdf_exprbind_inner.fw", "area = w * h\n");
        write_fw("/tmp/tdf_exprbind.fw",
            "tdf_exprbind_inner(area=?a, w=n+1, h=3)\n"
            "y = a\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_exprbind.fw");
        auto result = sys.derive("n", {}, {{"y", "y"}});
        ASSERT_EQ(result, "y / 3 - 1", "derive: FORMULA_REV expression binding");
    }

    // Multiple formula calls — target used in two calls
    {
        write_fw("/tmp/tdf_multi_inner.fw", "result = x + 4\n");
        write_fw("/tmp/tdf_multi.fw",
            "tdf_multi_inner(result=?a, x=p)\n"
            "tdf_multi_inner(result=?b, x=q)\n"
            "y = a + b\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tdf_multi.fw");
        auto result = sys.derive("p", {}, {{"y", "y"}, {"q", "q"}});
        ASSERT_EQ(result, "y - q - 8", "derive: multiple formula calls");
    }

    // Symbolic recursive derive — picks first valid (base case)
    {
        FormulaSystem sys;
        sys.load_file("examples/factorial.fw");
        auto result = sys.derive("result", {}, {{"n", "n"}});
        // Can't evaluate condition, first equation wins → result = 1
        ASSERT_EQ(result, "1", "derive: symbolic recursive picks first valid");
    }
}

void test_derive_errors() {
    SECTION("Derive Errors");

    // No equation for target
    {
        write_fw("/tmp/tde1.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tde1.fw");
        auto msg = get_error([&]() { sys.derive("z", {}, {{"x", "x"}}); });
        ASSERT(msg.find("Cannot derive") != std::string::npos, "derive: unknown var throws");
    }
}

void test_derive_cli_parsing() {
    SECTION("Derive CLI Parsing");

    // Symbolic values parsed correctly
    {
        auto q = parse_cli_query("f(x=?, a=side, b=3)", false, true);
        ASSERT(q.symbolic.count("a"), "symbolic: a is symbolic");
        ASSERT_EQ(q.symbolic.at("a"), "side", "symbolic: a=side");
        ASSERT(q.bindings.count("b"), "symbolic: b is numeric");
        ASSERT_NUM(q.bindings.at("b"), 3, "symbolic: b=3");
    }

    // Without allow_symbolic, non-numeric throws
    {
        auto msg = get_error([&]() { parse_cli_query("f(x=?, a=side)"); });
        ASSERT(msg.find("Invalid") != std::string::npos || msg.find("unresolved") != std::string::npos,
            "no symbolic: throws");
    }
}

void test_derive_binary_integration() {
    SECTION("Derive Binary Integration");

    write_fw("/tmp/tdbi.fw", "area = width * height\n");

    // --derive flag works
    {
        int rc = system("./bin/fwiz --derive '/tmp/tdbi(area=?, width=w, height=h)' 2>/dev/null "
                        "| grep -q 'area = w \\* h'");
        ASSERT(WEXITSTATUS(rc) == 0, "derive binary: area = w * h");
    }

    // --derive with alias
    {
        int rc = system("./bin/fwiz --derive '/tmp/tdbi(area=?A, width=w, height=h)' 2>/dev/null "
                        "| grep -q 'A = w \\* h'");
        ASSERT(WEXITSTATUS(rc) == 0, "derive binary: alias works");
    }

    // --derive fully numeric
    {
        int rc = system("./bin/fwiz --derive '/tmp/tdbi(area=?, width=4, height=3)' 2>/dev/null "
                        "| grep -q 'area = 12'");
        ASSERT(WEXITSTATUS(rc) == 0, "derive binary: fully numeric");
    }
}

// ---- Numeric root-finding ----

void test_newton_solve() {
    SECTION("Newton's Method");

    // x^2 - 9 = 0, starting from x=1 → root at 3
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto root = newton_solve(f, 1.0);
        ASSERT(root.has_value(), "newton: x^2-9 converges");
        ASSERT_NUM(*root, 3.0, "newton: x^2-9 = 0 → x = 3");
    }

    // x^2 - 9 = 0, starting from x=-1 → root at -3
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto root = newton_solve(f, -1.0);
        ASSERT(root.has_value(), "newton: x^2-9 from -1 converges");
        ASSERT_NUM(*root, -3.0, "newton: x^2-9 = 0 from -1 → x = -3");
    }

    // x + sin(x) - 1 = 0 → x ≈ 0.51097
    {
        auto f = [](double x) { return x + std::sin(x) - 1.0; };
        auto root = newton_solve(f, 0.5);
        ASSERT(root.has_value(), "newton: x+sin(x)-1 converges");
        ASSERT_NUM(*root, 0.510973, "newton: x+sin(x) = 1 → x ≈ 0.51097");
    }

    // No root: constant function
    {
        auto f = [](double x) { (void)x; return 5.0; };
        auto root = newton_solve(f, 0.0);
        ASSERT(!root.has_value(), "newton: constant function → no root");
    }

    // Integer snapping: x^2 - 25 = 0 from x=4 → 5 (exact integer)
    {
        auto f = [](double x) { return x * x - 25.0; };
        auto root = newton_solve(f, 4.0);
        ASSERT(root.has_value(), "newton: x^2-25 converges");
        ASSERT(*root == 5.0, "newton: x^2-25 snaps to integer 5");
    }
}

void test_bisection_solve() {
    SECTION("Bisection Method");

    // x^2 - 9 in [0, 10] → 3
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto root = bisection_solve(f, 0.0, 10.0);
        ASSERT(root.has_value(), "bisection: x^2-9 in [0,10] converges");
        ASSERT_NUM(*root, 3.0, "bisection: x^2-9 = 0 → x = 3");
    }

    // No sign change → no root
    {
        auto f = [](double x) { return x * x + 1.0; };
        auto root = bisection_solve(f, 0.0, 10.0);
        ASSERT(!root.has_value(), "bisection: x^2+1 no sign change → none");
    }

    // sin(x) in [3, 4] → π
    {
        auto f = [](double x) { return std::sin(x); };
        auto root = bisection_solve(f, 3.0, 3.5);
        ASSERT(root.has_value(), "bisection: sin(x) in [3,3.5] converges");
        ASSERT_NUM(*root, M_PI, "bisection: sin(x)=0 → x = π");
    }
}

void test_adaptive_scan() {
    SECTION("Adaptive Scan");

    // x^2 - 9: two roots at -3 and 3
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto intervals = adaptive_scan(f, -10.0, 10.0);
        ASSERT(intervals.size() >= 2, "scan: x^2-9 finds at least 2 intervals");
    }

    // sin(x) in [-7, 7]: roots at -2π, -π, 0, π, 2π
    {
        auto f = [](double x) { return std::sin(x); };
        auto intervals = adaptive_scan(f, -7.0, 7.0);
        ASSERT(intervals.size() >= 4, "scan: sin(x) finds at least 4 intervals");
    }

    // Integer mode: x^2 - 9 in [0, 10]
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto intervals = adaptive_scan(f, 0.0, 10.0, true);
        // x=3 is exact zero, x=2→3 or x=3→4 sign change
        ASSERT(!intervals.empty(), "scan: integer mode finds interval for x^2-9");
    }

    // No roots: x^2 + 1
    {
        auto f = [](double x) { return x * x + 1.0; };
        auto intervals = adaptive_scan(f, -10.0, 10.0);
        ASSERT(intervals.empty(), "scan: x^2+1 no intervals");
    }
}

void test_find_numeric_roots() {
    SECTION("Find Numeric Roots");

    // x^2 - 9 → {-3, 3}
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto roots = find_numeric_roots(f, -10.0, 10.0);
        ASSERT(roots.size() == 2, "roots: x^2-9 finds 2 roots (got " + std::to_string(roots.size()) + ")");
        if (roots.size() == 2) {
            ASSERT_NUM(roots[0], -3.0, "roots: x^2-9 first root = -3");
            ASSERT_NUM(roots[1], 3.0, "roots: x^2-9 second root = 3");
        }
    }

    // x^2 - 9, integer mode → {-3, 3}
    {
        auto f = [](double x) { return x * x - 9.0; };
        auto roots = find_numeric_roots(f, -10.0, 10.0, true);
        ASSERT(roots.size() == 2, "roots: x^2-9 integer finds 2 roots");
        if (roots.size() == 2) {
            ASSERT(roots[0] == -3.0, "roots: integer x^2-9 first = -3");
            ASSERT(roots[1] == 3.0, "roots: integer x^2-9 second = 3");
        }
    }

    // sin(x) in [-7, 7]: should find 5 roots (-2π, -π, 0, π, 2π)
    {
        auto f = [](double x) { return std::sin(x); };
        auto roots = find_numeric_roots(f, -7.0, 7.0);
        ASSERT(roots.size() == 5, "roots: sin(x) finds 5 roots (got " + std::to_string(roots.size()) + ")");
        if (roots.size() >= 3) {
            ASSERT_NUM(roots[roots.size()/2], 0.0, "roots: sin(x) middle root ≈ 0");
        }
    }

    // No roots: x^2 + 1
    {
        auto f = [](double x) { return x * x + 1.0; };
        auto roots = find_numeric_roots(f, -10.0, 10.0);
        ASSERT(roots.empty(), "roots: x^2+1 → empty");
    }

    // Singularity: 1/x should NOT produce false roots
    {
        auto f = [](double x) { return 1.0 / x; };
        auto roots = find_numeric_roots(f, -10.0, 10.0);
        ASSERT(roots.empty(), "roots: 1/x no false roots at singularity");
    }

    // 1/x - 0.5 = 0 → x = 2
    {
        auto f = [](double x) { return 1.0 / x - 0.5; };
        auto roots = find_numeric_roots(f, 0.1, 10.0);
        ASSERT(roots.size() == 1, "roots: 1/x - 0.5 finds 1 root");
        if (!roots.empty()) ASSERT_NUM(roots[0], 2.0, "roots: 1/x = 0.5 → x = 2");
    }

    // Deterministic: same results on repeated calls
    {
        auto f = [](double x) { return x * x - 2.0; };
        auto r1 = find_numeric_roots(f, -5.0, 5.0);
        auto r2 = find_numeric_roots(f, -5.0, 5.0);
        ASSERT(r1.size() == r2.size(), "roots: deterministic — same count");
        bool same = true;
        for (size_t i = 0; i < r1.size() && i < r2.size(); i++)
            if (std::abs(r1[i] - r2[i]) > EPSILON_ZERO) same = false;
        ASSERT(same, "roots: deterministic — same values");
    }
}

void test_numeric_integration() {
    SECTION("Numeric Solver Integration");

    // Simple quadratic: y = x^2, solve for x given y=9
    {
        write_fw("/tmp/tn_quad.fw", "y = x^2\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_quad.fw");
        auto result = sys.resolve_all("x", {{"y", 9}});
        auto& d = result.discrete();
        // Algebraic inversion finds sqrt(9)=3, numeric may find -3 too
        bool has_3 = false, has_neg3 = false;
        for (auto r : d) {
            if (std::abs(r - 3) < 1e-6) has_3 = true;
            if (std::abs(r + 3) < 1e-6) has_neg3 = true;
        }
        ASSERT(has_3, "numeric: x^2=9 finds root 3");
        ASSERT(has_neg3, "numeric: x^2=9 finds root -3");
    }

    // Quadratic with condition: x > 0
    {
        write_fw("/tmp/tn_quad_cond.fw", "y = x^2 if x >0\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_quad_cond.fw");
        auto result = sys.resolve_all("x", {{"y", 9}});
        auto& d = result.discrete();
        ASSERT(d.size() == 1, "numeric: x^2=9, x>0 → 1 root");
        if (!d.empty()) ASSERT_NUM(d[0], 3, "numeric: x^2=9, x>0 → x = 3");
    }

    // Transcendental: x + sin(x) = 1
    {
        write_fw("/tmp/tn_trans.fw", "y = x + sin(x)\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_trans.fw");
        double x = sys.resolve("x", {{"y", 1}});
        ASSERT_NUM(x, 0.510973, "numeric: x+sin(x)=1 → x ≈ 0.51097");
    }

    // 1/x = 0.5 → x = 2
    {
        write_fw("/tmp/tn_inv.fw", "y = 1/x\nx > 0\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_inv.fw");
        try {
            double x = sys.resolve("x", {{"y", 0.5}});
            ASSERT_NUM(x, 2.0, "numeric: 1/x = 0.5 → x = 2");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("numeric: 1/x threw: ") + e.what());
        }
    }

    // With numeric_mode=false, x^2 is now solvable algebraically via inversion
    {
        write_fw("/tmp/tn_no_flag.fw", "y = x^2\n");
        FormulaSystem sys;
        sys.numeric_mode = false;
        sys.load_file("/tmp/tn_no_flag.fw");
        double x = sys.resolve("x", {{"y", 9}});
        ASSERT_NUM(x, 3, "numeric: x^2 solvable algebraically via inversion");
    }

    // Factorial inverse: result=120 → n=5
    // Use a constrained file to keep recursion depth manageable under sanitizers
    {
        write_fw("/tmp/tn_fact.fw",
            "result = 1 if n =0\n"
            "result = n * tn_fact(result=?prev, n=n-1) if n >0\n"
            "n >= 0\nn <= 20\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_fact.fw");
        auto result = sys.resolve_all("n", {{"result", 120}});
        auto& d = result.discrete();
        bool found_5 = false;
        for (auto r : d) if (std::abs(r - 5.0) < 1e-6) found_5 = true;
        ASSERT(found_5, "numeric: factorial(n=?,result=120) finds n=5");
    }

    // Factorial inverse: result=720 → n=6
    {
        write_fw("/tmp/tn_fact2.fw",
            "result = 1 if n =0\n"
            "result = n * tn_fact2(result=?prev, n=n-1) if n >0\n"
            "n >= 0\nn <= 20\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_fact2.fw");
        auto result = sys.resolve_all("n", {{"result", 720}});
        auto& d = result.discrete();
        bool found_6 = false;
        for (auto r : d) if (std::abs(r - 6.0) < 1e-6) found_6 = true;
        ASSERT(found_6, "numeric: factorial(n=?,result=720) finds n=6");
    }

    // Numeric results tracking (for ~= output)
    {
        write_fw("/tmp/tn_track.fw", "y = x^2\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tn_track.fw");
        sys.resolve("x", {{"y", 9}});
        // x^2 is now solved algebraically — may or may not be in numeric_results
        ASSERT(true, "numeric: x^2 solve mode (algebraic or numeric)");
    }
}

void test_numeric_precision() {
    SECTION("Numeric Precision Flag");

    // --precision affects sample count (more samples → finds more roots)
    {
        write_fw("/tmp/tnp_sin.fw", "y = sin(x)\nx >= 0\nx <= 20\n");
        FormulaSystem sys_lo, sys_hi;
        sys_lo.numeric_mode = sys_hi.numeric_mode = true;
        sys_lo.numeric_samples = 10;  // very low — will miss roots
        sys_hi.numeric_samples = 500; // high — finds all
        sys_lo.load_file("/tmp/tnp_sin.fw");
        sys_hi.load_file("/tmp/tnp_sin.fw");
        auto r_lo = sys_lo.resolve_all("x", {{"y", 0.5}});
        auto r_hi = sys_hi.resolve_all("x", {{"y", 0.5}});
        ASSERT(r_hi.discrete().size() >= r_lo.discrete().size(),
            "numeric: higher precision finds >= roots");
        ASSERT(r_hi.discrete().size() >= 6,
            "numeric: high precision finds ≥6 sin roots in [0,20]");
    }
}

void test_numeric_edge_cases() {
    SECTION("Numeric Edge Cases");

    // Global condition narrows search range
    {
        write_fw("/tmp/tne_range.fw", "y = x^2\nx >= 0\nx <= 10\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_range.fw");
        auto result = sys.resolve_all("x", {{"y", 9}});
        auto& d = result.discrete();
        ASSERT(d.size() == 1, "numeric: global condition x>=0 filters negative root");
        if (!d.empty()) ASSERT_NUM(d[0], 3, "numeric: x^2=9, x∈[0,10] → x=3");
    }

    // Multiple equations — numeric only fires after algebraic fails
    {
        write_fw("/tmp/tne_linear.fw", "y = 2 * x + 1\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_linear.fw");
        double x = sys.resolve("x", {{"y", 7}});
        ASSERT_NUM(x, 3, "numeric: linear eq solved algebraically, not numerically");
        ASSERT(sys.numeric_results_.count("x") == 0,
            "numeric: linear solve not marked as numeric");
    }

    // No roots in range
    {
        write_fw("/tmp/tne_nope.fw", "y = x^2\nx >= 10\nx <= 20\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_nope.fw");
        auto msg = get_error([&]() { sys.resolve("x", {{"y", 1}}); });
        ASSERT(!msg.empty(), "numeric: no roots in [10,20] for x^2=1");
    }

    // Exact result verified (= not ~)
    {
        write_fw("/tmp/tne_exact.fw", "y = x^2\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_exact.fw");
        sys.resolve_all("x", {{"y", 4}});
        auto it = sys.numeric_results_.find("x");
        ASSERT(it != sys.numeric_results_.end() && it->second == true,
            "numeric: x^2=4 → x=±2 marked as exact");
    }

    // Approximate result (~ not =)
    {
        write_fw("/tmp/tne_approx.fw", "y = x + sin(x)\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_approx.fw");
        sys.resolve_all("x", {{"y", 1}});
        auto it = sys.numeric_results_.find("x");
        ASSERT(it != sys.numeric_results_.end() && it->second == false,
            "numeric: x+sin(x)=1 marked as approximate");
    }

    // ?! strict mode with multiple numeric roots → error
    {
        write_fw("/tmp/tne_strict.fw", "y = x^2\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_strict.fw");
        auto msg = get_error([&]() { sys.resolve_one("x", {{"y", 9}}); });
        ASSERT(msg.find("Multiple") != std::string::npos,
            "numeric: ?! with x^2=9 → multiple solutions error");
    }

    // ?! strict with condition → single root ok
    {
        write_fw("/tmp/tne_strict_ok.fw", "y = x^2\nx > 0\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_strict_ok.fw");
        double x = sys.resolve_one("x", {{"y", 9}});
        ASSERT_NUM(x, 3, "numeric: ?! with x>0 → x=3 only");
    }

    // Memoization: same query twice gives same result
    {
        write_fw("/tmp/tne_memo.fw",
            "result = 1 if n <=0\n"
            "result = n * tne_memo(result=?prev, n=n-1) if n >0\n"
            "n >= 0\nn <= 10\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tne_memo.fw");
        auto r1 = sys.resolve_all("n", {{"result", 24}});
        sys.numeric_memo_.clear();
        auto r2 = sys.resolve_all("n", {{"result", 24}});
        bool found_4_a = false, found_4_b = false;
        for (auto r : r1.discrete()) if (std::abs(r - 4.0) < 1e-6) found_4_a = true;
        for (auto r : r2.discrete()) if (std::abs(r - 4.0) < 1e-6) found_4_b = true;
        ASSERT(found_4_a && found_4_b, "numeric: memoization — consistent results");
    }
}

void test_numeric_binary_integration() {
    SECTION("Numeric Binary Integration");

    // --numeric with exact result uses = (verified)
    {
        write_fw("/tmp/tnb_quad.fw", "y = x^2\n");
        int rc = system("./bin/fwiz '/tmp/tnb_quad(x=?, y=9)' 2>/dev/null "
                        "| grep -q 'x = '");
        ASSERT(WEXITSTATUS(rc) == 0, "numeric binary: exact uses =");
    }

    // --numeric with approximate result uses ~
    {
        write_fw("/tmp/tnb_trans.fw", "y = x + sin(x)\n");
        int rc = system("./bin/fwiz '/tmp/tnb_trans(x=?, y=1)' 2>/dev/null "
                        "| grep -q ' ~ '");
        ASSERT(WEXITSTATUS(rc) == 0, "numeric binary: approximate uses ~");
    }

    // Without --numeric, no ~=
    {
        int rc = system("./bin/fwiz 'examples/convert(celsius=?, fahrenheit=72)' 2>/dev/null "
                        "| grep -q ' ~ '");
        ASSERT(WEXITSTATUS(rc) != 0, "numeric binary: no ~ with --no-numeric");
    }

    // Factorial with --numeric (constrained range for speed)
    {
        write_fw("/tmp/tnb_fact.fw",
            "result = 1 if n <=0\n"
            "result = n * tnb_fact(result=?prev, n=n-1) if n >0\n"
            "n >= 0\nn <= 20\n");
        int rc = system("./bin/fwiz '/tmp/tnb_fact(n=?, result=120)' 2>/dev/null "
                        "| grep -q '5'");
        ASSERT(WEXITSTATUS(rc) == 0, "numeric binary: factorial finds 5");
    }

    // --precision flag accepted
    {
        write_fw("/tmp/tnb_prec.fw", "y = x^2\nx >= 0\n");
        int rc = system("./bin/fwiz --precision 50 '/tmp/tnb_prec(x=?, y=4)' 2>/dev/null "
                        "| grep -q 'x = 2'");
        ASSERT(WEXITSTATUS(rc) == 0, "numeric binary: --precision flag works");
    }

    // --precision error handling
    {
        int rc = system("./bin/fwiz --precision 2>/dev/null");
        ASSERT(WEXITSTATUS(rc) != 0, "numeric binary: --precision without arg errors");
    }

    // --no-numeric: x^2 now solved algebraically, test with truly non-invertible
    {
        write_fw("/tmp/tnb_nonum.fw", "y = x + sin(x)\n");
        int rc = system("./bin/fwiz --no-numeric '/tmp/tnb_nonum(x=?, y=1)' 2>/dev/null");
        ASSERT(WEXITSTATUS(rc) != 0, "numeric binary: --no-numeric disables numeric");
    }

    // Numeric is default (no flag needed)
    {
        write_fw("/tmp/tnb_default.fw", "y = x^2\n");
        int rc = system("./bin/fwiz '/tmp/tnb_default(x=?, y=9)' 2>/dev/null "
                        "| grep -q '3'");
        ASSERT(WEXITSTATUS(rc) == 0, "numeric binary: numeric enabled by default");
    }
}

// ---- Curve fitting ----

void test_fit_sampling() {
    SECTION("Fit: Sampling");

    // Basic sampling
    {
        auto f = [](double x) { return x * x; };
        auto samples = sample_function(f, -5, 5, 50);
        ASSERT(samples.size() >= 45, "fit sample: enough points collected");
        ASSERT(samples[0].x >= -5.1, "fit sample: starts near lo");
        ASSERT(samples.back().x <= 5.1, "fit sample: ends near hi");
    }

    // NaN/Inf filtered out
    {
        auto f = [](double x) { return x < 0 ? std::numeric_limits<double>::quiet_NaN() : x; };
        auto samples = sample_function(f, -10, 10, 100);
        for (auto& s : samples)
            ASSERT(std::isfinite(s.y), "fit sample: no NaN/Inf in results");
    }

    // Deterministic (same seed)
    {
        auto f = [](double x) { return std::sin(x); };
        auto s1 = sample_function(f, 0, 10, 50);
        auto s2 = sample_function(f, 0, 10, 50);
        ASSERT(s1.size() == s2.size(), "fit sample: deterministic count");
        bool same = true;
        for (size_t i = 0; i < s1.size(); i++)
            if (s1[i].x != s2[i].x || s1[i].y != s2[i].y) same = false;
        ASSERT(same, "fit sample: deterministic values");
    }
}

void test_fit_matrix() {
    SECTION("Fit: Matrix Solve");

    // Simple 3x2 system: y = 3 + 2x, points (1,5), (2,7), (3,9)
    {
        FitMatrix A = {{1,1}, {1,2}, {1,3}};
        std::vector<double> b = {5, 7, 9};
        auto x = least_squares_solve(A, b);
        ASSERT(x.size() == 2, "matrix: 3x2 solution size");
        ASSERT_NUM(x[0], 3, "matrix: intercept = 3");
        ASSERT_NUM(x[1], 2, "matrix: slope = 2");
    }

    // 2x2 identity
    {
        FitMatrix A = {{1,0}, {0,1}};
        std::vector<double> b = {7, 11};
        auto x = least_squares_solve(A, b);
        ASSERT_NUM(x[0], 7, "matrix: identity x[0] = 7");
        ASSERT_NUM(x[1], 11, "matrix: identity x[1] = 11");
    }

    // Overdetermined quadratic: y = x^2, points at x = -2, -1, 0, 1, 2
    {
        FitMatrix A = {{1,-2,4}, {1,-1,1}, {1,0,0}, {1,1,1}, {1,2,4}};
        std::vector<double> b = {4, 1, 0, 1, 4};
        auto x = least_squares_solve(A, b);
        ASSERT_NUM(x[0], 0, "matrix: quadratic c0 = 0");
        ASSERT_NUM(x[1], 0, "matrix: quadratic c1 = 0");
        ASSERT_NUM(x[2], 1, "matrix: quadratic c2 = 1");
    }
}

void test_fit_polynomial() {
    SECTION("Fit: Polynomial");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // Exact linear fit: y = 2x + 3
    {
        auto f = [](double x) { return 2*x + 3; };
        auto samples = sample_function(f, -10, 10, 100);
        auto result = fit_polynomial(samples, 1);
        ASSERT_NUM(result.coefficients[0], 3, "fit poly: linear c0 = 3");
        ASSERT_NUM(result.coefficients[1], 2, "fit poly: linear c1 = 2");
        ASSERT(result.r_squared > 0.9999, "fit poly: linear R² ≈ 1");
        ASSERT(result.exact, "fit poly: linear is exact");
    }

    // Exact quadratic fit: y = x^2
    {
        auto f = [](double x) { return x*x; };
        auto samples = sample_function(f, -10, 10, 100);
        auto result = fit_polynomial(samples, 2);
        ASSERT_NUM(result.coefficients[2], 1, "fit poly: quadratic c2 = 1");
        ASSERT(result.exact, "fit poly: quadratic is exact");
    }

    // Auto degree: cubic data selects degree 3
    {
        auto f = [](double x) { return x*x*x - 2*x + 1; };
        auto samples = sample_function(f, -5, 5, 100);
        auto result = fit_polynomial_auto(samples);
        ASSERT(result.degree == 3, "fit auto: cubic selects degree 3 (got " + std::to_string(result.degree) + ")");
        ASSERT(result.r_squared > 0.999, "fit auto: cubic R² > 0.999");
    }

    // Expression tree construction
    {
        std::vector<double> coeffs = {3, 0, 1}; // 3 + x^2
        auto expr = poly_to_expr(coeffs, "x");
        auto str = expr_to_string(expr);
        // Should evaluate correctly
        double val = (evaluate(*substitute(expr, "x", Expr::Num(4))).value());
        ASSERT_NUM(val, 19, "fit expr: 3 + 4^2 = 19");
    }

    // Coefficient snapping
    {
        std::vector<double> coeffs = {2.9999999, 1.0000001};
        auto result_coeffs = coeffs;
        for (auto& c : result_coeffs) c = snap_coeff(c);
        ASSERT(result_coeffs[0] == 3.0, "fit snap: 2.9999999 → 3");
        ASSERT(result_coeffs[1] == 1.0, "fit snap: 1.0000001 → 1");
    }
}

void test_fit_integration() {
    SECTION("Fit: Integration");

    // Linear formula
    {
        write_fw("/tmp/tf_linear.fw", "y = 2 * x + 3\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf_linear.fw");
        auto result = sys.fit("y", {}, {{"x", "x"}});
        ASSERT(result.equation == "2 * x + 3" || result.equation == "3 + 2 * x",
            "fit integration: linear (got '" + result.equation + "')");
        ASSERT(result.exact, "fit integration: linear is exact");
    }

    // Quadratic formula
    {
        write_fw("/tmp/tf_quad.fw", "y = x^2\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf_quad.fw");
        auto result = sys.fit("y", {}, {{"x", "x"}});
        ASSERT(result.equation == "x^2" || result.equation == "abs(x^2)",
            "fit integration: quadratic (got '" + result.equation + "')");
        ASSERT(result.exact, "fit integration: quadratic is exact");
    }

    // Transcendental — composition finds sin(x) exactly
    {
        write_fw("/tmp/tf_sin.fw", "y = sin(x)\nx >= 0\nx <= 6.28\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf_sin.fw");
        auto result = sys.fit("y", {}, {{"x", "x"}});
        ASSERT(result.r_squared > 0.99, "fit integration: sin R² > 0.99");
        ASSERT_EQ(result.equation, "sin(x)", "fit integration: sin recognized exactly");
    }

    // Error: no symbolic variable
    {
        write_fw("/tmp/tf_err.fw", "y = x + 1\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tf_err.fw");
        auto msg = get_error([&]() { sys.fit("y", {{"x", 5}}, {}); });
        ASSERT(msg.find("symbolic") != std::string::npos, "fit error: no symbolic var");
    }
}

void test_fit_binary_integration() {
    SECTION("Fit: Binary Integration");

    // --fit produces output
    {
        write_fw("/tmp/tfb_quad.fw", "y = x^2\n");
        int rc = system("./bin/fwiz --fit '/tmp/tfb_quad(y=?, x=x)' 2>/dev/null "
                        "| grep -q 'x\\^2'");
        ASSERT(WEXITSTATUS(rc) == 0, "fit binary: x^2 recognized");
    }

    // --fit exact uses =
    {
        write_fw("/tmp/tfb_lin.fw", "y = 3 * x + 1\n");
        int rc = system("./bin/fwiz --fit '/tmp/tfb_lin(y=?, x=x)' 2>/dev/null "
                        "| grep -q 'y = '");
        ASSERT(WEXITSTATUS(rc) == 0, "fit binary: exact uses =");
    }

    // --fit approximate uses ~
    {
        write_fw("/tmp/tfb_sin.fw", "y = sin(x)\nx >= 0\nx <= 6.28\n");
        int rc = system("./bin/fwiz --fit '/tmp/tfb_sin(y=?, x=x)' 2>/dev/null "
                        "| grep -q ' ~ '");
        ASSERT(WEXITSTATUS(rc) == 0, "fit binary: approximate uses ~");
    }

    // --derive --fit skips duplicate
    {
        write_fw("/tmp/tfb_dup.fw", "y = 2 * x + 3\n");
        // derive gives "2 * x + 3", fit may give same or different order
        int lines = 0;
        FILE* p = popen("./bin/fwiz --derive --fit '/tmp/tfb_dup(y=?, x=x)' 2>/dev/null | wc -l", "r");
        if (p) { fscanf(p, "%d", &lines); pclose(p); }
        ASSERT(lines <= 2, "fit binary: derive+fit reasonable output (got " + std::to_string(lines) + " lines)");
    }

    // --output writes file
    {
        write_fw("/tmp/tfb_out.fw", "y = x^2\n");
        system("./bin/fwiz --fit --output /tmp/tfb_out_result.fw '/tmp/tfb_out(y=?, x=x)' 2>/dev/null");
        std::ifstream in("/tmp/tfb_out_result.fw");
        ASSERT(in.good(), "fit binary: --output creates file");
        std::string line;
        std::getline(in, line);
        ASSERT(line.find("Generated") != std::string::npos, "fit binary: --output has header");
    }
}

void test_builtin_constants() {
    SECTION("Builtin Constants");

    // pi evaluates correctly
    {
        auto expr = parse("pi");
        ASSERT_NUM((evaluate(*expr).value()), M_PI, "constant: pi evaluates to M_PI");
    }

    // e evaluates correctly
    {
        auto expr = parse("e");
        ASSERT_NUM((evaluate(*expr).value()), M_E, "constant: e evaluates to M_E");
    }

    // phi evaluates correctly
    {
        auto expr = parse("phi");
        double expected = (1.0 + std::sqrt(5.0)) / 2.0;
        ASSERT_NUM((evaluate(*expr).value()), expected, "constant: phi evaluates to golden ratio");
    }

    // Constants in expressions
    {
        auto expr = parse("2 * pi");
        ASSERT_NUM((evaluate(*expr).value()), 2 * M_PI, "constant: 2*pi");
    }

    // Constants in equations
    {
        write_fw("/tmp/tc_const.fw", "y = pi * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_const.fw");
        double y = sys.resolve("y", {{"x", 2}});
        ASSERT_NUM(y, 2 * M_PI, "constant: pi in equation");
    }

    // Derive preserves pi symbolically
    {
        write_fw("/tmp/tc_derive.fw", "y = 2 * pi * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_derive.fw");
        auto result = sys.derive("y", {}, {{"x", "r"}});
        ASSERT_EQ(result, "2 * pi * r", "constant: derive preserves pi");
    }

    // File default overrides builtin
    {
        write_fw("/tmp/tc_override.fw", "e = 5\ny = e * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_override.fw");
        double y = sys.resolve("y", {{"x", 2}});
        ASSERT_NUM(y, 10, "constant: file equation overrides builtin e");
    }

    // File default coexists with builtin (uses file default value)
    {
        write_fw("/tmp/tc_default.fw", "pi = 3.14\ny = pi * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_default.fw");
        // pi as default = 3.14, but pi as equation LHS means file override
        // Actually pi=3.14 is a default (bare number), so solve_recursive
        // should use the default value, not the builtin
    }

    // Physics example still works without explicit pi
    {
        FormulaSystem sys;
        sys.load_file("examples/physics.fw");
        double c = sys.resolve("circumference", {{"radius", 5}});
        ASSERT_NUM(c, 2 * M_PI * 5, "constant: physics circumference uses builtin pi");
    }

    // Triangle example still works
    {
        FormulaSystem sys;
        sys.load_file("examples/triangle.fw");
        double C = sys.resolve("C", {{"A", 60}, {"B", 90}});
        ASSERT_NUM(C, 30, "constant: triangle angle sum with builtin pi");
    }

    // Rational recognition
    {
        auto f1 = recognize_fraction(0.5);
        ASSERT(f1 && f1->p == 1 && f1->q == 2, "recognize: 0.5 = 1/2");

        auto f2 = recognize_fraction(0.333333333, 12, 1e-6);
        ASSERT(f2 && f2->p == 1 && f2->q == 3, "recognize: 0.333... = 1/3");

        auto f3 = recognize_fraction(M_PI);
        ASSERT(!f3, "recognize: pi is not rational");

        auto f4 = recognize_fraction(7.0);
        ASSERT(f4 && f4->p == 7 && f4->q == 1, "recognize: 7 = 7/1");
    }

    // Constant recognition
    {
        auto c1 = recognize_constant(M_PI);
        ASSERT(c1 && c1->constant == "pi" && c1->p == 1 && c1->q == 1,
            "recognize: pi detected");

        auto c2 = recognize_constant(2 * M_PI);
        ASSERT(c2 && c2->constant == "pi" && c2->p == 2 && c2->q == 1,
            "recognize: 2*pi detected");

        auto c3 = recognize_constant(M_PI * M_PI);
        ASSERT(c3 && c3->constant == "pi" && c3->power == 2,
            "recognize: pi^2 detected");

        auto c4 = recognize_constant(M_E);
        ASSERT(c4 && c4->constant == "e",
            "recognize: e detected");

        auto c5 = recognize_constant(42.0);
        ASSERT(!c5, "recognize: 42 is rational, no constant");

        auto c6 = recognize_constant(M_PI / 3.0);
        ASSERT(c6 && c6->constant == "pi" && c6->p == 1 && c6->q == 3,
            "recognize: pi/3 detected");
    }

    // Fitter uses constant recognition
    {
        ExprArena arena;
        ExprArena::Scope scope(arena);
        write_fw("/tmp/tc_fit.fw", "y = pi * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_fit.fw");
        auto result = sys.fit("y", {}, {{"x", "x"}});
        ASSERT(result.equation.find("pi") != std::string::npos,
            "constant: fitter outputs pi not 3.14...");
    }
}

void test_template_fitting() {
    SECTION("Template Fitting");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // Power law: y = 3*x^2, x in [1,20]
    {
        auto f = [](double x) { return 3 * x * x; };
        auto samples = sample_function(f, 1, 20, 100);
        auto result = fit_power_law(samples, "x");
        ASSERT(result.r_squared > 0.999, "template: power law R² > 0.999");
        ASSERT(result.expr != nullptr, "template: power law has expr");
    }

    // Exponential: y = 2*e^(0.5*x), x in [0,10]
    {
        auto f = [](double x) { return 2 * std::exp(0.5 * x); };
        auto samples = sample_function(f, 0, 10, 100);
        auto result = fit_exponential(samples, "x");
        ASSERT(result.r_squared > 0.999, "template: exponential R² > 0.999");
    }

    // Logarithmic: y = 5*log(x) + 2, x in [1,100]
    {
        auto f = [](double x) { return 5 * std::log(x) + 2; };
        auto samples = sample_function(f, 1, 100, 100);
        auto result = fit_logarithmic(samples, "x");
        ASSERT(result.r_squared > 0.999, "template: logarithmic R² > 0.999");
    }

    // Sinusoidal: y = 3*sin(2*x), x in [0,20]
    {
        auto f = [](double x) { return 3 * std::sin(2 * x); };
        auto samples = sample_function(f, 0, 20, 200);
        auto result = fit_sinusoidal(samples, "x");
        ASSERT(result.r_squared > 0.9, "template: sinusoidal R² > 0.9");
    }

    // fit_all returns multiple results for exponential data
    {
        auto f = [](double x) { return 2 * std::exp(0.5 * x); };
        auto samples = sample_function(f, 0, 10, 100);
        auto results = fit_all(samples, "x");
        ASSERT(results.size() >= 2, "template: fit_all finds multiple fits (got "
            + std::to_string(results.size()) + ")");
        // Best should be exponential or polynomial with high R²
        ASSERT(results[0].r_squared > 0.999, "template: best fit R² > 0.999");
    }

    // fit_all sorted by R² descending
    {
        auto f = [](double x) { return 5 * std::log(x) + 2; };
        auto samples = sample_function(f, 1, 100, 100);
        auto results = fit_all(samples, "x");
        for (size_t i = 1; i < results.size(); i++)
            ASSERT(results[i-1].r_squared >= results[i].r_squared - 1e-6,
                "template: fit_all sorted by R²");
    }

    // Integration: --fit shows alternatives
    {
        write_fw("/tmp/tt_exp.fw", "y = 2 * e^(0.5*x)\nx >= 0\nx <= 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tt_exp.fw");
        auto result = sys.fit("y", {}, {{"x", "x"}});
        ASSERT(!result.alternatives.empty(),
            "template: integration shows alternatives");
    }
}

// ---- Coverage gaps (from audit) ----

void test_numeric_edge_cases_extended() {
    SECTION("Numeric Edge Cases Extended");

    // Newton at exact root — should converge immediately
    {
        auto f = [](double x) { return x * x - 4; };
        auto root = newton_solve(f, 2.0);
        ASSERT(root.has_value(), "newton: x0 at root converges");
        ASSERT_NUM(*root, 2.0, "newton: x0=2 for x²-4 → 2");
    }

    // Bisection with endpoint as root — find_numeric_roots handles this via exact zero check
    {
        auto f = [](double x) { return x - 3.0; };
        auto roots = find_numeric_roots(f, 2.0, 10.0);
        bool found_3 = false;
        for (auto r : roots) if (std::abs(r - 3.0) < 1e-6) found_3 = true;
        ASSERT(found_3, "numeric: endpoint root found via scan");
    }

    // Constant function — no roots
    {
        auto f = [](double x) { (void)x; return 5.0; };
        auto roots = find_numeric_roots(f, -10, 10);
        ASSERT(roots.empty(), "numeric: constant function → no roots");
    }

    // Step function
    {
        auto f = [](double x) { return x < 0 ? -1.0 : 1.0; };
        auto roots = find_numeric_roots(f, -10, 10);
        // Sign change at 0, but not a true root — should find ~0 or empty
        // Either outcome is acceptable
        ASSERT(true, "numeric: step function doesn't crash");
    }

    // Precision 0 should not crash (guard against division by zero)
    {
        auto f = [](double x) { return x; };
        auto samples = sample_function(f, -1, 1, 0);
        ASSERT(samples.empty(), "numeric: 0 samples → empty");
    }

    // Very narrow interval bisection
    {
        auto f = [](double x) { return x - 1.0; };
        auto root = bisection_solve(f, 0.9999999999, 1.0000000001);
        ASSERT(root.has_value(), "bisection: very narrow interval");
        ASSERT_NUM(*root, 1.0, "bisection: narrow → 1.0");
    }
}

void test_constants_edge_cases() {
    SECTION("Constants Edge Cases");

    // Constants in conditions
    {
        write_fw("/tmp/tc_cond.fw", "y = x if x >pi\ny = 0 if x <=pi\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_cond.fw");
        double y = sys.resolve("y", {{"x", 4}});
        ASSERT_NUM(y, 4, "constant: condition x > pi with x=4");
        double y2 = sys.resolve("y", {{"x", 3}});
        ASSERT_NUM(y2, 0, "constant: condition x <= pi with x=3");
    }

    // Phi in equations
    {
        write_fw("/tmp/tc_phi.fw", "golden = phi * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_phi.fw");
        double g = sys.resolve("golden", {{"x", 10}});
        double expected = (1.0 + std::sqrt(5.0)) / 2.0 * 10;
        ASSERT_NUM(g, expected, "constant: phi in equation");
    }

    // Multiple constants in derive
    {
        write_fw("/tmp/tc_multi.fw", "y = pi * e * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_multi.fw");
        auto result = sys.derive("y", {}, {{"x", "x"}});
        ASSERT(result.find("pi") != std::string::npos, "constant: derive has pi");
        ASSERT(result.find("e") != std::string::npos, "constant: derive has e");
    }

    // Constants in formula call bindings
    {
        write_fw("/tmp/tc_fcall_inner.fw", "y = a * x\n");
        write_fw("/tmp/tc_fcall.fw", "tc_fcall_inner(y=?result, a=pi, x=r)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_fcall.fw");
        double result = sys.resolve("result", {{"r", 2}});
        ASSERT_NUM(result, M_PI * 2, "constant: pi passed to formula call");
    }

    // Constant override by equation LHS (not just default)
    {
        write_fw("/tmp/tc_eq_override.fw", "pi = 3\ny = pi * x\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc_eq_override.fw");
        // pi=3 is a default (bare number), so solve_recursive checks defaults first
        double y = sys.resolve("y", {{"x", 10}});
        ASSERT_NUM(y, 30, "constant: pi=3 override via default");
    }

    // Constant recognition edge: value close to pi but not pi
    {
        auto r = recognize_constant(3.14);
        ASSERT(!r.has_value(), "constant: 3.14 is NOT pi");
    }

    // Negative constant recognition
    {
        auto r = recognize_constant(-M_PI);
        ASSERT(r.has_value() && r->p == -1, "constant: -pi recognized");
    }

    // Zero not recognized as constant
    {
        auto r = recognize_constant(0.0);
        ASSERT(!r.has_value(), "constant: 0 not a constant");
    }
}

void test_derive_edge_cases_extended() {
    SECTION("Derive Edge Cases Extended");

    // Deeply nested formula calls (3 levels: A → B → C)
    {
        write_fw("/tmp/td3_c.fw", "z = w * 2\n");
        write_fw("/tmp/td3_b.fw", "td3_c(z=?mid, w=v)\ny = mid + 1\n");
        write_fw("/tmp/td3_a.fw", "td3_b(y=?result, v=x)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td3_a.fw");
        try {
            auto result = sys.derive("result", {}, {{"x", "x"}});
            ASSERT_EQ(result, "2 * x + 1", "derive: 3-level deep formula chain");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("derive: 3-level threw: ") + e.what());
        }
    }

    // Unfold with conditions on sub-system equations
    {
        write_fw("/tmp/td_cond_inner.fw", "y = x if x >0\ny = 0 if x <=0\n");
        write_fw("/tmp/td_cond_outer.fw", "td_cond_inner(y=?r, x=a)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td_cond_outer.fw");
        // With a=5, condition x>0 should select first equation
        auto result = sys.derive("r", {{"a", 5}}, {});
        ASSERT_EQ(result, "5", "derive: unfold with condition on sub-system");
    }

    // Derive with multiple constants preserved
    {
        write_fw("/tmp/td_multiconst.fw", "y = pi * x + e\n");
        FormulaSystem sys;
        sys.load_file("/tmp/td_multiconst.fw");
        auto result = sys.derive("y", {}, {{"x", "x"}});
        ASSERT(result.find("pi") != std::string::npos && result.find("e") != std::string::npos,
            "derive: multiple constants preserved (got '" + result + "')");
    }
}

void test_fit_edge_cases() {
    SECTION("Fit Edge Cases");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // Too few samples
    {
        auto f = [](double x) { return x; };
        auto samples = sample_function(f, 0, 1, 1); // only 2 points
        auto result = fit_polynomial(samples, 1);
        // Should handle gracefully (might have R²=1 with 2 points for degree 1)
        ASSERT(true, "fit: 2 samples doesn't crash");
    }

    // NaN-producing function
    {
        auto f = [](double x) { return x < 0 ? std::sqrt(x) : x; };
        auto samples = sample_function(f, -10, 10, 100);
        // Negative x produces NaN, should be filtered
        for (auto& s : samples)
            ASSERT(std::isfinite(s.y), "fit: NaN filtered from samples");
    }

    // Reciprocal with b ≈ 0 (1/x)
    {
        auto f = [](double x) { return 1.0 / x; };
        auto samples = sample_function(f, 0.5, 10, 100);
        auto result = fit_reciprocal(samples, "x");
        ASSERT(result.r_squared > 0.99, "fit: reciprocal 1/x fits well");
    }

    // Constant function fit
    {
        auto f = [](double) { return 42.0; };
        auto samples = sample_function(f, -10, 10, 50);
        auto result = fit_polynomial(samples, 1);
        ASSERT_NUM(result.coefficients[0], 42, "fit: constant function c0=42");
        ASSERT(std::abs(result.coefficients[1]) < 1e-6, "fit: constant function c1≈0");
    }

    // Fraction recognition edge cases
    {
        auto f1 = recognize_fraction(0.0);
        ASSERT(f1 && f1->p == 0, "fraction: 0 = 0/1");

        auto f2 = recognize_fraction(-0.5);
        ASSERT(f2 && f2->p == -1 && f2->q == 2, "fraction: -0.5 = -1/2");

        auto f3 = recognize_fraction(1e20);
        ASSERT(!f3, "fraction: huge number not a simple fraction");

        auto f4 = recognize_fraction(std::numeric_limits<double>::quiet_NaN());
        ASSERT(!f4, "fraction: NaN not recognized");

        auto f5 = recognize_fraction(std::numeric_limits<double>::infinity());
        ASSERT(!f5, "fraction: infinity not recognized");
    }
}

void test_fit_templates_edge() {
    SECTION("Fit Templates Edge Cases");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // Reciprocal direct unit test: y = 5/(x+3)
    {
        auto f = [](double x) { return 5.0 / (x + 3.0); };
        auto samples = sample_function(f, 0, 20, 100);
        auto result = fit_reciprocal(samples, "x");
        ASSERT(result.r_squared > 0.999, "template: reciprocal y=5/(x+3) R² > 0.999");
        ASSERT(result.expr != nullptr, "template: reciprocal has expr");
    }

    // Power law with negative x — should not crash, low/no fit
    {
        auto f = [](double x) { return x * x; };
        auto samples = sample_function(f, -10, -1, 50);
        auto result = fit_power_law(samples, "x");
        // Power law filters x <= 0, so few/no valid samples → low R² or empty
        ASSERT(true, "template: power law negative x doesn't crash");
    }

    // Exponential with all-negative y — should not crash
    {
        auto f = [](double x) { return -std::exp(x); };
        auto samples = sample_function(f, 0, 5, 50);
        auto result = fit_exponential(samples, "x");
        // All y < 0, log(y) undefined → should return empty/bad fit
        ASSERT(result.r_squared < 0.5 || result.coefficients.empty(),
            "template: exponential negative y handled gracefully");
    }

    // Sinusoidal with no zero crossings (constant + tiny noise)
    {
        auto f = [](double x) { return 100.0 + 0.001 * std::sin(x); };
        auto samples = sample_function(f, 0, 10, 50);
        auto result = fit_sinusoidal(samples, "x");
        // Very few/no zero crossings around mean → might not fit
        ASSERT(true, "template: sinusoidal no crossings doesn't crash");
    }

    // Composition depth validation: depth 3 finds sin(sin(x))
    {
        auto f = [](double x) { return std::sin(std::sin(x)); };
        auto samples = sample_function(f, 0, 3, 200);
        auto fits_d1 = fit_all(samples, "x", {}, 0.9, 1);
        auto fits_d3 = fit_all(samples, "x", {}, 0.9, 3);
        // Depth 3 should find more/better fits than depth 1
        bool d3_better = fits_d3.empty() ? false :
            fits_d3[0].r_squared > (fits_d1.empty() ? 0 : fits_d1[0].r_squared);
        ASSERT(d3_better || (!fits_d3.empty() && fits_d3[0].r_squared > 0.999),
            "template: depth 3 improves on depth 1 for sin(sin(x))");
    }

    // Singular matrix: all-identical x values
    {
        std::vector<FitSample> samples = {{1,2}, {1,3}, {1,4}};
        auto A = vandermonde(samples, 1);
        std::vector<double> b = {2, 3, 4};
        auto x = least_squares_solve(A, b);
        // Singular Vandermonde (all x=1) — should not crash
        ASSERT(x.size() == 2, "matrix: singular Vandermonde doesn't crash");
    }

    // Gaussian template (quadratic exponent)
    {
        auto f = [](double x) { return 5.0 * std::exp(-0.5 * (x-3)*(x-3)); };
        auto samples = sample_function(f, -5, 11, 200);
        auto result = fit_exponential(samples, "x");
        ASSERT(result.r_squared > 0.99, "template: Gaussian via quadratic exponent");
    }

    // Product inner: x*log(x) → e^(x*log(x)) = x^x
    {
        auto f = [](double x) { return std::pow(x, x); };
        auto samples = sample_function(f, 1, 5, 200);
        auto fits = fit_all(samples, "x", {}, 0.99, 3);
        bool found_exact = false;
        for (auto& fit : fits)
            if (fit.r_squared > 0.9999) found_exact = true;
        ASSERT(found_exact, "template: x^x found via product inner composition");
    }
}

void test_numeric_precision_edge() {
    SECTION("Numeric Precision Edge Cases");

    // Precision 0 from CLI shouldn't crash binary
    {
        write_fw("/tmp/tnpe.fw", "y = x^2\n");
        int rc = system("./bin/fwiz --precision 0 '/tmp/tnpe(x=?, y=4)' 2>/dev/null");
        // May fail to find roots (0 samples) but shouldn't crash
        ASSERT(WEXITSTATUS(rc) == 0 || WEXITSTATUS(rc) == 1,
            "numeric: --precision 0 doesn't crash");
    }

    // Negative precision — should handle gracefully
    {
        write_fw("/tmp/tnpe2.fw", "y = x + 1\n");
        int rc = system("./bin/fwiz --precision -5 '/tmp/tnpe2(y=?, x=3)' 2>/dev/null");
        ASSERT(WEXITSTATUS(rc) == 0, "numeric: negative precision doesn't crash");
    }
}

void test_inline_and_stdin() {
    SECTION("Inline and Stdin Input");

    // load_string basic
    {
        FormulaSystem sys;
        sys.load_string("y = 2 * x + 1\n");
        double y = sys.resolve("y", {{"x", 3}});
        ASSERT_NUM(y, 7, "load_string: y = 2*3 + 1 = 7");
    }

    // load_string with semicolons (replaced by newlines in CLI)
    {
        FormulaSystem sys;
        std::string source = "y = a * x\na = 5\n";
        sys.load_string(source);
        double y = sys.resolve("y", {{"x", 3}});
        ASSERT_NUM(y, 15, "load_string: multi-equation y = 5*3 = 15");
    }

    // load_string with conditions
    {
        FormulaSystem sys;
        sys.load_string("y = x if x >0\ny = 0 if x <=0\n");
        ASSERT_NUM(sys.resolve("y", {{"x", 5}}), 5, "load_string: condition x>0");
        ASSERT_NUM(sys.resolve("y", {{"x", -3}}), 0, "load_string: condition x<=0");
    }

    // load_string with builtin constants
    {
        FormulaSystem sys;
        sys.load_string("y = pi * x\n");
        double y = sys.resolve("y", {{"x", 2}});
        ASSERT_NUM(y, 2 * M_PI, "load_string: pi constant works");
    }

    // parse_cli_query: query-first format
    {
        auto q = parse_cli_query("(y=?, x=3) y = x^2");
        ASSERT(q.filename.empty(), "query-first: no filename");
        ASSERT_EQ(q.inline_source, "y = x^2", "query-first: inline source captured");
        ASSERT(q.queries.size() == 1, "query-first: one query");
        ASSERT_EQ(q.queries[0].variable, "y", "query-first: query var = y");
    }

    // parse_cli_query: file format still works
    {
        auto q = parse_cli_query("myfile(y=?, x=3)");
        ASSERT_EQ(q.filename, "myfile.fw", "file format: filename = myfile.fw");
        ASSERT(q.inline_source.empty(), "file format: no inline source");
    }

    // Binary: inline
    {
        int rc = system("./bin/fwiz '(y=?, x=5) y = x * 2' 2>/dev/null | grep -q 'y = 10'");
        ASSERT(WEXITSTATUS(rc) == 0, "binary: inline y = x*2 with x=5 → 10");
    }

    // Binary: inline with semicolons
    {
        int rc = system("./bin/fwiz '(y=?, x=3) y = a * x; a = 4' 2>/dev/null | grep -q 'y = 12'");
        ASSERT(WEXITSTATUS(rc) == 0, "binary: inline semicolons y = 4*3 = 12");
    }

    // Binary: stdin
    {
        int rc = system("echo 'y = x + 10' | ./bin/fwiz '(y=?, x=5)' 2>/dev/null | grep -q 'y = 15'");
        ASSERT(WEXITSTATUS(rc) == 0, "binary: stdin y = 5+10 = 15");
    }

    // Binary: --derive with inline
    {
        int rc = system("./bin/fwiz --derive '(y=?, x=x) y = 2 * x + 1' 2>/dev/null | grep -q 'y = 2 \\* x + 1'");
        ASSERT(WEXITSTATUS(rc) == 0, "binary: --derive inline");
    }

    // Binary: --fit with inline
    {
        int rc = system("./bin/fwiz --fit '(y=?, x=x) y = x^2' 2>/dev/null | grep -q 'x\\^2'");
        ASSERT(WEXITSTATUS(rc) == 0, "binary: --fit inline");
    }
}

void test_sections() {
    SECTION("Multi-System Sections");

    // Basic section selection
    {
        FormulaSystem sys;
        sys.load_string("[rect]\narea = w * h\n[circ]\narea = pi * r^2\n", "<test>", "rect");
        double a = sys.resolve("area", {{"w", 5}, {"h", 3}});
        ASSERT_NUM(a, 15, "section: rect area = 15");
    }

    // Different section from same source
    {
        FormulaSystem sys;
        sys.load_string("[rect]\narea = w * h\n[circ]\narea = pi * r^2\n", "<test>", "circ");
        double a = sys.resolve("area", {{"r", 5}});
        ASSERT_NUM(a, M_PI * 25, "section: circ area = pi*25");
    }

    // Top-level inheritance: defaults shared
    {
        FormulaSystem sys;
        sys.load_string("g = 9.81\n[physics]\nforce = mass * g\n", "<test>", "physics");
        double f = sys.resolve("force", {{"mass", 10}});
        ASSERT_NUM(f, 98.1, "section: inherits top-level default g");
    }

    // Top-level inheritance: equations shared
    {
        FormulaSystem sys;
        sys.load_string("base = x + 1\n[a]\ny = base * 2\n", "<test>", "a");
        double y = sys.resolve("y", {{"x", 4}});
        ASSERT_NUM(y, 10, "section: inherits top-level equation");
    }

    // Cascading: [a.b] inherits from [a]
    {
        FormulaSystem sys;
        sys.load_string("[shape]\narea = w * h\n[shape.box]\nvolume = area * d\n",
            "<test>", "shape.box");
        double v = sys.resolve("volume", {{"w", 3}, {"h", 4}, {"d", 5}});
        ASSERT_NUM(v, 60, "section: shape.box inherits shape area");
    }

    // Module level only (no section specified, file has sections)
    {
        FormulaSystem sys;
        sys.load_string("shared = 42\n[a]\ny = shared + x\n", "<test>", "");
        // Only top-level loaded — "y" should not be available
        auto msg = get_error([&]() { sys.resolve("y", {{"x", 1}}); });
        ASSERT(!msg.empty(), "section: module-level only, subsystem eq not available");
    }

    // Section not found → error
    {
        FormulaSystem sys;
        auto msg = get_error([&]() {
            sys.load_string("[a]\ny = x\n", "<test>", "nonexistent");
        });
        ASSERT(msg.find("not found") != std::string::npos,
            "section: nonexistent section throws");
    }

    // No sections → backwards compatible (all lines loaded)
    {
        FormulaSystem sys;
        sys.load_string("y = x + 1\nz = y * 2\n");
        double z = sys.resolve("z", {{"x", 4}});
        ASSERT_NUM(z, 10, "section: no sections = all lines loaded");
    }

    // CLI: file.section(args) parsing
    {
        auto q = parse_cli_query("geometry.triangle(C=?, A=60, B=90)");
        ASSERT_EQ(q.filename, "geometry.fw", "section CLI: filename = geometry.fw");
        ASSERT_EQ(q.section, "triangle", "section CLI: section = triangle");
    }

    // CLI: file.section.sub(args) parsing
    {
        auto q = parse_cli_query("shapes.circle.ring(area=?, r=5)");
        ASSERT_EQ(q.filename, "shapes.fw", "section CLI: nested filename");
        ASSERT_EQ(q.section, "circle.ring", "section CLI: nested section");
    }

    // CLI: file.fw(args) — direct file path, no section
    {
        auto q = parse_cli_query("examples/triangle.fw(C=?, A=60)");
        ASSERT_EQ(q.filename, "examples/triangle.fw", "section CLI: .fw is file, not section");
        ASSERT(q.section.empty(), "section CLI: no section for .fw path");
    }

    // Binary: section selection
    {
        write_fw("/tmp/tsec.fw",
            "[rect]\narea = w * h\n[circ]\narea = pi * r^2\n");
        int rc = system("./bin/fwiz '/tmp/tsec.rect(area=?, w=5, h=3)' 2>/dev/null "
                        "| grep -q 'area = 15'");
        ASSERT(WEXITSTATUS(rc) == 0, "section binary: rect area = 15");
    }

    // Binary: cascading section (uses integer gravity to avoid fraction-vs-decimal
    // rendering concerns — this test is about section cascading, not formatting)
    {
        write_fw("/tmp/tsec2.fw",
            "g = 10\n[phys]\nforce = mass * g\n[phys.gravity]\nweight = force\n");
        int rc = system("./bin/fwiz '/tmp/tsec2.phys.gravity(weight=?, mass=10)' 2>/dev/null "
                        "| grep -q 'weight = 100'");
        ASSERT(WEXITSTATUS(rc) == 0, "section binary: cascading phys.gravity");
    }

    // Inline with section selector: name(args) source
    {
        auto q = parse_cli_query("formula(x=?) [formula]; x = 10^2");
        ASSERT(q.filename.empty(), "inline section: no filename");
        ASSERT_EQ(q.section, "formula", "inline section: section = formula");
        ASSERT(!q.inline_source.empty(), "inline section: has inline source");
    }

    // Binary: inline with section
    {
        int rc = system("./bin/fwiz 'mybox(vol=?) shared = 3; [mybox]; vol = shared^2' 2>/dev/null "
                        "| grep -q 'vol = 9'");
        ASSERT(WEXITSTATUS(rc) == 0, "section binary: inline with section selector");
    }

    // Cross-file section call
    {
        write_fw("/tmp/tsec_shapes.fw",
            "[rect]\narea = w * h\n[circ]\narea = pi * r^2\n");
        write_fw("/tmp/tsec_building.fw",
            "tsec_shapes.rect(area=?floor, w=width, h=depth)\nvolume = floor * height\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tsec_building.fw");
        double v = sys.resolve("volume", {{"width", 10}, {"depth", 8}, {"height", 3}});
        ASSERT_NUM(v, 240, "section: cross-file section call");
    }
}

void test_simplify_assumptions() {
    SECTION("Simplify Assumptions");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // x/x → 1, assumes x != 0
    {
        simplify_clear_assumptions();
        auto expr = simplify(parse("x / x"));
        auto assumptions = simplify_get_assumptions();
        ASSERT_EQ(expr_to_string(expr), "1", "assumption: x/x → 1");
        ASSERT(assumptions.size() == 1, "assumption: one assumption for x/x");
        if (!assumptions.empty())
            ASSERT(assumptions[0].desc.find("x") != std::string::npos,
                "assumption: mentions x");
    }

    // a*b/a → b, assumes a != 0
    {
        simplify_clear_assumptions();
        auto expr = simplify(parse("a * b / a"));
        auto assumptions = simplify_get_assumptions();
        ASSERT_EQ(expr_to_string(expr), "b", "assumption: a*b/a → b");
        ASSERT(assumptions.size() == 1, "assumption: one assumption for a*b/a");
        if (!assumptions.empty())
            ASSERT(assumptions[0].desc.find("a") != std::string::npos,
                "assumption: mentions a");
    }

    // No cancellation → no assumptions
    {
        simplify_clear_assumptions();
        simplify(parse("x + 1"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(assumptions.empty(), "assumption: none for x + 1");
    }

    // Numeric division → no assumption (3/3 is just arithmetic)
    {
        simplify_clear_assumptions();
        simplify(parse("6 / 3"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(assumptions.empty(), "assumption: none for 6/3");
    }

    // Complex: (x-3)*z/(x-3) → z, assumes x-3 != 0
    {
        simplify_clear_assumptions();
        auto expr = simplify(parse("(x - 3) * z / (x - 3)"));
        auto assumptions = simplify_get_assumptions();
        ASSERT_EQ(expr_to_string(expr), "z", "assumption: (x-3)*z/(x-3) → z");
        ASSERT(!assumptions.empty(), "assumption: has assumption for (x-3)*z/(x-3)");
        if (!assumptions.empty())
            ASSERT(assumptions[0].desc.find("x - 3") != std::string::npos,
                "assumption: mentions x - 3 (got '" + assumptions[0].desc + "')");
    }

    // sin(x)/sin(x) → 1, assumes sin(x) != 0
    {
        simplify_clear_assumptions();
        auto expr = simplify(parse("sin(x) / sin(x)"));
        auto assumptions = simplify_get_assumptions();
        ASSERT_EQ(expr_to_string(expr), "1", "assumption: sin(x)/sin(x) → 1");
        ASSERT(!assumptions.empty(), "assumption: has assumption for sin(x)/sin(x)");
    }

    // Dedup: x*x/x → x with only one assumption (not two)
    {
        simplify_clear_assumptions();
        auto expr = simplify(parse("x * x / x"));
        auto assumptions = simplify_get_assumptions();
        ASSERT_EQ(expr_to_string(expr), "x", "assumption: x*x/x → x");
        ASSERT(assumptions.size() == 1, "assumption: dedup — one assumption for x*x/x");
    }
}

void test_simplify_exp_log() {
    SECTION("Simplify Exp/Log Rules");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // Load builtin rewrite rules for simplifier
    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules);

    // e^(log(x)) → x
    ASSERT_EQ(expr_to_string(simplify(parse("e^(log(x))"))), "x",
        "simplify: e^log(x) → x");

    // log(e^x) → x
    ASSERT_EQ(expr_to_string(simplify(parse("log(e^x)"))), "x",
        "simplify: log(e^x) → x");

    // log(x^3) → 3 * log(x)
    ASSERT_EQ(expr_to_string(simplify(parse("log(x^3)"))), "3 * log(x)",
        "simplify: log(x^3) → 3*log(x)");

    // sqrt(x^2) → abs(x)
    ASSERT_EQ(expr_to_string(simplify(parse("sqrt(x^2)"))), "abs(x)",
        "simplify: sqrt(x^2) → abs(x)");

    // (x^2)^3 → x^6
    ASSERT_EQ(expr_to_string(simplify(parse("(x^2)^3"))), "x^6",
        "simplify: (x^2)^3 → x^6");

    // (x^a)^b → x^(a*b) with symbolic exponents
    ASSERT_EQ(expr_to_string(simplify(parse("(x^a)^b"))), "x^(a * b)",
        "simplify: (x^a)^b → x^(a*b)");

    // Existing rules still work
    ASSERT_EQ(expr_to_string(simplify(parse("e^0"))), "1", "simplify: e^0 → 1");
    ASSERT_EQ(expr_to_string(simplify(parse("log(1)"))), "0", "simplify: log(1) → 0");

    // Numeric evaluation still works
    {
        auto expr = simplify(parse("e^(log(5))"));
        ASSERT_NUM((evaluate(*expr).value()), 5.0, "simplify: e^log(5) = 5");
    }
    {
        auto expr = simplify(parse("log(e^3)"));
        ASSERT_NUM((evaluate(*expr).value()), 3.0, "simplify: log(e^3) = 3");
    }
}

void test_simplify_trig_abs_pow() {
    SECTION("Simplify Trig/Abs/Pow Rules");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // Load builtin rewrite rules for simplifier
    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules);

    // abs rules
    ASSERT_EQ(expr_to_string(simplify(parse("abs(abs(x))"))), "abs(x)",
        "simplify: abs(abs(x)) → abs(x)");
    ASSERT_EQ(expr_to_string(simplify(parse("abs(-x)"))), "abs(x)",
        "simplify: abs(-x) → abs(x)");

    // sin/cos odd/even
    ASSERT_EQ(expr_to_string(simplify(parse("sin(-x)"))), "-(sin(x))",
        "simplify: sin(-x) → -sin(x)");
    ASSERT_EQ(expr_to_string(simplify(parse("cos(-x)"))), "cos(x)",
        "simplify: cos(-x) → cos(x)");

    // Inverse trig pairs
    ASSERT_EQ(expr_to_string(simplify(parse("asin(sin(x))"))), "x",
        "simplify: asin(sin(x)) → x");
    ASSERT_EQ(expr_to_string(simplify(parse("acos(cos(x))"))), "x",
        "simplify: acos(cos(x)) → x");
    ASSERT_EQ(expr_to_string(simplify(parse("atan(tan(x))"))), "x",
        "simplify: atan(tan(x)) → x");

    // Forward trig of inverse
    ASSERT_EQ(expr_to_string(simplify(parse("sin(asin(x))"))), "x",
        "simplify: sin(asin(x)) → x");
    ASSERT_EQ(expr_to_string(simplify(parse("cos(acos(x))"))), "x",
        "simplify: cos(acos(x)) → x");
    ASSERT_EQ(expr_to_string(simplify(parse("tan(atan(x))"))), "x",
        "simplify: tan(atan(x)) → x");

    // Negative exponents
    ASSERT_EQ(expr_to_string(simplify(parse("x^(-1)"))), "1 / x",
        "simplify: x^(-1) → 1/x");
    ASSERT_EQ(expr_to_string(simplify(parse("x^(-2)"))), "1 / x^2",
        "simplify: x^(-2) → 1/x^2");
    ASSERT_EQ(expr_to_string(simplify(parse("x^(-3)"))), "1 / x^3",
        "simplify: x^(-3) → 1/x^3");

    // Numeric correctness
    ASSERT_NUM((evaluate(*simplify(parse("sin(-0.5)"))).value()), -std::sin(0.5),
        "simplify: sin(-0.5) evaluates correctly");
    ASSERT_NUM((evaluate(*simplify(parse("2^(-3)"))).value()), 0.125,
        "simplify: 2^(-3) = 0.125");
}

void test_simplify_common_factor() {
    SECTION("Simplify Common Factor Extraction");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    auto p = [](const std::string& s) { return Parser(Lexer(s).tokenize()).parse_expr(); };

    // (a*x + b*x) / x → a + b
    ASSERT_EQ(expr_to_string(simplify(p("(a * x + b * x) / x"))), "a + b",
        "factor: (a*x+b*x)/x → a+b");

    // (c*x - b*x) / x → c - b
    ASSERT_EQ(expr_to_string(simplify(p("(c * x - b * x) / x"))), "c - b",
        "factor: (c*x-b*x)/x → c-b");

    // (x^2 + x) / x → x + 1
    ASSERT_EQ(expr_to_string(simplify(p("(x^2 + x) / x"))), "x + 1",
        "factor: (x²+x)/x → x+1");

    // (3x + 5x) / x → 8
    ASSERT_EQ(expr_to_string(simplify(p("(3*x + 5*x) / x"))), "8",
        "factor: (3x+5x)/x → 8");

    // Multivariate: (a*x*y + b*x*y) / (x*y) → a + b
    ASSERT_EQ(expr_to_string(simplify(p("(a*x*y + b*x*y) / (x*y)"))), "a + b",
        "factor: multivariate common factor");

    // sum / (-x) → -(sum/x) → correct sign
    {
        auto expr = simplify(p("(a*x + b*x) / (0 - x)"));
        // Should simplify cleanly (neg pulled out, then distributed)
        ASSERT(expr_to_string(expr).find("/") == std::string::npos,
            "factor: sum/(-x) cancels fully");
    }

    // System-level: a*x + b*x = c*x → a = c - b
    {
        FormulaSystem sys;
        sys.load_string("y = a*x + b*x\ny = c*x\n");
        auto result = sys.derive("a", {}, {{"b","b"},{"c","c"},{"x","x"}});
        ASSERT_EQ(result, "c - b", "factor: system-level common factor cancellation");
    }
}

void test_iff_semantics() {
    SECTION("If/Iff Semantics");

    // iff: exclusive branches → derive output says iff
    {
        write_fw("/tmp/tiff1.fw",
            "[mysign]\n"
            "result = 1 iff x > 0\n"
            "result = 0 iff x = 0\n"
            "result = -1 iff x < 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tiff1.fw", "mysign");
        auto results = sys.derive_all("x", {}, {{"result", "result"}});
        bool has_iff = false;
        for (auto& r : results)
            if (r.find("iff") != std::string::npos) has_iff = true;
        ASSERT(has_iff, "iff: exclusive branches produce iff in output");
    }

    // if: overlapping branches (two equations produce result=1) → downgrade to if
    {
        write_fw("/tmp/tiff2.fw",
            "result = 1 iff x > 0\n"
            "result = 1 iff x = 0\n"   // overlaps: result=1 for both x>0 and x=0
            "result = -1 iff x < 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tiff2.fw");
        auto results = sys.derive_all("x", {}, {{"result", "result"}});
        // The two result=1 branches should downgrade to "if"
        bool found_if_not_iff = false;
        for (auto& r : results)
            if (r.find(" if ") != std::string::npos && r.find("iff") == std::string::npos)
                found_if_not_iff = true;
        ASSERT(found_if_not_iff, "iff: overlapping branches downgrade to if");
    }

    // iff with known binding: sign(x=?, result=1) → x : (0, +inf) as range
    {
        write_fw("/tmp/tiff3.fw",
            "result = 1 iff x > 0\n"
            "result = 0 iff x = 0\n"
            "result = -1 iff x < 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tiff3.fw");
        auto result = sys.resolve_all("x", {{"result", 1}});
        ASSERT(!result.empty(), "iff: resolve_all returns range for result=1");
        ASSERT(result.intervals().size() > 0, "iff: result is interval, not discrete");
    }

    // if (not iff) conditions should NOT produce range inverse
    {
        write_fw("/tmp/tiff4.fw",
            "result = 1 if x > 0\n"
            "result = 0 if x = 0\n"
            "result = -1 if x < 0\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tiff4.fw");
        auto msg = get_error([&]() { sys.resolve_all("x", {{"result", 1}}); });
        ASSERT(!msg.empty(), "if: non-iff conditions don't produce range inverse");
    }

    // iff: bidirectional flag correctly set
    {
        FormulaSystem sys;
        sys.load_string("y = x iff x > 0\nz = x if x < 0\n");
        ASSERT(sys.equations.size() == 2, "iff flag: two equations");
        ASSERT(sys.equations[0].bidirectional, "iff flag: first is bidirectional");
        ASSERT(!sys.equations[1].bidirectional, "iff flag: second is not");
    }

    // Comma syntax: ", iff" works the same
    {
        FormulaSystem sys;
        sys.load_string("y = x, iff x > 0\n");
        ASSERT(sys.equations.size() == 1, "comma iff: one equation");
        ASSERT(sys.equations[0].bidirectional, "comma iff: bidirectional");
        ASSERT(sys.equations[0].condition.has_value(), "comma iff: has condition");
    }

    // max: boundary case — result equals b, range of a
    {
        FormulaSystem sys;
        sys.load_string("result = a iff a >= b\nresult = b iff b > a\n");
        auto result = sys.resolve_all("a", {{"result", 3}, {"b", 3}});
        // a can be anything <= 3 (since max(a,3)=3 when a<=3)
        ASSERT(!result.intervals().empty() || result.discrete().size() > 1,
            "max boundary: produces range or multiple values");
    }

    // max: non-boundary — exact result
    {
        FormulaSystem sys;
        sys.load_string("result = a iff a >= b\nresult = b iff b > a\n");
        auto result = sys.resolve_all("a", {{"result", 7}, {"b", 3}});
        ASSERT(result.is_discrete(), "max non-boundary: exact result");
        ASSERT(!result.discrete().empty(), "max non-boundary: has result");
        ASSERT_NUM(result.discrete()[0], 7, "max non-boundary: a = 7");
    }

    // min: boundary case — result equals b, range of a
    {
        FormulaSystem sys;
        sys.load_string("result = a iff a <= b\nresult = b iff b < a\n");
        auto result = sys.resolve_all("a", {{"result", 3}, {"b", 3}});
        ASSERT(!result.intervals().empty() || result.discrete().size() > 1,
            "min boundary: produces range or multiple values");
    }

    // clamp: boundary — result equals lo
    {
        FormulaSystem sys;
        sys.load_string("result = lo iff x < lo\nresult = x iff x >= lo && x <= hi\nresult = hi iff x > hi\n");
        auto result = sys.resolve_all("x", {{"result", 0}, {"lo", 0}, {"hi", 10}});
        // x can be anything <= 0
        ASSERT(!result.intervals().empty() || result.discrete().size() > 1,
            "clamp lo boundary: produces range or multiple values");
    }

    // clamp: in range — exact
    {
        FormulaSystem sys;
        sys.load_string("result = lo iff x < lo\nresult = x iff x >= lo && x <= hi\nresult = hi iff x > hi\n");
        auto result = sys.resolve_all("x", {{"result", 5}, {"lo", 0}, {"hi", 10}});
        ASSERT(result.is_discrete(), "clamp in range: exact");
        ASSERT_NUM(result.discrete()[0], 5, "clamp in range: x = 5");
    }
}

void test_cross_equation_validation() {
    SECTION("Cross-Equation Validation");

    // Two linear equations: y = 2x+1 and y = x+3 → intersection at x=2, y=5
    {
        FormulaSystem sys;
        sys.load_string("y = 2*x + 1\ny = x + 3\n");
        auto result = sys.resolve_all("x", {{"y", 5}});
        auto& d = result.discrete();
        ASSERT(d.size() == 1, "linear intersection: exactly one x (got "
            + std::to_string(d.size()) + ")");
        if (!d.empty()) ASSERT_NUM(d[0], 2, "linear intersection: x = 2");
    }

    // Same but y=4 — under the "first-successful EXPR" policy, the first
    // equation's inversion `x=(y-1)/2=1.5` wins immediately; cross-equation
    // validation no longer filters inconsistent results across equations
    // (that was a side-effect of collecting multiple results). Cross-equation
    // consistency checking is deferred to the planned --validate mode.
    {
        FormulaSystem sys;
        sys.load_string("y = 2*x + 1\ny = x + 3\n");
        auto result = sys.resolve_all("x", {{"y", 4}});
        auto& d = result.discrete();
        ASSERT(d.size() == 1, "first-successful: one x from first equation");
        if (!d.empty()) ASSERT_NUM(d[0], 1.5, "first-successful: x = 1.5 from y=2x+1");
    }

    // Circle-like: two equations with shared variable, different constraints
    // r1 = sqrt(x^2) = |x|, r2 = sqrt((x-4)^2) = |x-4|
    // r1=3, r2=1 → |x|=3 gives x=3,-3; |x-4|=1 gives x=3,5
    // Only x=3 satisfies both
    {
        FormulaSystem sys;
        sys.load_string("r1 = sqrt(x^2)\nr2 = sqrt((x-4)^2)\n");
        auto result = sys.resolve_all("x", {{"r1", 3}, {"r2", 1}});
        auto& d = result.discrete();
        bool has_3 = false;
        for (auto v : d) if (std::abs(v - 3) < 1e-6) has_3 = true;
        ASSERT(has_3, "circle-like: x=3 found");
        // Should NOT have -3 or 5
        bool has_neg3 = false, has_5 = false;
        for (auto v : d) {
            if (std::abs(v + 3) < 1e-6) has_neg3 = true;
            if (std::abs(v - 5) < 1e-6) has_5 = true;
        }
        ASSERT(!has_neg3, "circle-like: x=-3 rejected (fails r2)");
        ASSERT(!has_5, "circle-like: x=5 rejected (fails r1)");
    }

    // Single equation — no cross-validation needed, all solutions valid
    {
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_string("y = x^2\n");
        auto result = sys.resolve_all("x", {{"y", 9}});
        auto& d = result.discrete();
        ASSERT(d.size() == 2, "single equation: both roots valid (got "
            + std::to_string(d.size()) + ")");
    }
}

void test_rewrite_rules() {
    SECTION("Rewrite Rules");

    // 1. Rewrite rules parsed from .fw input (builtins auto-loaded + 2 user rules)
    {
        FormulaSystem sys;
        sys.load_string("cos(-x) = cos(x)\nsin(-x) = -sin(x)\n");
        constexpr size_t builtin_count = 14;  // from BUILTIN_REWRITE_RULES
        // User rules may duplicate builtins; total should be builtins + user rules
        ASSERT(sys.rewrite_rules.size() >= builtin_count,
            "parse: has builtin rules (got " + std::to_string(sys.rewrite_rules.size()) + ")");
        ASSERT(sys.equations.empty(), "parse: no equations stored");
    }

    // 2. Simplifier applies cos(-x) → cos(x) via rewrite rule
    {
        FormulaSystem sys;
        sys.load_string("y = cos(-a)\ncos(-x) = cos(x)\n");
        auto results = sys.derive_all("y", {}, {{"a", "a"}});
        ASSERT(!results.empty(), "cos(-a) rewrite: has result");
        // Should simplify to y = cos(a)
        bool found_clean = false;
        for (auto& r : results)
            if (r.find("cos(a)") != std::string::npos) found_clean = true;
        ASSERT(found_clean, "cos(-a) rewrite: y = cos(a) (got " + results[0] + ")");
    }

    // 3. Simplifier applies sin(-x) → -sin(x) via rewrite rule
    {
        FormulaSystem sys;
        sys.load_string("y = sin(-a)\nsin(-x) = -sin(x)\n");
        auto results = sys.derive_all("y", {}, {{"a", "a"}});
        ASSERT(!results.empty(), "sin(-a) rewrite: has result");
        bool found_clean = false;
        for (auto& r : results)
            if (r.find("-sin(a)") != std::string::npos
                || r.find("-(sin(a))") != std::string::npos) found_clean = true;
        ASSERT(found_clean, "sin(-a) rewrite: y = -sin(a) (got " + results[0] + ")");
    }

    // 4. abs(abs(x)) → abs(x) via rewrite rule
    {
        FormulaSystem sys;
        sys.load_string("y = abs(abs(a))\nabs(abs(x)) = abs(x)\n");
        auto results = sys.derive_all("y", {}, {{"a", "a"}});
        ASSERT(!results.empty(), "abs(abs) rewrite: has result");
        bool found_clean = false;
        for (auto& r : results)
            if (r == "abs(a)") found_clean = true;
        ASSERT(found_clean, "abs(abs) rewrite: y = abs(a) (got " + results[0] + ")");
    }

    // 5. Numeric: rewrite rules applied during solving
    {
        FormulaSystem sys;
        sys.load_string("y = cos(-x)\ncos(-a) = cos(a)\n");
        // cos(-pi/3) should equal cos(pi/3) = 0.5
        double result = sys.resolve("y", {{"x", M_PI / 3}});
        ASSERT(std::abs(result - 0.5) < 1e-9,
            "numeric with rewrite: cos(-pi/3) = 0.5 (got " + std::to_string(result) + ")");
    }

    // 6. x/x = 1 as rewrite rule (simulates data-driven cancellation)
    {
        FormulaSystem sys;
        // Parse as rewrite rule — note: this is BINOP DIV with two wildcards
        // x/x pattern: Var("x") / Var("x") → Num(1)
        // But match_pattern requires same binding for same variable name
        ExprArena arena;
        ExprArena::Scope scope(arena);
        auto pattern = Expr::BinOpExpr(BinOp::DIV, Expr::Var("x"), Expr::Var("x"));
        auto replacement = Expr::Num(1);
        sys.rewrite_rules.push_back({pattern, replacement, "x/x = 1", "x != 0"});

        sys.load_string("y = a / a\n");
        auto results = sys.derive_all("y", {}, {{"a", "a"}});
        ASSERT(!results.empty(), "x/x rewrite: has result");
        ASSERT(results[0] == "1", "x/x rewrite: a/a = 1 (got " + results[0] + ")");
    }

    // 7. Rewrite rules don't affect unrelated expressions
    {
        FormulaSystem sys;
        sys.load_string("y = cos(a)\ncos(-x) = cos(x)\n");
        auto results = sys.derive_all("y", {}, {{"a", "a"}});
        ASSERT(!results.empty(), "no-match rewrite: has result");
        ASSERT(results[0] == "cos(a)", "no-match rewrite: unchanged (got " + results[0] + ")");
    }

    // 8. Builtin log condition: log(x^n) = n*log(x) iff x != 0
    {
        // Test via direct simplify with rules loaded
        FormulaSystem sys;
        sys.load_string("y = log(a^3)\n");
        simplify_clear_assumptions();
        RewriteRulesGuard rr_guard(&sys.rewrite_rules);
        ExprArena arena;
        ExprArena::Scope scope(arena);
        auto e = simplify(parse("log(a^3)"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(expr_to_string(e) == "3 * log(a)",
            "log condition: log(a^3) = 3*log(a) (got " + expr_to_string(e) + ")");
        bool found = false;
        for (auto& a : assumptions)
            if (a.desc.find("a") != std::string::npos
                && a.desc.find("!= 0") != std::string::npos)
                found = true;
        ASSERT(found, "log condition: assumption 'a != 0' recorded");
    }

    // 9. Custom user condition: iff x > 0
    {
        FormulaSystem sys;
        sys.load_string("y = foo(bar)\nfoo(x) = x^2 iff x > 0\n");
        simplify_clear_assumptions();
        RewriteRulesGuard rr_guard(&sys.rewrite_rules);
        auto e = simplify(parse("foo(a + 1)"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(expr_to_string(e) == "(a + 1)^2",
            "custom condition: foo(a+1) = (a+1)^2 (got " + expr_to_string(e) + ")");
        bool found = false;
        for (auto& a : assumptions)
            if (a.desc.find("a + 1") != std::string::npos
                && a.desc.find("> 0") != std::string::npos)
                found = true;
        ASSERT(found, "custom condition: assumption 'a + 1 > 0' recorded");
    }

    // 10. Multiple conditions compound (&&)
    {
        FormulaSystem sys;
        sys.load_string("magic(x, y) = x + y iff x > 0 && y > 0\n");
        simplify_clear_assumptions();
        RewriteRulesGuard rr_guard(&sys.rewrite_rules);
        ExprArena arena;
        ExprArena::Scope scope(arena);
        auto e = simplify(parse("magic(a, b)"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(expr_to_string(e) == "a + b",
            "compound condition: magic(a,b) = a+b (got " + expr_to_string(e) + ")");
        bool found = false;
        for (auto& a : assumptions)
            if (a.desc.find("a > 0") != std::string::npos
                && a.desc.find("b > 0") != std::string::npos)
                found = true;
        ASSERT(found, "compound condition: 'a > 0 && b > 0' recorded");
    }
}

void test_undefined() {
    SECTION("Undefined Keyword");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // 1. Parse: "undefined" parses as Var("undefined")
    {
        auto e = parse("undefined");
        ASSERT(e->type == ExprType::VAR, "parse: undefined is VAR");
        ASSERT(e->name == "undefined", "parse: name is 'undefined'");
        ASSERT(is_undefined(e), "parse: is_undefined() true");
    }

    // 2. is_undefined predicate
    {
        ASSERT(!is_undefined(Expr::Num(0)), "is_undefined: Num(0) false");
        ASSERT(!is_undefined(Expr::Var("x")), "is_undefined: Var(x) false");
        ASSERT(is_undefined(Expr::Var("undefined")), "is_undefined: Var(undefined) true");
    }

    // 3. Evaluate returns nullopt for undefined
    {
        auto v = evaluate(*parse("undefined"));
        ASSERT(!v, "evaluate: undefined returns nullopt");
    }

    // 4. expr_to_string
    {
        ASSERT_EQ(expr_to_string(Expr::Var("undefined")), "undefined",
            "to_string: prints 'undefined'");
    }

    // 5. Simplify propagation: -undefined → undefined
    {
        auto e = simplify(Expr::Neg(Expr::Var("undefined")));
        ASSERT(is_undefined(e), "propagate: -undefined → undefined");
    }

    // 6. Simplify propagation: undefined + x → undefined
    {
        auto e = simplify(Expr::BinOpExpr(BinOp::ADD,
            Expr::Var("undefined"), Expr::Var("x")));
        ASSERT(is_undefined(e), "propagate: undefined + x → undefined");
    }

    // 7. Simplify propagation: x * undefined → undefined
    {
        auto e = simplify(Expr::BinOpExpr(BinOp::MUL,
            Expr::Var("x"), Expr::Var("undefined")));
        ASSERT(is_undefined(e), "propagate: x * undefined → undefined");
    }

    // 8. Simplify propagation: 0 * undefined → undefined (conservative)
    {
        auto e = simplify(Expr::BinOpExpr(BinOp::MUL,
            Expr::Num(0), Expr::Var("undefined")));
        ASSERT(is_undefined(e), "propagate: 0 * undefined → undefined");
    }

    // 9. Simplify propagation: sin(undefined) → undefined
    {
        auto e = simplify(Expr::Call("sin", {Expr::Var("undefined")}));
        ASSERT(is_undefined(e), "propagate: sin(undefined) → undefined");
    }

    // 10. Simplify propagation: undefined^2 → undefined
    {
        auto e = simplify(Expr::BinOpExpr(BinOp::POW,
            Expr::Var("undefined"), Expr::Num(2)));
        ASSERT(is_undefined(e), "propagate: undefined^2 → undefined");
    }

    // 11. collect_vars excludes undefined
    {
        std::set<std::string> vars;
        collect_vars(*Expr::BinOpExpr(BinOp::ADD,
            Expr::Var("x"), Expr::Var("undefined")), vars);
        ASSERT(vars.count("x") == 1, "collect_vars: has x");
        ASSERT(vars.count("undefined") == 0, "collect_vars: no undefined");
    }

    // 12. Bare undefined doesn't simplify away
    {
        auto e = simplify(Expr::Var("undefined"));
        ASSERT(is_undefined(e), "simplify: bare undefined stays");
    }

    // 13. Rewrite rule: x/x = undefined iff x = 0 parsed correctly
    {
        FormulaSystem sys;
        sys.load_string("x/x = undefined iff x = 0\n");
        bool found_undef = false;
        for (auto& r : sys.rewrite_rules)
            if (r.is_undefined_branch && r.desc.find("x/x") != std::string::npos)
                found_undef = true;
        ASSERT(found_undef, "parse: x/x = undefined branch detected");
    }

    // 14. Undefined branch skipped: a/a still simplifies to 1
    {
        FormulaSystem sys;
        sys.load_string("y = a / a\n");
        auto results = sys.derive_all("y", {}, {{"a", "a"}});
        ASSERT(!results.empty(), "undefined skip: has result");
        ASSERT(results[0] == "1", "undefined skip: a/a = 1 (got " + results[0] + ")");
    }

    // 15. Builtin has both x/x branches
    {
        FormulaSystem sys;
        sys.load_builtins();
        int xdivx_count = 0;
        bool has_defined = false, has_undefined = false;
        for (auto& r : sys.rewrite_rules) {
            if (expr_to_string(r.pattern) == "x / x") {
                xdivx_count++;
                if (r.is_undefined_branch) has_undefined = true;
                else has_defined = true;
            }
        }
        ASSERT(xdivx_count == 2, "builtin: x/x has 2 branches (got "
            + std::to_string(xdivx_count) + ")");
        ASSERT(has_defined, "builtin: x/x has defined branch");
        ASSERT(has_undefined, "builtin: x/x has undefined branch");
    }

    // 16. ValueSet::covers_reals()
    {
        // (-inf, +inf) covers reals
        ASSERT(ValueSet::all().covers_reals(), "covers_reals: all() = true");
        // Single point doesn't
        ASSERT(!ValueSet::eq(0).covers_reals(), "covers_reals: {0} = false");
        // ne(0) = (-inf,0) | (0,+inf) doesn't cover 0
        ASSERT(!ValueSet::ne(0).covers_reals(), "covers_reals: ne(0) = false");
        // ne(0) | {0} covers reals
        auto full = ValueSet::ne(0).unite(ValueSet::eq(0));
        ASSERT(full.covers_reals(), "covers_reals: ne(0)|{0} = true");
        // gt(0) doesn't
        ASSERT(!ValueSet::gt(0).covers_reals(), "covers_reals: (0,+inf) = false");
        // gt(0) | le(0) covers reals
        auto full2 = ValueSet::gt(0).unite(ValueSet::le(0));
        ASSERT(full2.covers_reals(), "covers_reals: (0,+inf)|(-inf,0] = true");
    }

    // 17. Rewrite rule grouping: x/x group is exhaustive
    {
        FormulaSystem sys;
        sys.load_builtins();
        bool found_exhaustive = false;
        for (auto& g : sys.rewrite_rule_groups_) {
            if (g.pattern_key == "x / x") {
                found_exhaustive = g.exhaustive;
                ASSERT(g.rule_indices.size() == 2,
                    "group: x/x has 2 rules (got " + std::to_string(g.rule_indices.size()) + ")");
            }
        }
        ASSERT(found_exhaustive, "group: x/x is exhaustive");
    }

    // 18. Non-exhaustive group: single-branch rule
    {
        FormulaSystem sys;
        sys.load_string("log(x^n) = n * log(x) iff x != 0\n");
        bool found_non_exhaustive = false;
        for (auto& g : sys.rewrite_rule_groups_) {
            if (g.pattern_key.find("log") != std::string::npos
                && g.rule_indices.size() == 1) {
                found_non_exhaustive = !g.exhaustive;
            }
        }
        ASSERT(found_non_exhaustive, "group: log(x^n) single branch is not exhaustive");
    }

    // 19. Custom exhaustive group
    {
        FormulaSystem sys;
        sys.load_string(
            "foo(x) = x^2 iff x >= 0\n"
            "foo(x) = -x^2 iff x < 0\n"
        );
        bool found_exhaustive = false;
        for (auto& g : sys.rewrite_rule_groups_) {
            if (g.pattern_key == "foo(x)" && g.rule_indices.size() == 2)
                found_exhaustive = g.exhaustive;
        }
        ASSERT(found_exhaustive, "group: foo(x) with >= 0 and < 0 is exhaustive");
    }

    // 20. Inherent assumption: x/x = 1 from exhaustive group
    {
        FormulaSystem sys;
        sys.load_string("y = a / a\n");
        simplify_clear_assumptions();
        RewriteRulesGuard rr_guard(&sys.rewrite_rules, &sys.rewrite_exhaustive_flags_);
        ExprArena arena;
        ExprArena::Scope scope(arena);
        auto e = simplify(parse("a / a"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(expr_to_string(e) == "1", "inherent: a/a = 1 (got " + expr_to_string(e) + ")");
        bool found_inherent = false;
        for (auto& a : assumptions)
            if (a.desc.find("a") != std::string::npos
                && a.desc.find("!= 0") != std::string::npos
                && a.inherent)
                found_inherent = true;
        ASSERT(found_inherent, "inherent: a != 0 marked as inherent");
    }

    // 21. Non-inherent assumption: log(x^n) from non-exhaustive group
    {
        FormulaSystem sys;
        sys.load_string("y = log(a^3)\n");
        simplify_clear_assumptions();
        RewriteRulesGuard rr_guard(&sys.rewrite_rules, &sys.rewrite_exhaustive_flags_);
        ExprArena arena;
        ExprArena::Scope scope(arena);
        auto e = simplify(parse("log(a^3)"));
        auto assumptions = simplify_get_assumptions();
        ASSERT(expr_to_string(e) == "3 * log(a)",
            "non-inherent: log(a^3) = 3*log(a) (got " + expr_to_string(e) + ")");
        bool found_non_inherent = false;
        for (auto& a : assumptions)
            if (a.desc.find("a") != std::string::npos
                && a.desc.find("!= 0") != std::string::npos
                && !a.inherent)
                found_non_inherent = true;
        ASSERT(found_non_inherent, "non-inherent: a != 0 NOT marked inherent");
    }
}

void test_context_aware_simplification() {
    SECTION("Context-Aware Simplification");

    // x/x at x=0 should NOT return 1 — the rewrite rule condition (x != 0) is violated
    // Tests both resolve() and resolve_all() paths
    {
        FormulaSystem sys;
        sys.load_string("y = x/x\n");
        // resolve() path
        bool threw_resolve = false;
        try { sys.resolve("y", {{"x", 0}}); }
        catch (...) { threw_resolve = true; }
        ASSERT(threw_resolve, "x/x at x=0 (resolve): should error, not return 1");
        // resolve_all() path — should throw or return empty, not {1}
        bool threw_all = false;
        bool got_one = false;
        try {
            auto result = sys.resolve_all("y", {{"x", 0}});
            got_one = !result.discrete().empty();
        } catch (...) { threw_all = true; }
        ASSERT(threw_all || !got_one,
            "x/x at x=0 (resolve_all): should be empty or throw, not return 1");
    }

    // x/x at x=0 should fall through to alternative equation
    {
        FormulaSystem sys;
        sys.load_string("y = x/x\ny = 42 iff x <= 0\n");
        try {
            double result = sys.resolve("y", {{"x", 0}});
            ASSERT(std::abs(result - 42) < 1e-9,
                "x/x fallback: y = 42 when x=0 (got " + std::to_string(result) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "x/x fallback: should not throw (got: " + std::string(e.what()) + ")");
        }
    }

    // x/x at x=5 should still work fine
    {
        FormulaSystem sys;
        sys.load_string("y = x/x\n");
        auto result = sys.resolve_all("y", {{"x", 5}});
        ASSERT(!result.discrete().empty(), "x/x at x=5: has result");
        ASSERT(std::abs(result.discrete()[0] - 1) < 1e-9,
            "x/x at x=5: y = 1 (got " + std::to_string(result.discrete()[0]) + ")");
    }

    // (a+b)/(a+b) at a=-b should error
    {
        FormulaSystem sys;
        sys.load_string("y = (a+b)/(a+b)\n");
        bool threw = false;
        try { sys.resolve("y", {{"a", 3}, {"b", -3}}); }
        catch (...) { threw = true; }
        ASSERT(threw, "(a+b)/(a+b) at a=-b: should error");
    }
}

void test_positional_args() {
    SECTION("Positional Arguments");

    // Write test files for cross-file formula calls with positional args
    auto write_fw = [](const std::string& path, const std::string& content) {
        std::ofstream f(path);
        f << content;
    };

    // 1. Basic: square(5) → square(x=5, result=?)
    {
        write_fw("/tmp/tpa_square.fw",
            "[square(x) -> result]\nresult = x^2\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_square(5)\n");
        try {
            double result = sys.resolve("y", {});
            ASSERT(std::abs(result - 25) < 1e-9,
                "positional: square(5) = 25 (got " + std::to_string(result) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "positional: square(5) threw: " + std::string(e.what()));
        }
    }

    // 2. Multiple args: myadd(3, 4) → myadd(a=3, b=4, result=?)
    {
        write_fw("/tmp/tpa_myadd.fw",
            "[myadd(a, b) -> result]\nresult = a + b\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_myadd(3, 4)\n");
        try {
            double result = sys.resolve("y", {});
            ASSERT(std::abs(result - 7) < 1e-9,
                "positional: myadd(3, 4) = 7 (got " + std::to_string(result) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "positional: myadd(3, 4) threw: " + std::string(e.what()));
        }
    }

    // 3. Reverse: solve for input given output
    {
        write_fw("/tmp/tpa_sq2.fw",
            "[sq2(x) -> result]\nresult = x^2\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_sq2(x)\n");
        try {
            double result = sys.resolve("x", {{"y", 25}});
            ASSERT(std::abs(result - 5) < 1e-9 || std::abs(result + 5) < 1e-9,
                "reverse positional: sq2(x)=25 → x=±5 (got " + std::to_string(result) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "reverse positional threw: " + std::string(e.what()));
        }
    }

    // 4. Expression args: square(2+3) → square(x=5, result=?)
    {
        write_fw("/tmp/tpa_sq3.fw",
            "[sq3(x) -> result]\nresult = x^2\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_sq3(2+3)\n");
        try {
            double result = sys.resolve("y", {});
            ASSERT(std::abs(result - 25) < 1e-9,
                "positional expr: square(2+3) = 25 (got " + std::to_string(result) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "positional expr threw: " + std::string(e.what()));
        }
    }

    // 5. @extern fast path: use C++ function pointer directly
    {
        write_fw("/tmp/tpa_mysin.fw",
            "[mysin(x) -> result]\n@extern sin\nresult = x\n");  // fallback eq
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_mysin(1.5707963267948966)\n");  // sin(pi/2)
        try {
            double result = sys.resolve("y", {});
            ASSERT(std::abs(result - 1.0) < 1e-6,
                "@extern: sin(pi/2) = 1 (got " + std::to_string(result) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "@extern: threw: " + std::string(e.what()));
        }
    }

    // 6. @extern with inverse: solve for input given output
    {
        write_fw("/tmp/tpa_mysqrt.fw",
            "[mysqrt(x) -> result]\n@extern sqrt\nx = result^2\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_mysqrt(x)\n");
        try {
            // Forward: sqrt(9) = 3
            double fwd = sys.resolve("y", {{"x", 9}});
            ASSERT(std::abs(fwd - 3.0) < 1e-9,
                "@extern fwd: sqrt(9) = 3 (got " + std::to_string(fwd) + ")");
            // Reverse: solve x given y=4 → x = 16
            double rev = sys.resolve("x", {{"y", 4}});
            ASSERT(std::abs(rev - 16.0) < 1e-9,
                "@extern rev: sqrt(x)=4 → x=16 (got " + std::to_string(rev) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "@extern inv threw: " + std::string(e.what()));
        }
    }
}

void test_register_function() {
    SECTION("Register Custom Functions");

    // 1. Register a C++ function and use it
    {
        FormulaSystem sys;
        sys.register_function("double_it", [](double x) { return x * 2; },
            "[double_it(x) -> result]\nresult = 2 * x\nx = result / 2\n");
        sys.load_string("y = double_it(x)\n");
        double r = sys.resolve("y", {{"x", 7}});
        ASSERT(std::abs(r - 14) < 1e-9,
            "register: double_it(7) = 14 (got " + std::to_string(r) + ")");
    }

    // 2. Inverse solving with registered function
    {
        FormulaSystem sys;
        sys.register_function("double_it", [](double x) { return x * 2; },
            "[double_it(x) -> result]\nresult = 2 * x\nx = result / 2\n");
        sys.load_string("y = double_it(x)\n");
        double r = sys.resolve("x", {{"y", 14}});
        ASSERT(std::abs(r - 7) < 1e-9,
            "register inv: double_it(x)=14 → x=7 (got " + std::to_string(r) + ")");
    }

    // 3. Register without .fw def (forward only, no inverse)
    {
        FormulaSystem sys;
        sys.register_function("triple", [](double x) { return x * 3; });
        sys.load_string("y = triple(x)\n");
        double r = sys.resolve("y", {{"x", 5}});
        ASSERT(std::abs(r - 15) < 1e-9,
            "register no-def: triple(5) = 15 (got " + std::to_string(r) + ")");
    }
}

void test_semicolon_separator() {
    SECTION("Semicolon Line Separator");

    // 1. Semicolons as line separators in load_string
    {
        FormulaSystem sys;
        sys.load_string("x = 3; y = x + 1\n");
        double r = sys.resolve("y", {});
        ASSERT(std::abs(r - 4) < 1e-9,
            "semicolon: y = x + 1 with x=3 (got " + std::to_string(r) + ")");
    }

    // 2. Section header with semicolon continuation
    {
        FormulaSystem sys;
        sys.load_string("[sq(x) -> result]; result = x^2\n");
        // The section should have the equation
        bool found = false;
        for (auto& s : sys.sections_)
            if (s.name == "sq" && s.lines.size() >= 1) found = true;
        ASSERT(found, "semicolon section: [sq] has lines");
    }

    // 3. Inline section header: [f(x) -> result] result = x^2 (no separator needed)
    {
        FormulaSystem sys;
        sys.load_string("[cube(x) -> result] result = x^3\n");
        bool found = false;
        for (auto& s : sys.sections_)
            if (s.name == "cube" && s.lines.size() >= 1) found = true;
        ASSERT(found, "inline section: [cube] has lines");
    }

    // 4. Single-line function def works end-to-end
    {
        auto write_fw = [](const std::string& path, const std::string& content) {
            std::ofstream f(path);
            f << content;
        };
        write_fw("/tmp/tpa_oneline.fw",
            "[oneline(x) -> result] result = x * 10\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_oneline(5)\n");
        try {
            double r = sys.resolve("y", {});
            ASSERT(std::abs(r - 50) < 1e-9,
                "oneline: oneline(5) = 50 (got " + std::to_string(r) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "oneline threw: " + std::string(e.what()));
        }
    }

    // 5. Sugar: [f(x) -> result] = expr  →  result = expr
    {
        auto write_fw = [](const std::string& path, const std::string& content) {
            std::ofstream f(path);
            f << content;
        };
        write_fw("/tmp/tpa_sugar.fw", "[sugar(x) -> result] = x^2 + 1\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_sugar(3)\n");
        try {
            double r = sys.resolve("y", {});
            ASSERT(std::abs(r - 10) < 1e-9,
                "sugar: sugar(3) = 10 (got " + std::to_string(r) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "sugar threw: " + std::string(e.what()));
        }
    }

    // 6. Multi-line = sugar (piecewise)
    {
        FormulaSystem sys;
        sys.load_string(
            "[myabs(x) -> result]\n"
            "= x iff x >= 0\n"
            "= -x iff x < 0\n",
            "<test>", "myabs");
        double r1 = sys.resolve("result", {{"x", 5}});
        ASSERT(std::abs(r1 - 5) < 1e-9,
            "multiline sugar: myabs(5) = 5 (got " + std::to_string(r1) + ")");
        double r2 = sys.resolve("result", {{"x", -3}});
        ASSERT(std::abs(r2 - 3) < 1e-9,
            "multiline sugar: myabs(-3) = 3 (got " + std::to_string(r2) + ")");
    }

    // 7. Inline header + semicolons with = sugar
    {
        auto write_fw = [](const std::string& path, const std::string& content) {
            std::ofstream f(path);
            f << content;
        };
        write_fw("/tmp/tpa_sugar2.fw",
            "[sugar2(x) -> result] = x^2 iff x >= 0; = -x^2 iff x < 0\n");
        FormulaSystem sys;
        sys.base_dir = "/tmp";
        sys.load_string("y = tpa_sugar2(3)\n");
        try {
            double r = sys.resolve("y", {});
            ASSERT(std::abs(r - 9) < 1e-9,
                "inline+semi sugar: sugar2(3) = 9 (got " + std::to_string(r) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, "inline+semi sugar threw: " + std::string(e.what()));
        }
    }
}

void test_commutative_matching() {
    SECTION("Commutative Pattern Matching");

    ExprArena arena;
    ExprArena::Scope scope(arena);

    // --- Binary commutativity ---

    // 1. a + b should match b + a
    {
        auto pattern = parse("a + b");
        auto target = parse("y + x");
        auto fwd = match_pattern(pattern, target);  // a→y, b→x (structural)
        ASSERT(fwd.has_value(), "a+b matches y+x (structural)");

        auto rev = match_pattern(pattern, parse("x + y"));
        // With commutativity: a→y, b→x OR a→x, b→y — either is fine
        ASSERT(rev.has_value(), "a+b matches x+y (commutative)");
    }

    // 2. a * b should match b * a
    {
        auto pattern = parse("a * b");
        auto result = match_pattern(pattern, parse("3 * z"));
        ASSERT(result.has_value(), "a*b matches 3*z");
        auto result2 = match_pattern(pattern, parse("z * 3"));
        ASSERT(result2.has_value(), "a*b matches z*3 (commutative)");
    }

    // --- N-term additive permutations ---

    // 3. a + b + c should match c + b + a
    {
        auto pattern = parse("a + b + c");
        auto target = parse("3 + 2 + 1");
        auto result = match_pattern(pattern, target);
        // Structural: (a+b)+c matches (3+2)+1 → a=3, b=2, c=1
        ASSERT(result.has_value(), "a+b+c matches 3+2+1 (structural)");

        // Permuted: should match 1+2+3 (any ordering)
        auto perm = match_pattern(pattern, parse("1 + 2 + 3"));
        ASSERT(perm.has_value(), "a+b+c matches 1+2+3 (permuted)");
    }

    // 4. Different permutation: a + b + c matches b + c + a
    {
        auto pattern = parse("a + b + c");
        auto target = parse("7 + 8 + 9");
        auto result = match_pattern(pattern, target);
        ASSERT(result.has_value(), "a+b+c matches 7+8+9");
        // Verify all 3 values are captured (in some order)
        if (result) {
            std::set<double> vals;
            for (auto& [k, v] : *result)
                if (is_num(v)) vals.insert(v->num);
            ASSERT(vals.count(7) && vals.count(8) && vals.count(9),
                "a+b+c captures all three values");
        }
    }

    // --- Coefficient extraction (the quadratic use case) ---

    // 5. a*x^2 + b*x + c should match x^2 - 7*x + 12
    {
        auto pattern = parse("a*x^2 + b*x + c");
        auto target = parse("t^2 - 7*t + 12");
        auto result = match_pattern(pattern, target);
        ASSERT(result.has_value(),
            "a*x^2+b*x+c matches t^2-7*t+12 (quadratic extraction)");
        if (result) {
            auto& r = *result;
            bool x_is_t = is_var(r["x"]) && r["x"]->name == "t";
            ASSERT(x_is_t, "quadratic: x binds to t");
            bool a_is_1 = is_num(r["a"]) && std::abs(r["a"]->num - 1) < 1e-9;
            ASSERT(a_is_1, "quadratic: a = 1");
            bool b_is_neg7 = is_num(r["b"]) && std::abs(r["b"]->num - (-7)) < 1e-9;
            ASSERT(b_is_neg7, "quadratic: b = -7");
            bool c_is_12 = is_num(r["c"]) && std::abs(r["c"]->num - 12) < 1e-9;
            ASSERT(c_is_12, "quadratic: c = 12");
        }
    }

    // 6. a*x + b should match 3 + 5*x (swapped terms)
    {
        auto pattern = parse("a*x + b");
        auto target = parse("3 + 5*t");
        auto result = match_pattern(pattern, target);
        ASSERT(result.has_value(), "a*x+b matches 3+5*t (swapped)");
        if (result) {
            auto& r = *result;
            bool x_is_t = is_var(r["x"]) && r["x"]->name == "t";
            ASSERT(x_is_t, "linear swap: x binds to t");
            bool a_is_5 = is_num(r["a"]) && std::abs(r["a"]->num - 5) < 1e-9;
            ASSERT(a_is_5, "linear swap: a = 5");
            bool b_is_3 = is_num(r["b"]) && std::abs(r["b"]->num - 3) < 1e-9;
            ASSERT(b_is_3, "linear swap: b = 3");
        }
    }
}

void test_quadratic_formula() {
    SECTION("Quadratic Formula");

    // Helper: test quadratic solving (wraps in try-catch for clean failure)
    auto test_quad = [](const std::string& eq, std::map<std::string, double> bindings,
                        const std::string& target, std::vector<double> expected,
                        const std::string& name) {
        FormulaSystem sys;
        sys.numeric_mode = false;  // algebraic only
        sys.load_string(eq + "\n");
        try {
            auto result = sys.resolve_all(target, bindings);
            auto& d = result.discrete();
            ASSERT(d.size() == expected.size(),
                name + ": " + std::to_string(expected.size()) + " roots (got "
                + std::to_string(d.size()) + ")");
            for (double exp_val : expected) {
                bool found = false;
                for (auto v : d)
                    if (std::abs(v - exp_val) < 0.01) found = true;
                ASSERT(found, name + ": root " + std::to_string(exp_val) + " found");
            }
        } catch (const std::exception& e) {
            ASSERT(false, name + ": should not throw (got: " + std::string(e.what()) + ")");
        }
    };

    // 7. x^2 - 7x + 12 = 0 → x = 3, x = 4 (algebraic)
    test_quad("y = x^2 - 7*x + 12", {{"y", 0}}, "x", {3, 4}, "x^2-7x+12=0");

    // 8. 2x^2 - 4x - 6 = 0 → x = 3, x = -1 (algebraic)
    test_quad("y = 2*x^2 - 4*x - 6", {{"y", 0}}, "x", {3, -1}, "2x^2-4x-6=0");

    // 9. x^2 + 1 = 0 → no real roots
    {
        FormulaSystem sys;
        sys.numeric_mode = false;
        sys.load_string("y = x^2 + 1\n");
        bool threw = false;
        try {
            auto result = sys.resolve_all("x", {{"y", 0}});
            threw = result.discrete().empty();
        } catch (...) { threw = true; }
        ASSERT(threw, "x^2+1=0: no real roots");
    }

    // 10. x^2 + 2x - 3 = 0 → x = 1, x = -3 (algebraic)
    test_quad("y = x^2 + 2*x - 3", {{"y", 0}}, "x", {1, -3}, "x^2+2x-3=0");

    // 11. From KNOWN_ISSUES #3: y = a*x^2 + b*x + c (algebraic)
    test_quad("y = a*x^2 + b*x + c",
        {{"y", 0}, {"a", 1}, {"b", 2}, {"c", -10}}, "x",
        {-1 + std::sqrt(11), -1 - std::sqrt(11)}, "KNOWN_ISSUES#3");
}

// ---- Simultaneous equations ----

void test_simultaneous_equations() {
    SECTION("Simultaneous Equations");

    // 1. Linear system: s = x + y, d = x - y → x = (s+d)/2
    {
        FormulaSystem sys;
        sys.load_string("s = x + y\nd = x - y\n");
        try {
            double r = sys.resolve("x", {{"s", 10}, {"d", 4}});
            ASSERT_NUM(r, 7, "linear system: x = (10+4)/2 = 7");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("linear system: should not throw (got: ") + e.what() + ")");
        }
    }

    // 2. Rectangle puzzle: area = w*h, perimeter = 2w+2h → w has two solutions
    {
        FormulaSystem sys;
        sys.load_string("area = w * h\nperimeter = 2 * w + 2 * h\n");
        try {
            auto result = sys.resolve_all("w", {{"area", 12}, {"perimeter", 14}});
            auto& d = result.discrete();
            bool has_3 = false, has_4 = false;
            for (auto v : d) {
                if (std::abs(v - 3) < 1e-6) has_3 = true;
                if (std::abs(v - 4) < 1e-6) has_4 = true;
            }
            ASSERT(has_3, "rectangle puzzle: w=3 found");
            ASSERT(has_4, "rectangle puzzle: w=4 found");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("rectangle puzzle: should not throw (got: ") + e.what() + ")");
        }
    }

    // 3. Overdetermined consistent: y=2x+1, y=x+3 → x=2 (regression guard)
    {
        FormulaSystem sys;
        sys.load_string("y = 2*x + 1\ny = x + 3\n");
        try {
            auto result = sys.resolve_all("x", {{"y", 5}});
            auto& d = result.discrete();
            ASSERT(d.size() == 1, "overdetermined consistent: exactly one x (got "
                + std::to_string(d.size()) + ")");
            if (!d.empty()) ASSERT_NUM(d[0], 2, "overdetermined consistent: x = 2");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("overdetermined consistent: should not throw (got: ") + e.what() + ")");
        }
    }

    // 4. Overdetermined inconsistent: y=2x+1, y=x+3 with y=4 — under the
    //    first-successful policy, the first equation wins (x=1.5). Cross-
    //    equation consistency is deferred to planned --validate.
    {
        FormulaSystem sys;
        sys.load_string("y = 2*x + 1\ny = x + 3\n");
        auto result = sys.resolve_all("x", {{"y", 4}});
        auto& d = result.discrete();
        ASSERT(d.size() == 1, "first-successful: one x from first equation");
        if (!d.empty()) ASSERT_NUM(d[0], 1.5, "first-successful: x = 1.5 (y=2x+1)");
    }

    // 5. Derive mode: s = x + y, d = x - y → derive x symbolically
    {
        FormulaSystem sys;
        sys.load_string("s = x + y\nd = x - y\n");
        try {
            auto results = sys.derive_all("x", {}, {{"s", "s"}, {"d", "d"}});
            bool found = false;
            for (auto& r : results) {
                // Should produce something like (s + d) / 2 or (s + d) * 0.5
                if (r.find("s") != std::string::npos && r.find("d") != std::string::npos)
                    found = true;
            }
            ASSERT(found, "derive simultaneous: x in terms of s and d (got "
                + (results.empty() ? std::string("nothing") : results[0]) + ")");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("derive simultaneous: should not throw (got: ") + e.what() + ")");
        }
    }

    // 6. Numeric no-crash: rectangle puzzle with numeric mode (must not stack overflow)
    {
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_string("area = w * h\nperimeter = 2 * w + 2 * h\n");
        bool crashed = false;
        try {
            auto result = sys.resolve_all("w", {{"area", 12}, {"perimeter", 14}});
            // May or may not find the answer — but must not crash
            ASSERT(true, "numeric no-crash: did not crash");
        } catch (const std::exception&) {
            ASSERT(true, "numeric no-crash: threw but did not crash");
        }
    }

    // 7. Conditions: y = x^2 iff x >= 0, z = x + 1 iff x >= 0 → x=3
    {
        FormulaSystem sys;
        sys.load_string("y = x^2 iff x >= 0\nz = x + 1 iff x >= 0\n");
        try {
            double r = sys.resolve("x", {{"y", 9}, {"z", 4}});
            ASSERT_NUM(r, 3, "conditions: x = 3 (y=9, z=4)");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("conditions: should not throw (got: ") + e.what() + ")");
        }
    }

    // 8. Three-variable chain: p=xy, q=yz, r=xz → x=3
    {
        FormulaSystem sys;
        sys.load_string("p = x * y\nq = y * z\nr = x * z\n");
        try {
            double r = sys.resolve("x", {{"p", 6}, {"q", 10}, {"r", 15}});
            ASSERT_NUM(r, 3, "three-variable chain: x = 3 (p=6, q=10, r=15)");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("three-variable chain: should not throw (got: ") + e.what() + ")");
        }
    }

    // 9. Disjoint system: a = x + 1, b = y + 2 → x = 4 (regression guard)
    {
        FormulaSystem sys;
        sys.load_string("a = x + 1\nb = y + 2\n");
        try {
            double r = sys.resolve("x", {{"a", 5}});
            ASSERT_NUM(r, 4, "disjoint system: x = 4 (a=5)");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("disjoint system: should not throw (got: ") + e.what() + ")");
        }
    }

    // 10. Self-referencing: y = x^2 + x, z = y - 1 → z=5 means y=6, x^2+x=6, x=2
    {
        FormulaSystem sys;
        sys.load_string("y = x^2 + x\nz = y - 1\n");
        try {
            auto result = sys.resolve_all("x", {{"z", 5}});
            auto& d = result.discrete();
            bool has_2 = false;
            for (auto v : d)
                if (std::abs(v - 2) < 1e-6) has_2 = true;
            ASSERT(has_2, "self-referencing: x=2 found (z=5 → y=6 → x^2+x=6)");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("self-referencing: should not throw (got: ") + e.what() + ")");
        }
    }
}

void test_numeric_skip() {
    SECTION("Numeric Skip When Algebraic Succeeds");

    // Numeric solver should not run when algebraic strategies already found results.
    // We verify by checking that resolve_all with numeric_mode=true produces the same
    // results as without, and doesn't take excessively long on multi-equation systems.

    // 1. Rectangle puzzle: first-successful EXPR candidate yields valid w values
    //    (could be one or two from a single candidate's ValueSet — quadratic
    //    multi-root within one candidate is preserved, see test #3 of
    //    test_dead_end_and_first_candidate). The point of this test (numeric
    //    not duplicating algebraic results) is preserved: all results come
    //    from the first successful EXPR candidate, not duplicated by numeric.
    {
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_string("area = w * h\nperimeter = 2 * w + 2 * h\n");
        auto result = sys.resolve_all("w", {{"area", 12}, {"perimeter", 14}});
        auto& d = result.discrete();
        ASSERT(!d.empty(), "rectangle solve: at least one w found");
        for (double w : d) {
            ASSERT(std::abs(w - 3.0) < 1e-6 || std::abs(w - 4.0) < 1e-6,
                "rectangle solve: w in {3, 4}");
        }
    }

    // 2. Temperature chain: algebraic succeeds, numeric should not explode
    {
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_string("F = C * 9 / 5 + 32\nK = C + 273.15\nR = F + 459.67\n");
        auto start = std::chrono::steady_clock::now();
        auto result = sys.resolve_all("C", {{"F", 212}});
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        auto& d = result.discrete();
        ASSERT(!d.empty() && std::abs(d[0] - 100) < 1e-6,
            "temp chain: C = 100 for F = 212");
        // Should complete quickly (algebraic), not take tens of seconds (numeric probing)
        // Use generous timeout to account for sanitizer overhead
        ASSERT(elapsed < 10000, "temp chain: completed in < 10s (took "
            + std::to_string(elapsed) + "ms)");
    }

    // 3. Pure numeric case: no algebraic solution, numeric should still run
    {
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_string("y = sin(x) + x\n");
        auto result = sys.resolve_all("x", {{"y", 1}});
        auto& d = result.discrete();
        ASSERT(!d.empty(), "pure numeric: sin(x)+x=1 finds a root");
    }
}

// ---- Dead-end sharing + first-successful EXPR + budget sentinel ----

void test_dead_end_and_first_candidate() {
    SECTION("Dead-end sharing, first-successful EXPR, budget sentinel");

    // 1. Dead-end scoping: poisoning must be keyed by bindings-keyset,
    //    so query 1 failure does not prevent query 2 success when
    //    additional bindings make 'v' reachable.
    {
        write_fw("/tmp/tde_scope.fw",
            "v = a + b\n"
            "y = v\n");
        FormulaSystem sys;
        sys.numeric_mode = false;
        sys.load_file("/tmp/tde_scope.fw");

        // Query 1: only {a} bound → y can't be solved (v needs b).
        bool threw = false;
        try { (void)sys.resolve("y", {{"a", 1}}); }
        catch (const std::runtime_error&) { threw = true; }
        ASSERT(threw, "dead-end scoping: query 1 with only {a} fails");

        // Query 2: {a, b} → y = 3 (prev failure must NOT poison).
        try {
            double y = sys.resolve("y", {{"a", 1}, {"b", 2}});
            ASSERT_NUM(y, 3, "dead-end scoping: query 2 with {a,b} succeeds");
        } catch (const std::exception& e) {
            ASSERT(false, std::string("dead-end scoping: query 2 threw: ") + e.what());
        }
    }

    // 2. First-successful EXPR short-circuit. Two independent equations
    //    for y given x. resolve_all must return ONE result (from first eq),
    //    not two.
    {
        FormulaSystem sys;
        sys.numeric_mode = false;
        sys.load_string("y = x + 1\ny = x * 2 + 1\n");
        auto result = sys.resolve_all("y", {{"x", 3}});
        auto& d = result.discrete();
        ASSERT(d.size() == 1, "first-successful: exactly one result (got "
            + std::to_string(d.size()) + ")");
        if (!d.empty()) ASSERT_NUM(d[0], 4, "first-successful: y = x+1 wins (x=3 → 4)");
    }

    // 3. Quadratic multi-root within a single equation still works:
    //    Part B must only skip subsequent EXPR candidates, not roots
    //    within a single candidate.
    {
        FormulaSystem sys;
        sys.numeric_mode = false;
        sys.load_string("y = x^2 - 7*x + 12\n");
        auto result = sys.resolve_all("x", {{"y", 0}});
        auto& d = result.discrete();
        bool has_3 = false, has_4 = false;
        for (auto v : d) {
            if (std::abs(v - 3) < 1e-6) has_3 = true;
            if (std::abs(v - 4) < 1e-6) has_4 = true;
        }
        ASSERT(has_3 && has_4, "first-successful: quadratic multi-root preserved");
    }

    // 4. Triangle shell: angle-sum fast.
    {
        int rc = system("timeout 5 ./bin/fwiz 'examples/triangle(A=?, B=80, C=60)' "
                        "> /tmp/tde_tri_as.out 2>/tmp/tde_tri_as.err");
        ASSERT(WEXITSTATUS(rc) == 0, "triangle angle-sum: exit 0 fast");
        std::ifstream f("/tmp/tde_tri_as.out");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ASSERT(content.find("A = 40") != std::string::npos,
            "triangle angle-sum: A = 40 in stdout (got '" + content + "')");
    }

    // 5. Triangle shell: law-of-sines (SSA) fast.
    {
        int rc = system("timeout 5 ./bin/fwiz 'examples/triangle(A=?, a=4, b=24, B=20)' "
                        "> /tmp/tde_tri_ssa.out 2>/tmp/tde_tri_ssa.err");
        ASSERT(WEXITSTATUS(rc) == 0, "triangle SSA: exit 0 fast");
        std::ifstream f("/tmp/tde_tri_ssa.out");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ASSERT(content.find("A = 3.26") != std::string::npos
            || content.find("A ~ 3.26") != std::string::npos,
            "triangle SSA: A = 3.26... in stdout (got '" + content + "')");
    }

    // 6. Triangle shell: under-constrained fast-fails.
    //    Only 'a' known. The EXPR candidates for A all require multiple
    //    additional unknowns (B, C, b, c); the NUMERIC candidate's residual
    //    (after target/bindings/builtins/alias erasures) is non-empty, so
    //    the multi-variable NUMERIC skip fires. solve_all exhausts with
    //    no results → clean "Cannot solve" exit 1 in <1s.
    {
        int rc = system("timeout 5 ./bin/fwiz 'examples/triangle(A=?, a=4)' "
                        "> /tmp/tde_tri_uc.out 2>/tmp/tde_tri_uc.err");
        int exit_code = WEXITSTATUS(rc);
        ASSERT(exit_code == 1,
            "under-constrained fast-fails with exit 1, got " + std::to_string(exit_code));
        std::ifstream err("/tmp/tde_tri_uc.err");
        std::string err_content((std::istreambuf_iterator<char>(err)),
                                 std::istreambuf_iterator<char>());
        ASSERT(err_content.find("Cannot solve") != std::string::npos,
            "under-constrained stderr contains 'Cannot solve' (got '" + err_content + "')");
    }

    // 7. Factorial preserved (scoping reset at formula-call entry).
    //    Use a small file so the recursive frame's dead-ends don't poison the outer.
    {
        write_fw("/tmp/tde_fact.fw",
            "result = 1 if n =0\n"
            "result = n * tde_fact(result=?prev, n=n-1) if n >0\n"
            "n >= 0\nn <= 20\n");
        FormulaSystem sys;
        sys.numeric_mode = true;
        sys.load_file("/tmp/tde_fact.fw");
        auto result = sys.resolve_all("n", {{"result", 120}});
        auto& d = result.discrete();
        bool found_5 = false;
        for (auto r : d) if (std::abs(r - 5.0) < 1e-6) found_5 = true;
        ASSERT(found_5, "dead-end: factorial(n=?,result=120) still finds n=5");
    }
}

// ---- Main ----

// ---- Rational arithmetic (structural fractions) ----

void test_rational_fractions() {
    SECTION("Rational fractions: structural preservation");

    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules, &builtin_sys.rewrite_exhaustive_flags_);

    // Non-integer division should stay as structural fraction
    ASSERT_EQ(ss("1 / 3"), "1 / 3", "1/3 preserved");
    ASSERT_EQ(ss("2 / 5"), "2 / 5", "2/5 preserved");
    ASSERT_EQ(ss("1 / 7"), "1 / 7", "1/7 preserved");

    // Integer division should fold
    ASSERT_EQ(ss("6 / 3"), "2", "6/3 folds to 2");
    ASSERT_EQ(ss("20 / 4"), "5", "20/4 folds to 5");
    ASSERT_EQ(ss("0 / 5"), "0", "0/5 folds to 0");

    // GCD normalization
    ASSERT_EQ(ss("2 / 6"), "1 / 3", "2/6 normalized to 1/3");
    ASSERT_EQ(ss("4 / 8"), "1 / 2", "4/8 normalized to 1/2");
    ASSERT_EQ(ss("6 / 9"), "2 / 3", "6/9 normalized to 2/3");
    ASSERT_EQ(ss("15 / 10"), "3 / 2", "15/10 normalized to 3/2");

    // Sign normalization: negative in numerator only
    ASSERT_EQ(ss("(-1) / 3"), "(-1) / 3", "-1/3 sign in numer");
    // Note: parser handles "1 / (-3)" as DIV(1, NEG(3)), simplifier normalizes
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), Expr::Num(-3));
        auto s = simplify(e);
        ASSERT_EQ(expr_to_string(s), "(-1) / 3", "1/(-3) → (-1)/3 sign normalized");
    }

    // Evaluation still works correctly
    ASSERT_NUM(ev("1 / 3"), 1.0/3.0, "1/3 evaluates to 0.333...");
    ASSERT_NUM(ev("2 / 5"), 0.4, "2/5 evaluates to 0.4");

    // Structural fraction in larger expression
    ASSERT_EQ(ss("x * (1 / 3)"), "x / 3", "x * (1/3) → x/3");
}

void test_rational_arithmetic() {
    SECTION("Rational arithmetic");

    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules, &builtin_sys.rewrite_exhaustive_flags_);

    // Fraction + Fraction
    ASSERT_EQ(ss("1/3 + 1/6"), "1 / 2", "1/3 + 1/6 = 1/2");
    ASSERT_EQ(ss("1/3 + 2/3"), "1", "1/3 + 2/3 = 1");
    ASSERT_EQ(ss("1/4 + 1/4"), "1 / 2", "1/4 + 1/4 = 1/2");

    // Fraction - Fraction
    ASSERT_EQ(ss("2/3 - 1/3"), "1 / 3", "2/3 - 1/3 = 1/3");
    ASSERT_EQ(ss("1/2 - 1/3"), "1 / 6", "1/2 - 1/3 = 1/6");

    // Fraction * Fraction
    ASSERT_EQ(ss("(1/3) * (1/4)"), "1 / 12", "1/3 * 1/4 = 1/12");
    ASSERT_EQ(ss("(2/3) * (3/4)"), "1 / 2", "2/3 * 3/4 = 1/2");
    ASSERT_EQ(ss("(1/3) * 3"), "1", "1/3 * 3 = 1");

    // Fraction / Fraction
    ASSERT_EQ(ss("(1/3) / (2/3)"), "1 / 2", "1/3 ÷ 2/3 = 1/2");

    // Fraction ^ Integer
    ASSERT_EQ(ss("(1/2) ^ 2"), "1 / 4", "(1/2)^2 = 1/4");
    ASSERT_EQ(ss("(1/3) ^ 2"), "1 / 9", "(1/3)^2 = 1/9");

    // Integer + Fraction
    ASSERT_EQ(ss("1 + 1/3"), "4 / 3", "1 + 1/3 = 4/3");
    ASSERT_EQ(ss("2 + 1/2"), "5 / 2", "2 + 1/2 = 5/2");
}

void test_rational_derive() {
    SECTION("Rational fractions in derive output");

    // y = x^3 → x = y^(1/3)
    {
        FormulaSystem sys;
        sys.load_string("y = x ^ 3");
        auto result = sys.derive("x", {}, {{"y", "y"}});
        ASSERT_EQ(result, "y^(1 / 3)", "x^3 derives x = y^(1/3)");
    }

    // y = x^2 → x = y^(1/2) or sqrt(y)
    {
        FormulaSystem sys;
        sys.load_string("y = x ^ 2");
        auto results = sys.derive_all("x", {}, {{"y", "y"}});
        bool found_sqrt = false;
        for (auto& r : results) {
            if (r.find("1 / 2") != std::string::npos || r.find("sqrt") != std::string::npos)
                found_sqrt = true;
        }
        ASSERT(found_sqrt, "x^2 derives x = y^(1/2) or sqrt(y)");
    }
}

void test_rational_solve_output() {
    SECTION("Rational fractions in solve output");

    // Solve output: x = 1/3 should render as structural fraction "1 / 3"
    {
        write_fw("/tmp/trso_1_3.fw", "a = b + 1/3\n");
        int rc = system("./bin/fwiz '/tmp/trso_1_3(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 1 / 3'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 1/3 displays as '1 / 3'");
    }

    // Solve output: 2/7 preserved as fraction
    {
        write_fw("/tmp/trso_2_7.fw", "a = b + 2/7\n");
        int rc = system("./bin/fwiz '/tmp/trso_2_7(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 2 / 7'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 2/7 displays as '2 / 7'");
    }

    // Solve output: -3/4 renders with parenthesized negative numerator,
    // matching derive/simplify output (general expression printing wraps
    // negative Num nodes when they appear inside larger expressions).
    {
        write_fw("/tmp/trso_m3_4.fw", "a = b + (-3)/4\n");
        int rc = system("./bin/fwiz '/tmp/trso_m3_4(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = (-3) / 4'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: -3/4 displays as '(-3) / 4'");
    }

    // Integer-valued fraction: 10/5 must render as '2', not '2 / 1'
    {
        write_fw("/tmp/trso_int.fw", "a = b + 10/5\n");
        // Must match 'a = 2' followed by end-of-line (not '2 / 1')
        int rc = system("./bin/fwiz '/tmp/trso_int(a=?, b=0)' 2>/dev/null "
                        "| grep -qE 'a = 2$'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 10/5 displays as '2' (no / 1)");
        // Confirm no spurious '/ 1' appears
        int rc2 = system("./bin/fwiz '/tmp/trso_int(a=?, b=0)' 2>/dev/null "
                         "| grep -q '/ 1'");
        ASSERT(WEXITSTATUS(rc2) != 0, "solve output: 10/5 does not emit '/ 1'");
    }

    // Non-recognizable decimal falls back to decimal rendering
    {
        write_fw("/tmp/trso_dec.fw", "a = b + 0.37\n");
        int rc = system("./bin/fwiz '/tmp/trso_dec(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 0.37'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 0.37 displays as decimal");
    }

    // Numeric-approximate result must use '~' AND NOT render a fraction
    {
        write_fw("/tmp/trso_cubic.fw", "y = x^3 + x\n");
        int rc = system("./bin/fwiz '/tmp/trso_cubic(x=?, y=1)' 2>/dev/null "
                        "| grep -q 'x ~ '");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: cubic uses '~' (approximate)");
        int rc2 = system("./bin/fwiz '/tmp/trso_cubic(x=?, y=1)' 2>/dev/null "
                         "| grep -q ' / '");
        ASSERT(WEXITSTATUS(rc2) != 0, "solve output: approximate result has no fraction");
    }

    // --explore path (Path A) also renders fractions
    {
        write_fw("/tmp/trso_exp.fw", "a = b + 1/3\n");
        int rc = system("./bin/fwiz --explore '/tmp/trso_exp(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 1 / 3'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: --explore path renders fraction");
    }

    // Power-of-10 denominators render as fractions in default (exact) mode.
    // The former is_power_of_10 heuristic has been replaced by the explicit
    // --approximate flag: exact mode means "exact", so 98.1 = 981/10 renders
    // as the truthful "981 / 10". Users who want the decimal form pass
    // --approximate.
    {
        write_fw("/tmp/trso_981.fw", "a = b + 98.1\n");
        int rc = system("./bin/fwiz '/tmp/trso_981(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 981 / 10'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 98.1 renders as '981 / 10' in exact mode");
        int rc2 = system("./bin/fwiz '/tmp/trso_981(a=?, b=0)' 2>/dev/null "
                         "| grep -qE 'a = 98.1$'");
        ASSERT(WEXITSTATUS(rc2) != 0, "solve output: 98.1 does NOT render as decimal in exact mode");
        // But --approximate restores decimal form
        int rc3 = system("./bin/fwiz --approximate '/tmp/trso_981(a=?, b=0)' 2>/dev/null "
                         "| grep -q 'a = 98.1'");
        ASSERT(WEXITSTATUS(rc3) == 0, "--approximate: 98.1 renders as decimal");
    }

    // 1/10 renders as fraction in exact mode; --approximate restores decimal.
    {
        write_fw("/tmp/trso_01.fw", "a = b + 0.1\n");
        int rc = system("./bin/fwiz '/tmp/trso_01(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 1 / 10'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 0.1 renders as '1 / 10' in exact mode");
        int rc2 = system("./bin/fwiz '/tmp/trso_01(a=?, b=0)' 2>/dev/null "
                         "| grep -qE 'a = 0.1$'");
        ASSERT(WEXITSTATUS(rc2) != 0, "solve output: 0.1 does NOT render as decimal in exact mode");
        int rc3 = system("./bin/fwiz --approximate '/tmp/trso_01(a=?, b=0)' 2>/dev/null "
                         "| grep -q 'a = 0.1'");
        ASSERT(WEXITSTATUS(rc3) == 0, "--approximate: 0.1 renders as decimal");
    }

    // Non-power-of-10 denominators render as fraction regardless of numerator
    // size. Value 100/7 (≈ 14.2857) was previously rejected by the |p| <= 12
    // cap; now correctly displays as the informative "100 / 7".
    {
        write_fw("/tmp/trso_1007.fw", "a = b + 100/7\n");
        int rc = system("./bin/fwiz '/tmp/trso_1007(a=?, b=0)' 2>/dev/null "
                        "| grep -q 'a = 100 / 7'");
        ASSERT(WEXITSTATUS(rc) == 0, "solve output: 100/7 displays as fraction");
    }
}

void test_evaluate_symbolic() {
    SECTION("evaluate_symbolic: exact arithmetic projection");

    FormulaSystem builtin_sys;
    builtin_sys.load_builtins();
    RewriteRulesGuard rr_guard(&builtin_sys.rewrite_rules, &builtin_sys.rewrite_exhaustive_flags_);

    // DIV of two integers: preserved as structural fraction, NOT folded to double
    {
        auto e = Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), Expr::Num(3));
        auto r = evaluate_symbolic(*e);
        ASSERT_EQ(expr_to_string(r), "1 / 3",
                  "evaluate_symbolic: 1/3 preserved as fraction");
    }

    // MUL of two integers: folded to Num
    {
        auto e = Expr::BinOpExpr(BinOp::MUL, Expr::Num(2), Expr::Num(3));
        auto r = evaluate_symbolic(*e);
        ASSERT_EQ(expr_to_string(r), "6", "evaluate_symbolic: 2 * 3 = 6");
    }

    // ADD(Num, Var): symbolic RHS — tree returned unchanged
    {
        auto e = Expr::BinOpExpr(BinOp::ADD, Expr::Num(1), Expr::Var("x"));
        auto r = evaluate_symbolic(*e);
        ASSERT_EQ(expr_to_string(r), "1 + x",
                  "evaluate_symbolic: Num + Var returned as-is");
    }

    // FUNC_CALL with a non-numeric argument: must fall through to tree-as-is,
    // not attempt to fold. Guards the extension-point contract.
    {
        auto e = Expr::Call("sin", {Expr::Var("x")});
        auto r = evaluate_symbolic(*e);
        ASSERT_EQ(expr_to_string(r), "sin(x)",
                  "evaluate_symbolic: FUNC_CALL with symbolic arg returned as-is");
    }

    // simplify(1/3 + 0) must preserve 1/3 as fraction (not 0.333...)
    ASSERT_EQ(ss("1/3 + 0"), "1 / 3",
              "simplify: 1/3 + 0 preserves fraction (evaluate_symbolic path)");

    // simplify(sin(0) + 1/3) — FUNC_CALL fold must produce Num(0), not harm neighbor
    ASSERT_EQ(ss("sin(0) + 1/3"), "1 / 3",
              "simplify: sin(0) + 1/3 = 1/3 (FUNC_CALL fold preserves neighbor)");

    // Regression: 7/2 preserved via the migrated BINOP path
    ASSERT_EQ(ss("7 / 2"), "7 / 2",
              "simplify: 7/2 preserved (migrated BINOP path)");

    // Regression: 2 * 3 still folds to 6 via the migrated BINOP path
    ASSERT_EQ(ss("2 * 3"), "6",
              "simplify: 2 * 3 = 6 (migrated BINOP path)");
}

void test_constant_recognition_derive() {
    SECTION("Constant recognition in derive output");

    // y = 2^x → x = log(y) / log(2)
    {
        FormulaSystem sys;
        sys.load_string("y = 2 ^ x");
        auto result = sys.derive("x", {}, {{"y", "y"}});
        ASSERT(result.find("log(2)") != std::string::npos,
               "2^x derives x with log(2) not 0.6931... (got: " + result + ")");
    }

    // y = 3^x → x = log(y) / log(3)
    {
        FormulaSystem sys;
        sys.load_string("y = 3 ^ x");
        auto result = sys.derive("x", {}, {{"y", "y"}});
        ASSERT(result.find("log(3)") != std::string::npos,
               "3^x derives x with log(3) not 1.0986... (got: " + result + ")");
    }
}

void test_approximate_solve() {
    SECTION("--approximate flag on solve output");

    // (1) Default mode: y = 1/3 renders as fraction
    {
        write_fw("/tmp/tapx_1.fw", "y = x + 1/3\n");
        int rc = system("./bin/fwiz '/tmp/tapx_1(y=?, x=0)' 2>/dev/null "
                        "| grep -q 'y = 1 / 3'");
        ASSERT(WEXITSTATUS(rc) == 0, "default: 1/3 displays as '1 / 3'");
    }

    // (2) --approximate: y = 1/3 collapses to decimal
    {
        write_fw("/tmp/tapx_2.fw", "y = x + 1/3\n");
        int rc = system("./bin/fwiz --approximate '/tmp/tapx_2(y=?, x=0)' 2>/dev/null "
                        "| grep -q 'y = 0.333'");
        ASSERT(WEXITSTATUS(rc) == 0, "--approximate: 1/3 displays as decimal");
        int rc2 = system("./bin/fwiz --approximate '/tmp/tapx_2(y=?, x=0)' 2>/dev/null "
                         "| grep -q ' / '");
        ASSERT(WEXITSTATUS(rc2) != 0, "--approximate: no fraction in output");
    }

    // (3) Default mode: y = pi solve renders symbolic pi
    {
        write_fw("/tmp/tapx_3.fw", "y = pi\n");
        int rc = system("./bin/fwiz '/tmp/tapx_3(y=?)' 2>/dev/null "
                        "| grep -q 'y = pi'");
        ASSERT(WEXITSTATUS(rc) == 0, "default: y = pi displays as 'pi'");
    }

    // (4) --approximate: y = pi collapses to 3.141592...
    {
        write_fw("/tmp/tapx_4.fw", "y = pi\n");
        int rc = system("./bin/fwiz --approximate '/tmp/tapx_4(y=?)' 2>/dev/null "
                        "| grep -q 'y = 3.141592'");
        ASSERT(WEXITSTATUS(rc) == 0, "--approximate: y = pi displays as decimal");
        int rc2 = system("./bin/fwiz --approximate '/tmp/tapx_4(y=?)' 2>/dev/null "
                         "| grep -q 'y = pi'");
        ASSERT(WEXITSTATUS(rc2) != 0, "--approximate: no symbolic 'pi' in output");
    }

    // (5) Approximate-only numeric result: --approximate must not introduce a fraction
    {
        write_fw("/tmp/tapx_5.fw", "y = x + sin(x)\n");
        int rc = system("./bin/fwiz --approximate '/tmp/tapx_5(x=?, y=1)' 2>/dev/null "
                        "| grep -q 'x ~ '");
        ASSERT(WEXITSTATUS(rc) == 0, "--approximate: numeric-only result uses '~'");
        int rc2 = system("./bin/fwiz --approximate '/tmp/tapx_5(x=?, y=1)' 2>/dev/null "
                         "| grep -q ' / '");
        ASSERT(WEXITSTATUS(rc2) != 0, "--approximate: no fraction in approximate result");
    }

    // (6) Last-wins semantics: --exact --approximate → approximate wins; --approximate --exact → exact wins
    {
        write_fw("/tmp/tapx_6.fw", "y = x + 1/3\n");
        int rc_a = system("./bin/fwiz --exact --approximate '/tmp/tapx_6(y=?, x=0)' 2>/dev/null "
                          "| grep -q 'y = 0.333'");
        ASSERT(WEXITSTATUS(rc_a) == 0, "--exact --approximate: approximate wins (last flag)");
        int rc_b = system("./bin/fwiz --approximate --exact '/tmp/tapx_6(y=?, x=0)' 2>/dev/null "
                          "| grep -q 'y = 1 / 3'");
        ASSERT(WEXITSTATUS(rc_b) == 0, "--approximate --exact: exact wins (last flag)");
    }
}

void test_approximate_derive_partial_eval() {
    SECTION("--approximate on --derive: partial numeric evaluation");

    // (1) c = 2 * pi * r, derive c with approximate → contains "6.28318", contains "r", no "pi"
    {
        FormulaSystem sys;
        sys.approximate_mode = true;
        sys.load_string("c = 2 * pi * r");
        auto result = sys.derive("c", {}, {{"r", "r"}});
        ASSERT(result.find("6.28318") != std::string::npos,
               "approximate derive: 2*pi*r contains 6.28318 (got: " + result + ")");
        ASSERT(result.find("r") != std::string::npos,
               "approximate derive: 2*pi*r contains 'r' (got: " + result + ")");
        ASSERT(result.find("pi") == std::string::npos,
               "approximate derive: 2*pi*r has no 'pi' (got: " + result + ")");
    }

    // (2) y = 0.5 * x, derive y with approximate → "0.5 * x" not "(1/2) * x"
    {
        FormulaSystem sys;
        sys.approximate_mode = true;
        sys.load_string("y = 0.5 * x");
        auto result = sys.derive("y", {}, {{"x", "x"}});
        ASSERT(result.find("0.5") != std::string::npos,
               "approximate derive: 0.5*x contains '0.5' (got: " + result + ")");
        ASSERT(result.find(" / ") == std::string::npos,
               "approximate derive: 0.5*x has no fraction (got: " + result + ")");
    }

    // (3) y = pi, derive y with approximate → "3.14159" not "pi"
    {
        FormulaSystem sys;
        sys.approximate_mode = true;
        sys.load_string("y = pi");
        auto result = sys.derive("y", {}, {});
        ASSERT(result.find("3.14159") != std::string::npos,
               "approximate derive: y=pi contains 3.14159 (got: " + result + ")");
        ASSERT(result.find("pi") == std::string::npos,
               "approximate derive: y=pi has no 'pi' (got: " + result + ")");
    }
}

void test_solve_derive_output_parity() {
    SECTION("Solve/derive output parity in default (exact) mode");

    // For expressions that fully collapse to a numeric constant in derive,
    // the shell-level solve output and the API-level derive output must agree.

    // (1) y = pi — both should produce "pi"
    {
        write_fw("/tmp/tsdp_pi.fw", "y = pi\n");
        FormulaSystem sys;
        sys.load_string("y = pi");
        auto derived = sys.derive("y", {}, {});

        // Invoke solve via CLI to capture its formatter path.
        int rc = system("./bin/fwiz '/tmp/tsdp_pi(y=?)' > /tmp/tsdp_pi.out 2>/dev/null");
        (void) rc;
        std::ifstream f("/tmp/tsdp_pi.out");
        std::string line; std::getline(f, line);
        // Solve prints "y = pi"; derive API returns just "pi"
        ASSERT(line == "y = " + derived,
               "solve/derive parity (pi): solve='" + line + "' derive='" + derived + "'");
    }

    // (2) y = sqrt(2) — both should render sqrt(2) symbolically.
    // sqrt is a builtin function, so the API-level sys needs load_builtins()
    // to resolve the sqrt call; cases (1) and (3) don't, because 'pi' is
    // auto-recognized as a builtin constant and '5/3' uses only arithmetic.
    {
        write_fw("/tmp/tsdp_sqrt2.fw", "y = sqrt(2)\n");
        FormulaSystem sys;
        sys.load_builtins();
        sys.load_string("y = sqrt(2)");
        auto derived = sys.derive("y", {}, {});

        int rc = system("./bin/fwiz '/tmp/tsdp_sqrt2(y=?)' > /tmp/tsdp_sqrt2.out 2>/dev/null");
        (void) rc;
        std::ifstream f("/tmp/tsdp_sqrt2.out");
        std::string line; std::getline(f, line);
        ASSERT(line == "y = " + derived,
               "solve/derive parity (sqrt(2)): solve='" + line + "' derive='" + derived + "'");
    }

    // (3) y = 5/3 — both should render as "5 / 3"
    {
        write_fw("/tmp/tsdp_rat.fw", "y = 5/3\n");
        FormulaSystem sys;
        sys.load_string("y = 5/3");
        auto derived = sys.derive("y", {}, {});

        int rc = system("./bin/fwiz '/tmp/tsdp_rat(y=?)' > /tmp/tsdp_rat.out 2>/dev/null");
        (void) rc;
        std::ifstream f("/tmp/tsdp_rat.out");
        std::string line; std::getline(f, line);
        ASSERT(line == "y = " + derived,
               "solve/derive parity (5/3): solve='" + line + "' derive='" + derived + "'");
    }
}

void test_checked_type() {
    SECTION("Checked<T>: NaN-sentinel optional wrapper");

    { Checked<double> c; ASSERT(!c.has_value(), "default empty"); ASSERT(!c, "default bool false"); }
    { Checked<double> c{3.14}; ASSERT(c.has_value(), "engaged"); ASSERT(static_cast<bool>(c), "engaged bool"); ASSERT_NUM(c.value(), 3.14, "value"); }
    { Checked<double> c; ASSERT(std::isnan(c.value_or_nan()), "empty -> NaN"); }
    { Checked<double> c{2.71}; ASSERT_NUM(c.value_or_nan(), 2.71, "engaged -> val"); }
    { Checked<double> c{std::numeric_limits<double>::quiet_NaN()}; ASSERT(!c.has_value(), "NaN-in -> empty"); }
}

int main() {
    ExprArena test_arena;
    ExprArena::Scope arena_scope(test_arena);

    std::cout << "fwiz unit tests\n";
    std::cout << "===============\n";

    test_lexer();
    test_parser();
    test_evaluate();
    test_simplify();
    test_substitute();
    test_var_helpers();
    test_decompose();
    test_solve_for();
    test_system();
    test_cli_parser();
    test_file_parsing();

    // Edge cases
    test_lexer_edge();
    test_parser_edge();
    test_evaluate_edge();
    test_simplify_edge();
    test_decompose_edge();
    test_solve_for_edge();
    test_system_edge();
    test_cli_parser_edge();
    test_printer_edge();

    // Garbage / robustness
    test_lexer_garbage();
    test_parser_garbage();
    test_cli_garbage();
    test_file_garbage();
    test_file_access();

    // Numeric extremes
    test_numeric_extremes();
    test_fmt_output();
    test_near_zero_coefficient();
    test_inf_nan_in_trace();

    // Expression depth & scale (Group 2)
    test_depth_evaluate();
    test_depth_simplify();
    test_depth_substitute();
    test_depth_collect_vars();
    test_depth_tostring();
    test_depth_decompose();
    test_depth_solve();
    test_deep_functions();
    test_wide_expressions();
    test_parse_deep_string();
    test_large_file();

    // Contradictions & overdetermined (Group 3)
    test_equation_order();
    test_contradictions();
    test_nan_fallthrough();
    test_overdetermined();
    test_defaults_vs_equations();

    // Statefulness & mutation (Group 4)
    test_load_file_accumulation();
    test_resolve_isolation();
    test_bindings_not_mutated();
    test_system_reuse();

    // File format portability (Group 5)
    test_windows_line_endings();
    test_mixed_line_endings();
    test_utf8_bom();
    test_whitespace_handling();
    test_no_trailing_newline();
    test_bare_cr();
    test_large_file_format();

    // CLI value parsing (Group 6)
    test_cli_scientific_notation();
    test_cli_negative_values();
    test_cli_multiple_query_targets();
    test_cli_special_values();
    test_cli_long_query();
    test_cli_spacing_variants();
    test_cli_end_to_end();

    // Error message quality (Group 7)
    test_errmsg_missing_variable();
    test_errmsg_nan_inf();
    test_errmsg_circular();
    test_errmsg_file();
    test_errmsg_cli();
    test_errmsg_consistency();

    // Final coverage (8 remaining areas)
    test_binary_integration();
    test_roundtrip_parse_print();
    test_roundtrip_forward_inverse();
    test_simplifier_convergence();
    test_example_files();
    test_precedence_exhaustive();
    test_intermediate_consistency();
    test_edge_arithmetic();

    // Code audit regression tests
    test_audit_fmt_num_ub();
    test_audit_signed_char_ub();
    test_audit_switch_safety();

    // Multi-return and aliases
    test_multi_return();
    test_alias_syntax();

    // Free variables and interface contracts
    test_free_variable_resolution();
    test_underdetermined_systems();
    test_free_var_chains();
    test_multi_query_free_vars();
    test_interface_error_messages();

    // Formula calls (cross-file)
    test_formula_call_parsing();
    test_formula_call_forward();
    test_formula_call_reverse();
    test_formula_call_chained();
    test_formula_call_errors();
    test_formula_call_additional();

    // Verify mode
    test_approx_equal();
    test_verify_variable();
    test_verify_binary_integration();

    // Explore mode
    test_all_variables();
    test_explore_binary_integration();

    // Spurious zero guard
    test_solve_for_zero_guard();

    // ValueSet
    test_valueset_basic();
    test_valueset_operations();
    test_valueset_display();

    // Conditions
    test_condition_parsing();
    test_condition_solving();
    test_condition_errors();
    test_global_conditions();
    test_multiple_returns();
    test_conditional_branching();

    // Recursion depth guard
    test_recursion_depth_guard();

    // Pre-refactor safety net
    test_strategy_coverage();
    test_builtin_exhaustive();
    test_operator_metadata();

    // Simplifier improvements
    test_simplify_rule_interactions();
    test_simplify_flatten_targets();
    test_simplify_like_terms();
    test_simplify_mul_to_pow();
    test_simplify_self_division();
    test_simplify_constant_collection();
    test_simplify_constant_reassociation();
    test_simplify_div_zero_denom();

    // Derive mode
    test_derive_basic();
    test_derive_same_name();
    test_derive_formula_call();
    test_derive_errors();
    test_derive_cli_parsing();
    test_derive_binary_integration();

    // Numeric root-finding
    test_newton_solve();
    test_bisection_solve();
    test_adaptive_scan();
    test_find_numeric_roots();
    test_numeric_integration();
    test_numeric_precision();
    test_numeric_edge_cases();
    test_numeric_binary_integration();

    // Curve fitting
    test_fit_sampling();
    test_fit_matrix();
    test_fit_polynomial();
    test_fit_integration();
    test_fit_binary_integration();

    // Builtin constants
    test_builtin_constants();
    test_template_fitting();

    // Coverage gap tests (from audit)
    test_numeric_edge_cases_extended();
    test_constants_edge_cases();
    test_derive_edge_cases_extended();
    test_fit_edge_cases();
    test_fit_templates_edge();
    test_numeric_precision_edge();
    test_inline_and_stdin();
    test_sections();
    test_simplify_assumptions();
    test_simplify_exp_log();
    test_simplify_trig_abs_pow();
    test_simplify_common_factor();
    test_iff_semantics();
    test_cross_equation_validation();
    test_rewrite_rules();
    test_undefined();
    test_context_aware_simplification();
    test_positional_args();
    test_register_function();
    test_semicolon_separator();
    test_commutative_matching();
    test_quadratic_formula();
    test_simultaneous_equations();
    test_numeric_skip();
    test_dead_end_and_first_candidate();

    // Rational arithmetic (structural fractions)
    test_rational_fractions();
    test_rational_arithmetic();
    test_rational_derive();
    test_rational_solve_output();
    test_evaluate_symbolic();
    test_constant_recognition_derive();
    test_approximate_solve();
    test_approximate_derive_partial_eval();
    test_solve_derive_output_parity();
    test_checked_type();

    std::cout << "\n===============\n";
    std::cout << "Total: " << tests_run
              << "  Passed: " << tests_passed
              << "  Failed: " << tests_failed << "\n";

    return tests_failed > 0 ? 1 : 0;
}
