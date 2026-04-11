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

double ev(const std::string& s) {
    return evaluate(simplify(parse(s)));
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

    // Division by zero
    {
        bool threw = false;
        try { ev("1/0"); } catch (...) { threw = true; }
        ASSERT(threw, "division by zero throws");
    }
    // Unresolved variable
    {
        bool threw = false;
        try { ev("x + 1"); } catch (...) { threw = true; }
        ASSERT(threw, "unresolved variable throws");
    }
}

// ---- Simplifier tests ----

void test_simplify() {
    SECTION("Simplifier");

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
        ASSERT_EQ(expr_to_string(simplify(e)), "y - x", "(-x) + y => y - x");
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
        ASSERT_EQ(expr_to_string(simplify(e)), "b - a", "-(a-b) => b-a");
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
    ASSERT_EQ(expr_to_string(simplify(r1)), "10 + y * 2", "sub x=10");

    auto r2 = substitute(r1, "y", Expr::Num(3));
    ASSERT_EQ(expr_to_string(simplify(r2)), "16", "sub x=10,y=3 => 16");

    // Substitute with expression
    auto r3 = substitute(e, "x", parse("a + b"));
    ASSERT_EQ(expr_to_string(r3), "a + b + y * 2", "sub x=(a+b)");

    // Substitute in function call
    auto e2 = parse("sqrt(x^2 + y^2)");
    auto r4 = substitute(e2, "x", Expr::Num(3));
    auto r5 = substitute(r4, "y", Expr::Num(4));
    ASSERT_NUM(evaluate(simplify(r5)), 5, "sub in sqrt(3^2+4^2)=5");

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
        ASSERT(lf.ok, "y+5 is linear in y");
        ASSERT_NUM(evaluate(lf.coeff), 1, "y+5 coeff=1");
        ASSERT_NUM(evaluate(lf.rest), 5, "y+5 rest=5");
    }

    // y * 2 - 5: coeff=2, rest=-5
    {
        auto lf = decompose_linear(parse("y * 2 - 5"), "y");
        ASSERT(lf.ok, "y*2-5 is linear in y");
        ASSERT_NUM(evaluate(lf.coeff), 2, "y*2-5 coeff=2");
        ASSERT_NUM(evaluate(lf.rest), -5, "y*2-5 rest=-5");
    }

    // y + 3 * y: coeff=4, rest=0
    {
        auto lf = decompose_linear(parse("y + 3 * y"), "y");
        ASSERT(lf.ok, "y+3*y is linear in y");
        ASSERT_NUM(evaluate(lf.coeff), 4, "y+3*y coeff=4");
        ASSERT_NUM(evaluate(lf.rest), 0, "y+3*y rest=0");
    }

    // speed * time: linear in time (coeff=speed), but speed is a var
    {
        auto lf = decompose_linear(parse("speed * time"), "time");
        ASSERT(lf.ok, "speed*time is linear in time");
        ASSERT_EQ(expr_to_string(lf.coeff), "speed", "coeff=speed");
        ASSERT_NUM(evaluate(lf.rest), 0, "rest=0");
    }

    // y * y is nonlinear
    {
        auto lf = decompose_linear(parse("y * y"), "y");
        ASSERT(!lf.ok, "y*y is nonlinear");
    }

    // sqrt(y) is nonlinear
    {
        auto lf = decompose_linear(parse("sqrt(y)"), "y");
        ASSERT(!lf.ok, "sqrt(y) is nonlinear");
    }

    // Expression with no target var: coeff=0
    {
        auto lf = decompose_linear(parse("a + b"), "z");
        ASSERT(lf.ok, "a+b linear in z (trivially)");
        ASSERT_NUM(evaluate(lf.coeff), 0, "coeff=0");
    }

    // Negated variable: -y => coeff=-1
    {
        auto lf = decompose_linear(parse("-y"), "y");
        ASSERT(lf.ok, "-y is linear");
        ASSERT_NUM(evaluate(lf.coeff), -1, "-y coeff=-1");
    }

    // Division: y / 3 => coeff=1/3
    {
        auto lf = decompose_linear(parse("y / 3"), "y");
        ASSERT(lf.ok, "y/3 is linear");
        ASSERT_NUM(evaluate(lf.coeff), 1.0/3.0, "y/3 coeff=1/3");
    }

    // Target in denominator is nonlinear
    {
        auto lf = decompose_linear(parse("1 / y"), "y");
        ASSERT(!lf.ok, "1/y is nonlinear");
    }
}

// ---- solve_for tests ----

void test_solve_for() {
    SECTION("Algebraic Solver (solve_for)");

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

    // Nonlinear: x = y^2 => cannot solve
    {
        auto sol = solve_for(Expr::Var("x"), parse("y^2"), "y");
        ASSERT(sol == nullptr, "cannot solve y^2 for y");
    }

    // Solve for x when x is on the LHS already: x = a + b => x = a + b
    {
        auto sol = solve_for(Expr::Var("x"), parse("a + b"), "x");
        ASSERT(sol != nullptr, "can solve for x on LHS");
        ASSERT_EQ(expr_to_string(sol), "a + b", "x = a + b");
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
        ASSERT_EQ(q.queries[0].first, "distance", "solve_for = distance");
        ASSERT_NUM(q.bindings.at("time"), 5, "time=5 binding");
    }
    {
        auto q = parse_cli_query("test.fw(x=?, y=3, z=10)");
        ASSERT_EQ(q.filename, "test.fw", "filename already has .fw");
        ASSERT_EQ(q.queries[0].first, "x", "solve_for = x");
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
        ASSERT_NUM(evaluate(v), 256, "2^2^3 = 2^8 = 256");
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
        double r = ev("sqrt(-1)");
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

    // Unknown function
    {
        bool threw = false;
        try { ev("foobar(1)"); } catch (...) { threw = true; }
        ASSERT(threw, "unknown function throws");
    }
}

void test_simplify_edge() {
    SECTION("Simplifier Edge Cases");

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
    ASSERT_EQ(ss("x * 2 * 3 * 4"), "x * 24", "chain fold x*2*3*4");

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
        ASSERT_EQ(expr_to_string(simplify(e)), "-(a / b)", "(-a)/b => -(a/b)");
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
        ASSERT(lf.ok, "no z present: ok");
        ASSERT_NUM(evaluate(lf.coeff), 0, "no z: coeff=0");
    }

    // 0 * y => coeff=0 (zero coefficient)
    {
        auto lf = decompose_linear(parse("0 * y"), "y");
        ASSERT(lf.ok, "0*y is linear");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), 0, "0*y coeff=0");
    }

    // Subtraction of same var: y - y => coeff=0
    {
        auto lf = decompose_linear(parse("y - y"), "y");
        ASSERT(lf.ok, "y-y is linear");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), 0, "y-y coeff=0");
    }

    // Complex coefficient: (a + b) * y
    {
        auto lf = decompose_linear(parse("(a + b) * y"), "y");
        ASSERT(lf.ok, "(a+b)*y is linear in y");
        ASSERT_EQ(expr_to_string(lf.coeff), "a + b", "(a+b)*y coeff=a+b");
    }

    // y appears in add and mul: 2*y + 3*y + y => coeff=6
    {
        auto lf = decompose_linear(parse("2*y + 3*y + y"), "y");
        ASSERT(lf.ok, "2y+3y+y is linear");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), 6, "2y+3y+y coeff=6");
    }

    // y in nested linear: (y + 1) * 2 - y => coeff=1, rest=2
    {
        auto lf = decompose_linear(parse("(y + 1) * 2 - y"), "y");
        ASSERT(lf.ok, "(y+1)*2-y is linear");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), 1, "(y+1)*2-y coeff=1");
        ASSERT_NUM(evaluate(simplify(lf.rest)), 2, "(y+1)*2-y rest=2");
    }

    // Negative coefficient: 5 - 3*y => coeff=-3, rest=5
    {
        auto lf = decompose_linear(parse("5 - 3*y"), "y");
        ASSERT(lf.ok, "5-3y is linear");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), -3, "5-3y coeff=-3");
        ASSERT_NUM(evaluate(simplify(lf.rest)), 5, "5-3y rest=5");
    }

    // y in exponent is nonlinear
    {
        auto lf = decompose_linear(parse("2^y"), "y");
        ASSERT(!lf.ok, "2^y is nonlinear");
    }

    // y in function arg is nonlinear
    {
        auto lf = decompose_linear(parse("sin(y) + 1"), "y");
        ASSERT(!lf.ok, "sin(y)+1 is nonlinear");
    }

    // Pure constant expression
    {
        auto lf = decompose_linear(Expr::Num(42), "y");
        ASSERT(lf.ok, "constant is linear (trivially)");
        ASSERT_NUM(evaluate(lf.coeff), 0, "constant coeff=0");
        ASSERT_NUM(evaluate(lf.rest), 42, "constant rest=42");
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
        ASSERT_NUM(evaluate(simplify(sol)), 5, "x = 5 => x = 5");
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
        ASSERT_NUM(evaluate(simplify(val)), 18, "y/3+2: x=8 => y=18");
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
        ASSERT_EQ(q.queries[0].first, "x", "spaces: solve_for");
        ASSERT_NUM(q.bindings.at("y"), 5, "spaces: binding");
    }

    // Only the query variable, no other bindings
    {
        auto q = parse_cli_query("f(x=?)");
        ASSERT_EQ(q.queries[0].first, "x", "single var query");
        ASSERT(q.bindings.empty(), "no bindings");
    }

    // Many bindings
    {
        auto q = parse_cli_query("f(z=?, a=1, b=2, c=3, d=4, e=5)");
        ASSERT_EQ(q.queries[0].first, "z", "many bindings: solve_for");
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
        ASSERT_EQ(q.queries[0].first, "x", "first query is x");
        ASSERT_EQ(q.queries[1].first, "y", "second query is y");
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
        ASSERT(msg.find("Invalid number") != std::string::npos,
            "non-numeric: clear error message");
    }
    {
        bool threw = false;
        std::string msg;
        try { parse_cli_query("f(x=y=5)"); } catch (const std::exception& e) {
            threw = true; msg = e.what();
        }
        ASSERT(threw, "multiple equals throws");
        ASSERT(msg.find("Invalid number") != std::string::npos,
            "multiple equals: clear error about bad value");
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
        ASSERT(std::isinf(evaluate(e)), "inf + 1 = inf");
    }
    {
        auto e = Expr::BinOpExpr(BinOp::MUL,
            Expr::Num(std::numeric_limits<double>::infinity()), Expr::Num(0));
        ASSERT(std::isnan(evaluate(e)), "inf * 0 = NaN");
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
        double r = ev("sqrt(-1)");
        ASSERT(std::isnan(r), "sqrt(-1) = NaN");
    }

    // log(-1) produces NaN
    {
        double r = ev("log(-1)");
        ASSERT(std::isnan(r), "log(-1) = NaN");
    }

    // (-1)^0.5 produces NaN
    {
        // Parser reads this as -(1^0.5) = -1, not (-1)^0.5
        // Build it manually
        auto e = Expr::BinOpExpr(BinOp::POW, Expr::Num(-1), Expr::Num(0.5));
        ASSERT(std::isnan(evaluate(e)), "(-1)^0.5 = NaN");
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
        ASSERT(std::isnan(evaluate(e)), "NaN + 5 = NaN");
    }

    // --- Division by zero ---

    // 0/0 throws
    {
        bool threw = false;
        try { ev("0 / 0"); } catch (...) { threw = true; }
        ASSERT(threw, "0/0 throws division by zero");
    }

    // 1/0 throws
    {
        bool threw = false;
        try { ev("1 / 0"); } catch (...) { threw = true; }
        ASSERT(threw, "1/0 throws division by zero");
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
        ASSERT_NUM(evaluate(e), 100, "depth 100: evaluate = 100");
    }
    {
        auto e = substitute(build_deep_add(1000), "x", Expr::Num(0));
        ASSERT_NUM(evaluate(e), 1000, "depth 1000: evaluate = 1000");
    }
    {
        auto e = substitute(build_deep_add(DEPTH_HIGH), "x", Expr::Num(0));
        ASSERT_NUM(evaluate(e), DEPTH_HIGH, "depth HIGH: evaluate");
    }

    // With a non-zero base value
    {
        auto e = substitute(build_deep_add(DEPTH_MED), "x", Expr::Num(42));
        ASSERT_NUM(evaluate(e), DEPTH_MED + 42, "depth MED: x=42, evaluate");
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
        ASSERT_NUM(evaluate(simplify(v)), 1000, "depth 1000: simplify then eval = 1000");
    }
}

void test_depth_substitute() {
    SECTION("Depth: Substitute");

    // Substitute at depth
    {
        auto e = build_deep_add(1000);
        auto s = substitute(e, "x", Expr::Num(7));
        ASSERT_NUM(evaluate(s), 1007, "depth 1000: sub x=7, eval = 1007");
    }
    {
        auto e = build_deep_add(DEPTH_HIGH);
        auto s = substitute(e, "x", Expr::Num(0));
        ASSERT_NUM(evaluate(s), DEPTH_HIGH, "depth HIGH: sub and eval");
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
        ASSERT(lf.ok, "depth 1000: linear in x");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), 1, "depth 1000: coeff=1");
        ASSERT_NUM(evaluate(simplify(lf.rest)), 1000, "depth 1000: rest=1000");
    }
    {
        auto e = build_deep_add(DEPTH_MED);
        auto lf = decompose_linear(e, "x");
        ASSERT(lf.ok, "depth MED: linear in x");
        ASSERT_NUM(evaluate(simplify(lf.coeff)), 1, "depth MED: coeff=1");
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
        ASSERT_NUM(evaluate(simplify(val)), 500, "depth 1000: y=1500 => x=500");
    }
}

void test_deep_functions() {
    SECTION("Depth: Nested Function Calls");

    // sqrt(sqrt(sqrt(...(x)...))) at moderate depth
    {
        auto e = build_deep_func(10);
        auto s = substitute(e, "x", Expr::Num(1));
        // sqrt^10(1) = 1
        ASSERT_NUM(evaluate(s), 1, "sqrt^10(1) = 1");
    }
    {
        // sqrt^20(1e300) — repeated sqrt of a large number converges to 1
        auto e = build_deep_func(100);
        auto s = substitute(e, "x", Expr::Num(1e300));
        double r = evaluate(s);
        ASSERT(r > 0.99 && r < 1.01, "sqrt^100(1e300) converges near 1");
    }
    {
        // Deep nested functions at depth 1000
        auto e = build_deep_func(DEPTH_MED);
        auto s = substitute(e, "x", Expr::Num(1));
        ASSERT_NUM(evaluate(s), 1, "sqrt^1000(1) = 1");
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
        ASSERT_NUM(evaluate(simplify(e)), n, "500 vars substituted and summed");
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
        ASSERT_NUM(evaluate(v), 100, "parse depth 100: eval = 100");
    }
    {
        auto s = build_deep_parse_string(1000);
        auto e = parse(s);
        auto v = substitute(e, "x", Expr::Num(0));
        ASSERT_NUM(evaluate(v), 1000, "parse depth 1000: eval = 1000");
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
        ASSERT_NUM(evaluate(v), 1000, "parse flat 1000 terms: eval = 1000");
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
        double r = sys.resolve(q.queries[0].first, q.bindings);
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

    // Space between minus and digit: invalid
    {
        bool threw = false;
        try { parse_cli_query("f(x=?, y=- 3)"); } catch (...) { threw = true; }
        ASSERT(threw, "'- 3' is invalid (space in number)");
    }

    // Double minus: invalid
    {
        bool threw = false;
        try { parse_cli_query("f(x=?, y=--3)"); } catch (...) { threw = true; }
        ASSERT(threw, "'--3' is invalid");
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
        ASSERT_EQ(q.queries[0].first, "x", "first query is x");
        ASSERT_EQ(q.queries[1].first, "y", "second query is y");
        ASSERT(q.bindings.empty(), "no bindings");
    }

    // Three queries
    {
        auto q = parse_cli_query("f(x=?, y=?, z=?)");
        ASSERT(q.queries.size() == 3, "three queries");
        ASSERT_EQ(q.queries[0].first, "x", "first is x");
        ASSERT_EQ(q.queries[1].first, "y", "second is y");
        ASSERT_EQ(q.queries[2].first, "z", "third is z");
    }

    // Query mixed with bindings
    {
        auto q = parse_cli_query("f(x=?, y=5)");
        ASSERT(q.queries.size() == 1, "one query");
        ASSERT_EQ(q.queries[0].first, "x", "x=? is query");
        ASSERT_NUM(q.bindings.at("y"), 5, "y=5 is binding");
    }

    // Binding then query
    {
        auto q = parse_cli_query("f(y=5, x=?)");
        ASSERT_EQ(q.queries[0].first, "x", "x=? after binding");
        ASSERT_NUM(q.bindings.at("y"), 5, "y=5 before query");
    }

    // Aliases: x=?ax means solve x, call it ax
    {
        auto q = parse_cli_query("f(x=?ax, y=?by)");
        ASSERT(q.queries.size() == 2, "two aliased queries");
        ASSERT_EQ(q.queries[0].first, "x", "first var is x");
        ASSERT_EQ(q.queries[0].second, "ax", "first alias is ax");
        ASSERT_EQ(q.queries[1].first, "y", "second var is y");
        ASSERT_EQ(q.queries[1].second, "by", "second alias is by");
    }

    // Bare ? defaults alias to variable name
    {
        auto q = parse_cli_query("f(x=?)");
        ASSERT_EQ(q.queries[0].first, "x", "bare ?: var is x");
        ASSERT_EQ(q.queries[0].second, "x", "bare ?: alias defaults to x");
    }

    // Mixed aliased and bare queries
    {
        auto q = parse_cli_query("f(x=?result, y=?, m=5)");
        ASSERT(q.queries.size() == 2, "mixed: two queries");
        ASSERT_EQ(q.queries[0].second, "result", "first aliased to result");
        ASSERT_EQ(q.queries[1].second, "y", "second bare, alias=y");
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
        ASSERT_EQ(parsed.queries[0].first, "target", "100 bindings: solve target");
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
        ASSERT_EQ(q.queries[0].first, var, "500-char variable name");
    }
}

void test_cli_spacing_variants() {
    SECTION("CLI: Spacing Variants");

    // No spaces at all
    {
        auto q = parse_cli_query("f(x=?,y=5,z=10)");
        ASSERT_EQ(q.queries[0].first, "x", "no spaces: solve x");
        ASSERT_NUM(q.bindings.at("y"), 5, "no spaces: y=5");
        ASSERT_NUM(q.bindings.at("z"), 10, "no spaces: z=10");
    }

    // Lots of spaces
    {
        auto q = parse_cli_query("f(  x = ?  ,  y = 5  ,  z = 10  )");
        ASSERT_EQ(q.queries[0].first, "x", "many spaces: solve x");
        ASSERT_NUM(q.bindings.at("y"), 5, "many spaces: y=5");
        ASSERT_NUM(q.bindings.at("z"), 10, "many spaces: z=10");
    }

    // Tabs
    {
        auto q = parse_cli_query("f(\tx\t=\t?\t,\ty\t=\t5\t)");
        ASSERT_EQ(q.queries[0].first, "x", "tabs: solve x");
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
        double r = sys.resolve(q.queries[0].first, q.bindings);
        double expected = 299792458.0 * 299792458.0;
        ASSERT(std::abs(r - expected) / expected < 1e-10, "E=mc^2 with c=299792458");
    }

    // Negative value in actual computation
    {
        write_fw("/tmp/tc6e2.fw", "y = x + 10\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc6e2.fw");
        auto q = parse_cli_query("tc6e2(y=?, x=-5)");
        double r = sys.resolve(q.queries[0].first, q.bindings);
        ASSERT_NUM(r, 5, "negative input: -5 + 10 = 5");
    }

    // Multiple bindings end-to-end
    {
        write_fw("/tmp/tc6e3.fw", "result = a * b + c * d\n");
        FormulaSystem sys;
        sys.load_file("/tmp/tc6e3.fw");
        auto q = parse_cli_query("tc6e3(result=?, a=2, b=3, c=4, d=5)");
        double r = sys.resolve(q.queries[0].first, q.bindings);
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
        double r1 = evaluate(simplify(v1)), r2 = evaluate(simplify(v2));
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
        ASSERT_EQ(expr_to_string(s), "b - a", "-(a-b) settles to b - a");
        // Verify no oscillation: simplifying again gives same result
        auto s2 = simplify(s);
        ASSERT_EQ(expr_to_string(s2), "b - a", "b - a is stable");
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
        double r = evaluate(simplify(e));
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

    // x/x with x=0 throws division by zero
    {
        auto e = parse("x / x");
        e = substitute(e, "x", Expr::Num(0));
        bool threw = false;
        try { evaluate(e); } catch (...) { threw = true; }
        ASSERT(threw, "x/x with x=0: division by zero");
    }

    // x/x with x=5 evaluates to 1
    {
        auto e = parse("x / x");
        e = substitute(e, "x", Expr::Num(5));
        ASSERT_NUM(evaluate(e), 1, "x/x with x=5 = 1");
    }

    // 0*x + 0*y simplifies to 0
    ASSERT_EQ(ss("0 * x + 0 * y"), "0", "0*x + 0*y = 0");

    // Very small difference
    {
        auto e = parse("x - y");
        e = substitute(e, "x", Expr::Num(1.0000000001));
        e = substitute(e, "y", Expr::Num(1.0));
        double r = evaluate(simplify(e));
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
        ASSERT_NUM(evaluate(Expr::Num(42)), 42, "evaluate NUM");
        ASSERT_NUM(evaluate(Expr::Neg(Expr::Num(5))), -5, "evaluate NEG");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::ADD, Expr::Num(2), Expr::Num(3))), 5, "evaluate ADD");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::SUB, Expr::Num(5), Expr::Num(3))), 2, "evaluate SUB");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::MUL, Expr::Num(4), Expr::Num(3))), 12, "evaluate MUL");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::DIV, Expr::Num(6), Expr::Num(2))), 3, "evaluate DIV");
        ASSERT_NUM(evaluate(Expr::BinOpExpr(BinOp::POW, Expr::Num(2), Expr::Num(3))), 8, "evaluate POW");
        ASSERT_NUM(evaluate(Expr::Call("sqrt", {Expr::Num(16)})), 4, "evaluate FUNC");
    }

    // Verify unresolved variable throws (not silent 0)
    {
        bool threw = false;
        try { evaluate(Expr::Var("x")); } catch (...) { threw = true; }
        ASSERT(threw, "evaluate VAR throws (not silent 0)");
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
        double x = sys.resolve(q.queries[0].first, q.bindings);
        double y = sys.resolve(q.queries[1].first, q.bindings);
        ASSERT_NUM(x, 8, "x = m+4 = 8");
        ASSERT_NUM(y, 1, "y = m-3 = 1");
    }

    // Aliases don't affect resolution — only output naming
    {
        auto q = parse_cli_query("f(x=?result, m=4)");
        ASSERT_EQ(q.queries[0].first, "x", "alias: resolves x");
        ASSERT_EQ(q.queries[0].second, "result", "alias: outputs as result");
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
        ASSERT_EQ(q.queries[0].second, "first", "first alias");
        ASSERT_EQ(q.queries[1].second, "second", "second alias");
    }
}

void test_alias_syntax() {
    SECTION("Alias Syntax Parsing");

    // Bare ? — alias defaults to variable name
    {
        auto q = parse_cli_query("f(x=?)");
        ASSERT_EQ(q.queries[0].first, "x", "bare: var=x");
        ASSERT_EQ(q.queries[0].second, "x", "bare: alias=x");
    }

    // Named alias
    {
        auto q = parse_cli_query("f(x=?myname)");
        ASSERT_EQ(q.queries[0].first, "x", "named: var=x");
        ASSERT_EQ(q.queries[0].second, "myname", "named: alias=myname");
    }

    // Alias with underscores and digits
    {
        auto q = parse_cli_query("f(x=?my_var_2)");
        ASSERT_EQ(q.queries[0].second, "my_var_2", "alias with underscore+digits");
    }

    // Multiple aliases in one query
    {
        auto q = parse_cli_query("f(x=?a, y=?b, z=?c, m=5)");
        ASSERT(q.queries.size() == 3, "three aliased queries");
        ASSERT_EQ(q.queries[0].second, "a", "alias a");
        ASSERT_EQ(q.queries[1].second, "b", "alias b");
        ASSERT_EQ(q.queries[2].second, "c", "alias c");
        ASSERT_NUM(q.bindings.at("m"), 5, "binding m=5");
    }

    // Alias with spaces around it
    {
        auto q = parse_cli_query("f( x = ?alias , m = 5 )");
        ASSERT_EQ(q.queries[0].first, "x", "spaces: var=x");
        ASSERT_EQ(q.queries[0].second, "alias", "spaces: alias preserved");
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
        ASSERT_EQ(sys.formula_calls[0].bindings.at("height"), "depth", "form1: height=depth");
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
        ASSERT_EQ(sys.formula_calls[0].bindings.at("width"), "width", "shorthand: width=width");
        ASSERT_EQ(sys.formula_calls[0].bindings.at("height"), "depth", "explicit: height=depth");
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
        ASSERT_EQ(sys.formula_calls[0].bindings.at("height"), "depth",
            "last-binding: height=depth parsed correctly");
    }

    // Trailing shorthand binding (single ident before closing paren)
    {
        write_fw("/tmp/fca_trail.fw", "fca_bound(area=?, width)\n");
        FormulaSystem sys;
        sys.load_file("/tmp/fca_trail.fw");
        ASSERT(sys.formula_calls[0].bindings.count("width"), "trailing shorthand: has width");
        ASSERT_EQ(sys.formula_calls[0].bindings.at("width"), "width",
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
        double val = evaluate(sol);
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
        double val = evaluate(sol);
        ASSERT_NUM(val, -3, "numeric coeff nonzero rest: c=-3");
    }
}

// ---- Simplifier improvement tests ----

void test_simplify_rule_interactions() {
    SECTION("Simplifier: Rule Interactions");

    auto ss = [](const char* s) { return expr_to_string(simplify(parse(s))); };

    // Like-terms + constant reassociation: x + 2*x + 3 + 4*x + 1
    // After refactor: "7 * x + 4"
    {
        auto e = simplify(parse("x + 2 * x + 3 + 4 * x + 1"));
        double val = evaluate(substitute(e, "x", Expr::Num(10)));
        ASSERT_NUM(val, 74, "like-terms + constant: x+2x+3+4x+1 at x=10 → 74");
    }

    // Power mul then like-term: x^2 + x*x + 3*x^2
    // x*x → x^2 (mul-to-pow), then x^2 + x^2 + 3*x^2 → 5*x^2 (like-terms)
    ASSERT_EQ(ss("x^2 + x * x + 3 * x^2"), "5 * x^2",
        "pow-mul then like-term: x^2+x*x+3x^2 → 5x^2");

    // Symmetric self-division: (2*x) / (2*x) → 1
    ASSERT_EQ(ss("(2 * x) / (2 * x)"), "1", "(2x)/(2x) → 1");

    // Negation of subtraction then subtract: -(a-b) - c → b - a - c
    ASSERT_EQ(ss("-(a - b) - c"), "b - a - c", "-(a-b)-c → b-a-c");

    // Division then multiply back: x / 2 * 2 → x
    ASSERT_EQ(ss("x / 2 * 2"), "x", "x/2*2 → x");

    // Deep mixed chain: ((x/2)*3 - 4)*2 + 5
    // x/2*3 → x*1.5, (x*1.5 - 4)*2 → 2*x*1.5 - 8 → x*3 - 8, +5 → x*3 - 3
    {
        auto e = simplify(parse("((x / 2) * 3 - 4) * 2 + 5"));
        double val = evaluate(substitute(e, "x", Expr::Num(10)));
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
        double val = evaluate(substitute(e, "x", Expr::Num(2)));
        ASSERT_NUM(val, 96, "x^2*2*x^2*3 at x=2 → 96 (6*16)");
    }

    // Chain: 2*x + 3*x - x → 4*x
    ASSERT_EQ(ss("2 * x + 3 * x - x"), "4 * x", "2x+3x-x → 4x");

    // Negation chain: -(-(-x)) → -x
    ASSERT_EQ(ss("-(-(-x))"), "-x", "triple negation → -x");

    // Mixed div/mul reassociation: ((x / 2) * 3) / 3
    // After refactor: "x / 2"
    {
        auto e = simplify(parse("((x / 2) * 3) / 3"));
        double val = evaluate(substitute(e, "x", Expr::Num(10)));
        ASSERT_NUM(val, 5, "((x/2)*3)/3 at x=10 → 5");
    }
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

    // Multiple like-terms across a chain: 2*x + y + 3*x → 5*x + y
    {
        auto e = simplify(parse("2 * x + y + 3 * x"));
        double val = evaluate(substitute(substitute(e, "x", Expr::Num(10)), "y", Expr::Num(1)));
        ASSERT_NUM(val, 51, "2x+y+3x at x=10,y=1 → 51");
    }

    // Constants scattered: 3 + x + 2 + y + 1 → x + y + 6
    {
        auto e = simplify(parse("3 + x + 2 + y + 1"));
        double val = evaluate(substitute(substitute(e, "x", Expr::Num(10)), "y", Expr::Num(20)));
        ASSERT_NUM(val, 36, "3+x+2+y+1 at x=10,y=20 → 36");
    }

    // Multiplicative flattening: collect all factors
    // a * b / a → b (cancel across non-adjacent)
    {
        auto e = simplify(parse("a * b / a"));
        double val = evaluate(substitute(substitute(e, "a", Expr::Num(5)), "b", Expr::Num(3)));
        ASSERT_NUM(val, 3, "a*b/a at a=5,b=3 → 3");
    }

    // Constants scattered in multiplication: 2 * x * 3 * y * 4 → 24*x*y
    {
        auto e = simplify(parse("2 * x * 3 * y * 4"));
        double val = evaluate(substitute(substitute(e, "x", Expr::Num(1)), "y", Expr::Num(1)));
        ASSERT_NUM(val, 24, "2*x*3*y*4 at x=1,y=1 → 24");
    }

    // Mixed: x * y / x * z → y * z (cancel x across mul/div chain)
    {
        auto e = simplify(parse("x * y / x * z"));
        double val = evaluate(substitute(substitute(substitute(
            e, "x", Expr::Num(7)), "y", Expr::Num(3)), "z", Expr::Num(2)));
        ASSERT_NUM(val, 6, "x*y/x*z at x=7,y=3,z=2 → 6");
    }

    // Cube surface from derive: 2*s^2 + 2*s^2 + 2*s^2 → 6*s^2
    ASSERT_EQ(expr_to_string(simplify(parse("2 * s^2 + 2 * s^2 + 2 * s^2"))),
        "6 * s^2", "2s^2+2s^2+2s^2 → 6s^2");

    // Isosceles derive: s^2 + other^2 - s^2 → other^2
    ASSERT_EQ(expr_to_string(simplify(parse("s^2 + other^2 - s^2"))),
        "other^2", "s^2+other^2-s^2 → other^2");

    // other^2 / (2 * side * other) → other / (2 * side)
    // (from isosceles triangle derive — needs cross-term cancellation)
    {
        auto e = simplify(parse("other^2 / (2 * side * other)"));
        double val = evaluate(substitute(substitute(
            e, "other", Expr::Num(6)), "side", Expr::Num(4)));
        ASSERT_NUM(val, 0.75, "other^2/(2*side*other) at other=6,side=4 → 0.75");
    }
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
    ASSERT_EQ(expr_to_string(simplify(parse("x * 6 / 2"))), "x * 3", "(x*6)/2 → x*3");
    ASSERT_EQ(expr_to_string(simplify(parse("x * 0.866 / 2"))), "x * 0.433", "(x*0.866)/2 → x*0.433");

    // (a / K1) * K2 → a / (K1/K2)
    ASSERT_EQ(expr_to_string(simplify(parse("x / 4 * 2"))), "x / 2", "(x/4)*2 → x/2");
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

    // x / (2*x) → 1/2 = 0.5
    ASSERT_EQ(expr_to_string(simplify(parse("x / (2 * x)"))), "0.5", "x/(2x) → 0.5");

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
    double val = evaluate(substitute(e, "x", Expr::Num(0)));
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
        ASSERT_EQ(result, "m * 9.81", "derive: default substituted");
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
        ASSERT(msg.find("Invalid number") != std::string::npos, "no symbolic: throws");
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

// ---- Main ----

int main() {
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

    // Simplifier improvements
    test_simplify_rule_interactions();
    test_simplify_flatten_targets();
    test_simplify_like_terms();
    test_simplify_mul_to_pow();
    test_simplify_self_division();
    test_simplify_constant_collection();
    test_simplify_constant_reassociation();

    // Derive mode
    test_derive_basic();
    test_derive_same_name();
    test_derive_formula_call();
    test_derive_errors();
    test_derive_cli_parsing();
    test_derive_binary_integration();

    std::cout << "\n===============\n";
    std::cout << "Total: " << tests_run
              << "  Passed: " << tests_passed
              << "  Failed: " << tests_failed << "\n";

    return tests_failed > 0 ? 1 : 0;
}
