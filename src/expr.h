#pragma once
#include <string>
#include <cstdint>
#include <memory>
#include <cassert>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <set>
#include <map>
#include <vector>
#include <deque>
#include <optional>
#include <stdexcept>
#include <functional>
#include <random>
#include <algorithm>

// Thresholds used throughout the solver
constexpr double EPSILON_ZERO = 1e-12;   // treat |x| < this as zero (coefficient guard, like-term combining)
constexpr double EPSILON_REL  = 1e-9;    // relative tolerance for verify mode (approx_equal)
constexpr int    SIMPLIFY_MAX_ITER = 20; // fixpoint loop limit for simplify()

static_assert(EPSILON_ZERO > 0 && EPSILON_ZERO < 1e-6, "EPSILON_ZERO must be a small positive value");
static_assert(EPSILON_REL > 0 && EPSILON_REL < 1e-3, "EPSILON_REL must be a small positive value");
static_assert(SIMPLIFY_MAX_ITER > 0 && SIMPLIFY_MAX_ITER < 1000, "SIMPLIFY_MAX_ITER must be reasonable");

// ============================================================================
//  Formatting (needed by ValueSet::to_string)
// ============================================================================

inline std::string fmt_num(double v) {
    if (std::abs(v) < 1e12 && v == static_cast<double>(static_cast<long long>(v)))
        return std::to_string(static_cast<long long>(v));
    std::ostringstream os;
    os << std::setprecision(10) << v;
    return os.str();
}

// ============================================================================
//  ValueSet — unified representation for conditions, ranges, and solutions
// ============================================================================

enum class NumberDomain : uint8_t { REAL, INTEGER, RATIONAL, COMPLEX, COUNT_ };

struct Interval {
    double low = 0, high = 0;
    bool low_inclusive = false, high_inclusive = false;

    bool contains(double v) const {
        bool above = low_inclusive ? (v >= low) : (v > low);
        bool below = high_inclusive ? (v <= high) : (v < high);
        return above && below;
    }

    bool empty() const {
        return (low > high) || (low == high && !(low_inclusive && high_inclusive));
    }
};

class ValueSet {
    std::vector<Interval> intervals_;
    std::vector<double> discrete_;
    NumberDomain domain_ = NumberDomain::REAL;

public:
    // Constructors
    ValueSet() = default;

    static ValueSet all() {
        ValueSet s;
        s.intervals_.push_back({-std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::infinity(), false, false});
        return s;
    }

    static ValueSet gt(double v) {
        ValueSet s;
        s.intervals_.push_back({v, std::numeric_limits<double>::infinity(), false, false});
        return s;
    }

    static ValueSet ge(double v) {
        ValueSet s;
        s.intervals_.push_back({v, std::numeric_limits<double>::infinity(), true, false});
        return s;
    }

    static ValueSet lt(double v) {
        ValueSet s;
        s.intervals_.push_back({-std::numeric_limits<double>::infinity(), v, false, false});
        return s;
    }

    static ValueSet le(double v) {
        ValueSet s;
        s.intervals_.push_back({-std::numeric_limits<double>::infinity(), v, false, true});
        return s;
    }

    static ValueSet eq(double v) {
        ValueSet s;
        s.discrete_.push_back(v);
        return s;
    }

    static ValueSet ne(double v) {
        ValueSet s;
        s.intervals_.push_back({-std::numeric_limits<double>::infinity(), v, false, false});
        s.intervals_.push_back({v, std::numeric_limits<double>::infinity(), false, false});
        return s;
    }

    static ValueSet discrete(std::initializer_list<double> values) {
        ValueSet s;
        s.discrete_.assign(values);
        return s;
    }

    static ValueSet discrete(const std::vector<double>& values) {
        ValueSet s;
        s.discrete_ = values;
        return s;
    }

    static ValueSet between(double lo, double hi, bool lo_inc, bool hi_inc) {
        ValueSet s;
        s.intervals_.push_back({lo, hi, lo_inc, hi_inc});
        return s;
    }

    // Queries
    bool empty() const { return intervals_.empty() && discrete_.empty(); }

    bool contains(double v) const {
        for (auto& iv : intervals_)
            if (iv.contains(v)) return true;
        for (auto& d : discrete_)
            if (std::abs(d - v) < EPSILON_ZERO) return true;
        return false;
    }

    const std::vector<Interval>& intervals() const { return intervals_; }
    const std::vector<double>& discrete() const { return discrete_; }
    NumberDomain domain() const { return domain_; }

    // Set operations
    ValueSet intersect(const ValueSet& other) const {
        ValueSet result;

        // Interval ∩ Interval
        for (auto& a : intervals_)
            for (auto& b : other.intervals_) {
                double lo = std::max(a.low, b.low);
                double hi = std::min(a.high, b.high);
                bool lo_inc = (a.low == b.low) ? (a.low_inclusive && b.low_inclusive)
                            : (lo == a.low) ? a.low_inclusive : b.low_inclusive;
                bool hi_inc = (a.high == b.high) ? (a.high_inclusive && b.high_inclusive)
                            : (hi == a.high) ? a.high_inclusive : b.high_inclusive;
                Interval iv{lo, hi, lo_inc, hi_inc};
                if (!iv.empty()) result.intervals_.push_back(iv);
            }

        // Discrete points: keep only those in both sets
        for (auto& d : discrete_)
            if (other.contains(d)) result.discrete_.push_back(d);
        for (auto& d : other.discrete_)
            if (this->contains(d)) {
                // Avoid duplicates
                bool dup = false;
                for (auto& rd : result.discrete_)
                    if (std::abs(rd - d) < EPSILON_ZERO) { dup = true; break; }
                if (!dup) result.discrete_.push_back(d);
            }

        return result;
    }

    ValueSet unite(const ValueSet& other) const {
        ValueSet result;
        result.intervals_ = intervals_;
        result.intervals_.insert(result.intervals_.end(),
            other.intervals_.begin(), other.intervals_.end());
        result.discrete_ = discrete_;
        for (auto& d : other.discrete_) {
            bool dup = false;
            for (auto& rd : result.discrete_)
                if (std::abs(rd - d) < EPSILON_ZERO) { dup = true; break; }
            if (!dup) result.discrete_.push_back(d);
        }
        return result;
    }

    // Filter a list of values through this set
    std::vector<double> filter(const std::vector<double>& values) const {
        std::vector<double> result;
        for (auto v : values)
            if (contains(v)) result.push_back(v);
        return result;
    }

    // Is this a purely discrete set (no intervals)?
    bool is_discrete() const { return intervals_.empty(); }

    // String representation
    std::string to_string() const {
        if (empty()) return "{}";

        std::vector<std::string> parts;

        for (auto& iv : intervals_) {
            std::string s;
            s += iv.low_inclusive ? "[" : "(";
            s += (iv.low == -std::numeric_limits<double>::infinity()) ? "-inf" : fmt_num(iv.low);
            s += ", ";
            s += (iv.high == std::numeric_limits<double>::infinity()) ? "+inf" : fmt_num(iv.high);
            s += iv.high_inclusive ? "]" : ")";
            parts.push_back(s);
        }

        if (!discrete_.empty()) {
            std::string s = "{";
            for (size_t i = 0; i < discrete_.size(); i++)
                s += (i ? ", " : "") + fmt_num(discrete_[i]);
            s += "}";
            parts.push_back(s);
        }

        if (parts.size() == 1) return parts[0];
        std::string result;
        for (size_t i = 0; i < parts.size(); i++)
            result += (i ? " | " : "") + parts[i];
        return result;
    }
};

// ============================================================================
//  Expression arena (contiguous allocation for cache locality)
// ============================================================================

enum class ExprType : uint8_t { NUM, VAR, BINOP, UNARY_NEG, FUNC_CALL, COUNT_ };
enum class BinOp   : uint8_t { ADD, SUB, MUL, DIV, POW, COUNT_ };

static_assert(static_cast<int>(ExprType::COUNT_) == 5, "ExprType has 5 real values");
static_assert(static_cast<int>(BinOp::COUNT_) == 5, "BinOp has 5 real values");
static_assert(static_cast<int>(BinOp::ADD) == 0, "BinOp values start at 0 (used as array index)");

struct Expr;
using ExprPtr = Expr*;

class ExprArena {
    static constexpr size_t CHUNK_SIZE = 1024;  // nodes per chunk
    std::vector<std::unique_ptr<Expr[]>> chunks;
    size_t next_in_chunk = CHUNK_SIZE;  // force first alloc to create chunk
    static inline thread_local ExprArena* current_ = nullptr;
public:
    Expr* alloc();  // defined after Expr

    static ExprArena* current() { return current_; }
    struct Scope {
        ExprArena* prev;
        Scope(ExprArena& a) : prev(current_) { current_ = &a; }
        ~Scope() { current_ = prev; }
    };
    size_t size() const { return chunks.empty() ? 0 : (chunks.size()-1) * CHUNK_SIZE + next_in_chunk; }
};

// ============================================================================
//  Expression tree
// ============================================================================

struct Expr {
    ExprType type;
    BinOp op{};                 // packed with type (2 bytes, then 6 padding before num)
    double num = 0;
    ExprPtr left = nullptr, right = nullptr, child = nullptr;
    std::string name;           // 32 bytes (SSO)
    std::vector<ExprPtr> args;  // 24 bytes

    static ExprPtr Num(double v);
    static ExprPtr Var(const std::string& n);
    static ExprPtr BinOpExpr(BinOp o, ExprPtr l, ExprPtr r);
    static ExprPtr Neg(ExprPtr c);
    static ExprPtr Call(const std::string& n, std::vector<ExprPtr> a);
};

// Arena allocation — contiguous chunks for cache locality
inline Expr* ExprArena::alloc() {
    if (next_in_chunk >= CHUNK_SIZE) {
        chunks.push_back(std::make_unique<Expr[]>(CHUNK_SIZE));
        next_in_chunk = 0;
    }
    return &chunks.back()[next_in_chunk++];
}

inline ExprPtr Expr::Num(double v) {
    assert(ExprArena::current() && "ExprArena::Scope must be active");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::NUM; e->num = v; return e;
}
inline ExprPtr Expr::Var(const std::string& n) {
    assert(ExprArena::current() && "ExprArena::Scope must be active");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::VAR; e->name = n; return e;
}
inline ExprPtr Expr::BinOpExpr(BinOp o, ExprPtr l, ExprPtr r) {
    assert(l && r && "BinOp operands must not be null");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::BINOP; e->op = o; e->left = l; e->right = r; return e;
}
inline ExprPtr Expr::Neg(ExprPtr c) {
    assert(c && "Neg operand must not be null");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::UNARY_NEG; e->child = c; return e;
}
inline ExprPtr Expr::Call(const std::string& n, std::vector<ExprPtr> a) {
    auto e = ExprArena::current()->alloc(); e->type = ExprType::FUNC_CALL; e->name = n; e->args = std::move(a); return e;
}

// ============================================================================
//  Type predicates
// ============================================================================

// Reference versions (no null check needed)
constexpr bool is_num(const Expr& e)     { return e.type == ExprType::NUM; }
constexpr bool is_var(const Expr& e)     { return e.type == ExprType::VAR; }
constexpr bool is_atomic(const Expr& e)  { return is_num(e) || is_var(e); }
constexpr bool is_zero(const Expr& e)    { return is_num(e) && e.num == 0; }
constexpr bool is_one(const Expr& e)     { return is_num(e) && e.num == 1; }
constexpr bool is_neg_one(const Expr& e) { return is_num(e) && e.num == -1; }
constexpr bool is_neg(const Expr& e)     { return e.type == ExprType::UNARY_NEG; }
constexpr bool is_neg_num(const Expr& e) { return is_num(e) && e.num < 0; }
// Pointer versions (null-safe, for struct fields)
inline bool is_num(const ExprPtr e)     { return e && is_num(*e); }
inline bool is_var(const ExprPtr e)     { return e && is_var(*e); }
inline bool is_atomic(const ExprPtr e)  { return e && is_atomic(*e); }
inline bool is_zero(const ExprPtr e)    { return e && is_zero(*e); }
inline bool is_one(const ExprPtr e)     { return e && is_one(*e); }
inline bool is_neg_one(const ExprPtr e) { return e && is_neg_one(*e); }
inline bool is_neg(const ExprPtr e)     { return e && is_neg(*e); }
inline bool is_neg_num(const ExprPtr e) { return e && is_neg_num(*e); }

constexpr bool is_additive(BinOp op)       { return op == BinOp::ADD || op == BinOp::SUB; }
constexpr bool is_multiplicative(BinOp op) { return op == BinOp::MUL || op == BinOp::DIV; }

// ============================================================================
//  BinOp metadata
// ============================================================================

struct BinOpInfo {
    const char* symbol;
    int precedence;
    double (*eval)(double, double);
};

inline double eval_div(double l, double r) {
    if (r == 0) throw std::runtime_error("Division by zero");
    return l / r;
}

inline const BinOpInfo& binop_info(BinOp op) {
    static const BinOpInfo table[] = {
        {" + ", 1, [](double l, double r) { return l + r; }},   // ADD
        {" - ", 1, [](double l, double r) { return l - r; }},   // SUB
        {" * ", 2, [](double l, double r) { return l * r; }},   // MUL
        {" / ", 2, eval_div},                                     // DIV
        {"^",   4, [](double l, double r) { return std::pow(l, r); }}, // POW
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<size_t>(BinOp::COUNT_),
        "BinOp table must have one entry per enum value");
    return table[static_cast<int>(op)];
}

// ============================================================================
//  Builtin function registry
// ============================================================================

inline const std::map<std::string, double(*)(double)>& builtin_functions() {
    static const std::map<std::string, double(*)(double)> registry = {
        {"sqrt", std::sqrt}, {"abs", std::fabs}, {"sin",  std::sin},
        {"cos",  std::cos},  {"tan", std::tan},  {"log",  std::log},
        {"asin", std::asin}, {"acos",std::acos}, {"atan", std::atan}
    };
    return registry;
}

// ============================================================================
//  Builtin constants
// ============================================================================

inline const std::map<std::string, double>& builtin_constants() {
    static const std::map<std::string, double> registry = {
        {"pi",  M_PI},
        {"e",   M_E},
        {"phi", (1.0 + std::sqrt(5.0)) / 2.0},  // golden ratio 1.618...
    };
    return registry;
}

// ============================================================================
//  Tree queries
// ============================================================================

inline void collect_vars(const Expr& e, std::set<std::string>& out) {
    switch (e.type) {
        case ExprType::NUM:       break;
        case ExprType::VAR:       out.insert(e.name); break;
        case ExprType::BINOP:     collect_vars(*e.left, out); collect_vars(*e.right, out); break;
        case ExprType::UNARY_NEG: collect_vars(*e.child, out); break;
        case ExprType::FUNC_CALL: for (auto& a : e.args) collect_vars(*a, out); break;
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
}

// Direct search — no allocation, returns at first hit
inline bool contains_var(const Expr& e, const std::string& v) {
    switch (e.type) {
        case ExprType::NUM:       return false;
        case ExprType::VAR:       return e.name == v;
        case ExprType::BINOP:     return contains_var(*e.left, v) || contains_var(*e.right, v);
        case ExprType::UNARY_NEG: return contains_var(*e.child, v);
        case ExprType::FUNC_CALL: for (auto& a : e.args) if (contains_var(*a, v)) return true;
                                  return false;
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return false;
    }
    return false;
}

// Structural equality — no allocation, used for simplifier fixpoint
inline bool expr_equal(const Expr& a, const Expr& b) {
    if (&a == &b) return true;    // pointer shortcut
    if (a.type != b.type) return false;
    switch (a.type) {
        case ExprType::NUM:       return a.num == b.num;
        case ExprType::VAR:       return a.name == b.name;
        case ExprType::UNARY_NEG: return expr_equal(*a.child, *b.child);
        case ExprType::BINOP:     return a.op == b.op
                                      && expr_equal(*a.left, *b.left)
                                      && expr_equal(*a.right, *b.right);
        case ExprType::FUNC_CALL:
            if (a.name != b.name || a.args.size() != b.args.size()) return false;
            for (size_t i = 0; i < a.args.size(); i++)
                if (!expr_equal(*a.args[i], *b.args[i])) return false;
            return true;
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return false;
    }
    return false;
}
// Pointer overloads for convenience
inline void collect_vars(ExprPtr e, std::set<std::string>& out) { if (e) collect_vars(*e, out); }
inline bool contains_var(ExprPtr e, const std::string& v) { return e && contains_var(*e, v); }
inline bool expr_equal(ExprPtr a, ExprPtr b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return expr_equal(*a, *b);
}

inline int precedence(const Expr& e) {
    if (e.type == ExprType::BINOP) return binop_info(e.op).precedence;
    if (e.type == ExprType::UNARY_NEG) return 3;
    return 5; // atom
}

inline std::string expr_to_string(const Expr& e) {
    switch (e.type) {
        case ExprType::NUM:
            return (e.num < 0) ? "(" + fmt_num(e.num) + ")" : fmt_num(e.num);

        case ExprType::VAR:
            return e.name;

        case ExprType::UNARY_NEG:
            return is_atomic(*e.child)
                ? "-" + expr_to_string(*e.child)
                : "-(" + expr_to_string(*e.child) + ")";

        case ExprType::BINOP: {
            auto& info = binop_info(e.op);
            int prec = info.precedence;

            auto wrap = [&](const Expr& child, bool rhs) {
                int cp = precedence(child);
                bool need = (cp < prec) ||
                    (cp == prec && rhs && (e.op == BinOp::SUB || e.op == BinOp::DIV));
                auto s = expr_to_string(child);
                return need ? "(" + s + ")" : s;
            };
            return wrap(*e.left, false) + info.symbol + wrap(*e.right, true);
        }

        case ExprType::FUNC_CALL: {
            std::string s = e.name + "(";
            for (size_t i = 0; i < e.args.size(); i++)
                s += (i ? ", " : "") + expr_to_string(*e.args[i]);
            return s + ")";
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return "?";
}
// Pointer overloads
inline int precedence(ExprPtr e) { return e ? precedence(*e) : 5; }
inline std::string expr_to_string(ExprPtr e) { return e ? expr_to_string(*e) : "?"; }

// ============================================================================
//  Substitute
// ============================================================================

inline ExprPtr substitute(ExprPtr e, const std::string& var, ExprPtr val) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:       return e;
        case ExprType::VAR:       return (e->name == var) ? val : e;
        case ExprType::UNARY_NEG: return Expr::Neg(substitute(e->child, var, val));
        case ExprType::BINOP:     return Expr::BinOpExpr(e->op,
                                      substitute(e->left, var, val),
                                      substitute(e->right, var, val));
        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> a;
            a.reserve(e->args.size());
            for (auto& arg : e->args) a.push_back(substitute(arg, var, val));
            return Expr::Call(e->name, a);
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

// ============================================================================
//  Evaluate
// ============================================================================

inline double evaluate(const Expr& e) {
    switch (e.type) {
        case ExprType::NUM: return e.num;
        case ExprType::VAR: {
            auto& consts = builtin_constants();
            auto it = consts.find(e.name);
            if (it != consts.end()) return it->second;
            throw std::runtime_error("Cannot evaluate: unresolved variable '" + e.name + "'");
        }
        case ExprType::UNARY_NEG:
            return -evaluate(*e.child);
        case ExprType::BINOP:
            return binop_info(e.op).eval(evaluate(*e.left), evaluate(*e.right));
        case ExprType::FUNC_CALL: {
            if (e.args.size() != 1)
                throw std::runtime_error("Unknown function: " + e.name);
            auto& registry = builtin_functions();
            auto it = registry.find(e.name);
            if (it == registry.end())
                throw std::runtime_error("Unknown function: " + e.name);
            return it->second(evaluate(*e.args[0]));
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    throw std::runtime_error("Cannot evaluate: unknown expression type");
}
inline double evaluate(ExprPtr e) {
    if (!e) throw std::runtime_error("Cannot evaluate null expression");
    return evaluate(*e);
}

// ============================================================================
//  Simplify
// ============================================================================

// ---- Flattening helpers ----

// Decompose expr into (base, exponent) — e.g. x^3 → (x, 3), x → (x, 1)
inline std::pair<ExprPtr, double> split_pow(ExprPtr e) {
    assert(e && "split_pow: null expression");
    if (e->type == ExprType::BINOP && e->op == BinOp::POW && is_num(e->right))
        return {e->left, e->right->num};
    return {e, 1.0};
}

// Flatten an additive chain (ADD/SUB) into (coefficient, base) terms.
// Each term represents coeff * base. Bare constants have base=nullptr.
inline void flatten_additive(ExprPtr e, double sign,
                             std::vector<std::pair<double, ExprPtr>>& terms) {
    assert(e && "flatten_additive: null expression");
    if (e->type == ExprType::NUM) {
        terms.push_back({sign * e->num, nullptr});
    } else if (e->type == ExprType::UNARY_NEG) {
        flatten_additive(e->child, -sign, terms);
    } else if (e->type == ExprType::BINOP && e->op == BinOp::ADD) {
        flatten_additive(e->left, sign, terms);
        flatten_additive(e->right, sign, terms);
    } else if (e->type == ExprType::BINOP && e->op == BinOp::SUB) {
        flatten_additive(e->left, sign, terms);
        flatten_additive(e->right, -sign, terms);
    } else if (e->type == ExprType::BINOP && e->op == BinOp::MUL
               && e->left->type == ExprType::NUM) {
        // k * expr → coefficient is k, base is expr
        terms.push_back({sign * e->left->num, e->right});
    } else if (e->type == ExprType::BINOP && e->op == BinOp::MUL
               && e->right->type == ExprType::NUM) {
        terms.push_back({sign * e->right->num, e->left});
    } else {
        terms.push_back({sign, e});
    }
}

// Reconstruct an expression from additive terms
inline ExprPtr rebuild_additive(const std::vector<std::pair<double, ExprPtr>>& terms) {
    if (terms.empty()) return Expr::Num(0);

    auto make_term = [](double coeff, const ExprPtr& base) -> ExprPtr {
        if (!base) return Expr::Num(coeff);
        if (coeff == 1.0) return base;
        if (coeff == -1.0) return Expr::Neg(base);
        return Expr::BinOpExpr(BinOp::MUL, Expr::Num(coeff), base);
    };

    ExprPtr result = nullptr;
    for (auto& [coeff, base] : terms) {
        if (std::abs(coeff) < EPSILON_ZERO) continue; // skip zero terms
        auto term = make_term(std::abs(coeff), base);
        if (!result) {
            result = (coeff < 0) ? Expr::Neg(term) : term;
        } else if (coeff > 0) {
            result = Expr::BinOpExpr(BinOp::ADD, result, term);
        } else {
            result = Expr::BinOpExpr(BinOp::SUB, result, term);
        }
    }
    return result ? result : Expr::Num(0);
}

// Flatten a MUL chain into (base, exponent) factors.
// Only flattens through MUL, not DIV (to preserve division structure).
// Numeric constants are collected into a single coefficient.
inline void flatten_multiplicative(ExprPtr e,
                                   double& coeff,
                                   std::vector<std::pair<ExprPtr, double>>& factors) {
    assert(e && "flatten_multiplicative: null expression");
    if (e->type == ExprType::NUM) {
        coeff *= e->num;
    } else if (e->type == ExprType::UNARY_NEG) {
        coeff = -coeff;
        flatten_multiplicative(e->child, coeff, factors);
    } else if (e->type == ExprType::BINOP && e->op == BinOp::MUL) {
        flatten_multiplicative(e->left, coeff, factors);
        flatten_multiplicative(e->right, coeff, factors);
    } else if (e->type == ExprType::BINOP && e->op == BinOp::DIV
               && e->right->type == ExprType::NUM && e->right->num != 0) {
        coeff /= e->right->num;
        flatten_multiplicative(e->left, coeff, factors);
    } else if (e->type == ExprType::BINOP && e->op == BinOp::POW
               && e->right->type == ExprType::NUM) {
        factors.push_back({e->left, e->right->num});
    } else {
        factors.push_back({e, 1.0});
    }
}

// Reconstruct an expression from multiplicative factors (MUL only, no DIV)
inline ExprPtr rebuild_multiplicative(double coeff,
                                      const std::vector<std::pair<ExprPtr, double>>& factors) {
    auto make_factor = [](const ExprPtr& base, double exp) -> ExprPtr {
        if (exp == 1.0) return base;
        return Expr::BinOpExpr(BinOp::POW, base, Expr::Num(exp));
    };

    std::vector<ExprPtr> parts;
    for (auto& [base, exp] : factors) {
        if (std::abs(exp) < EPSILON_ZERO) continue; // base^0 = 1, skip
        parts.push_back(make_factor(base, exp));
    }

    bool neg = coeff < 0;
    double abs_coeff = std::abs(coeff);
    ExprPtr result = nullptr;
    if (abs_coeff != 1.0 || parts.empty()) result = Expr::Num(abs_coeff);
    for (auto& f : parts)
        result = result ? Expr::BinOpExpr(BinOp::MUL, result, f) : f;
    if (!result) result = Expr::Num(1);

    return neg ? Expr::Neg(result) : result;
}

// Group additive terms by base, combining coefficients
// Group like terms: merge entries with equal keys by summing their values.
// GetKey returns the ExprPtr key, GetVal/SetVal access the numeric value,
// Nullify marks an entry as consumed.
template<typename Vec, typename GetKey, typename GetVal, typename Nullify>
inline void group_like(Vec& items, GetKey key, GetVal val, Nullify nullify) {
    for (size_t i = 0; i < items.size(); i++) {
        if (!key(items[i])) continue;
        for (size_t j = i + 1; j < items.size(); j++) {
            if (!key(items[j])) continue;
            if (expr_equal(key(items[i]), key(items[j]))) {
                val(items[i]) += val(items[j]);
                nullify(items[j]);
            }
        }
    }
}

inline void group_additive(std::vector<std::pair<double, ExprPtr>>& terms) {
    group_like(terms,
        [](auto& t) { return t.second; },
        [](auto& t) -> double& { return t.first; },
        [](auto& t) { t.first = 0; t.second = nullptr; });
}

inline void group_multiplicative(std::vector<std::pair<ExprPtr, double>>& factors) {
    group_like(factors,
        [](auto& f) { return f.first; },
        [](auto& f) -> double& { return f.second; },
        [](auto& f) { f.second = 0; f.first = nullptr; });
}

// ============================================================================
//  Simplification assumptions (conditions generated by simplification)
// ============================================================================

// When simplify cancels S/S → 1, it records the assumption S ≠ 0.
// Call simplify_get_assumptions() after simplify() to retrieve them.
struct SimplifyAssumption {
    ExprPtr expr;       // the expression assumed non-zero
    std::string desc;   // human-readable: "x - 3 != 0"
};

inline std::vector<SimplifyAssumption>& simplify_assumptions_() {
    static thread_local std::vector<SimplifyAssumption> assumptions;
    return assumptions;
}

inline void simplify_assume_nonzero(const ExprPtr& expr) {
    if (is_num(expr)) return;
    auto& a = simplify_assumptions_();
    std::string desc = expr_to_string(expr) + " != 0";
    // Dedup by string (safe across arena scopes — pointer comparison isn't)
    for (auto& existing : a)
        if (existing.desc == desc) return;
    a.push_back({expr, desc});
}

inline std::vector<SimplifyAssumption> simplify_get_assumptions() {
    auto result = std::move(simplify_assumptions_());
    simplify_assumptions_().clear();
    return result;
}

inline void simplify_clear_assumptions() {
    simplify_assumptions_().clear();
}

// ---- Simplify: per-operator helpers ----

inline ExprPtr simplify_additive(const ExprPtr& combined) {
    std::vector<std::pair<double, ExprPtr>> terms;
    flatten_additive(combined, 1.0, terms);
    double constant = 0;
    std::vector<std::pair<double, ExprPtr>> symbolic;
    for (auto& [c, b] : terms) {
        if (!b) constant += c;
        else    symbolic.push_back({c, b});
    }
    group_additive(symbolic);
    if (std::abs(constant) >= EPSILON_ZERO)
        symbolic.push_back({constant, nullptr});
    return rebuild_additive(symbolic);
}

inline ExprPtr simplify_mul(const ExprPtr& l, const ExprPtr& r) {
    if (is_zero(l) || is_zero(r)) return Expr::Num(0);
    auto combined = Expr::BinOpExpr(BinOp::MUL, l, r);
    double coeff = 1.0;
    std::vector<std::pair<ExprPtr, double>> factors;
    flatten_multiplicative(combined, coeff, factors);
    group_multiplicative(factors);
    return rebuild_multiplicative(coeff, factors);
}

inline ExprPtr simplify_div(const ExprPtr& l, const ExprPtr& r) {
    if (is_zero(l)) return Expr::Num(0);
    if (is_one(r)) return l;
    if (is_neg_one(r)) return Expr::Neg(l);
    if (is_neg_num(r))
        return Expr::BinOpExpr(BinOp::DIV, Expr::Neg(l), Expr::Num(-r->num));
    if (is_neg(l) && is_neg(r))
        return Expr::BinOpExpr(BinOp::DIV, l->child, r->child);
    if (is_neg(r))
        return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, l, r->child));
    if (is_neg(l))
        return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, l->child, r));
    // Constant reassociation: (K * a) / K2 or (a * K) / K2
    if (is_num(r) && l->type == ExprType::BINOP && l->op == BinOp::MUL) {
        if (is_num(l->right))
            return Expr::BinOpExpr(BinOp::MUL, l->left, Expr::Num(l->right->num / r->num));
        if (is_num(l->left))
            return Expr::BinOpExpr(BinOp::MUL, Expr::Num(l->left->num / r->num), l->right);
    }
    if (is_num(r) && l->type == ExprType::BINOP && l->op == BinOp::DIV && is_num(l->right))
        return Expr::BinOpExpr(BinOp::DIV, l->left, Expr::Num(l->right->num * r->num));
    // Distribute division over sum: (a*x + b*x) / x → a + b
    // Flatten numerator additively, try dividing each term by denominator
    if (l->type == ExprType::BINOP && is_additive(l->op)) {
        std::vector<std::pair<double, ExprPtr>> terms;
        flatten_additive(l, 1.0, terms);
        bool all_cancel = true;
        std::vector<ExprPtr> divided;
        for (auto& [coeff, base] : terms) {
            if (!base) { // constant term
                divided.push_back(Expr::BinOpExpr(BinOp::DIV, Expr::Num(coeff), r));
                continue;
            }
            ExprPtr term = (coeff == 1.0) ? base
                : Expr::BinOpExpr(BinOp::MUL, Expr::Num(coeff), base);
            auto d = simplify_div(term, r);
            // Check if the division actually cancelled (no DIV remaining at top)
            if (d->type == ExprType::BINOP && d->op == BinOp::DIV) {
                all_cancel = false; break;
            }
            divided.push_back(d);
        }
        if (all_cancel && !divided.empty()) {
            ExprPtr result = divided[0];
            for (size_t i = 1; i < divided.size(); i++)
                result = Expr::BinOpExpr(BinOp::ADD, result, divided[i]);
            return result;
        }
    }

    // Cross-term cancellation: flatten both sides, cancel matching bases
    double lc = 1.0, rc = 1.0;
    std::vector<std::pair<ExprPtr, double>> lf, rf;
    flatten_multiplicative(l, lc, lf);
    flatten_multiplicative(r, rc, rf);
    bool changed = false;
    for (auto& [lb, le] : lf) {
        if (!lb) continue;
        for (auto& [rb, re] : rf) {
            if (!rb) continue;
            if (expr_equal(lb, rb)) {
                simplify_assume_nonzero(lb);
                le -= re; re = 0; rb = nullptr; changed = true;
            }
        }
    }
    if (changed) {
        auto top = rebuild_multiplicative(lc, lf);
        auto bot = rebuild_multiplicative(rc, rf);
        if (is_one(bot)) return top;
        if (is_neg_one(bot)) return Expr::Neg(top);
        return Expr::BinOpExpr(BinOp::DIV, top, bot);
    }
    return Expr::BinOpExpr(BinOp::DIV, l, r);
}

// ---- Simplify: main entry ----

inline ExprPtr simplify_once(const ExprPtr& e) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return e;

        case ExprType::UNARY_NEG: {
            auto c = simplify_once(e->child);
            if (is_num(c)) return Expr::Num(-c->num);
            if (is_neg(c)) return c->child;
            if (c->type == ExprType::BINOP && is_additive(c->op))
                return simplify_additive(Expr::Neg(c));
            if (c->type == ExprType::BINOP && c->op == BinOp::DIV)
                return Expr::BinOpExpr(BinOp::DIV, Expr::Neg(c->left), c->right);
            return Expr::Neg(c);
        }

        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> sa;
            bool all_num = true;
            for (auto& a : e->args) {
                sa.push_back(simplify_once(a));
                if (!is_num(sa.back())) all_num = false;
            }
            auto s = Expr::Call(e->name, sa);
            if (all_num && builtin_functions().count(e->name)) return Expr::Num(evaluate(s));

            // log/exp simplifications
            if (e->name == "log" && sa.size() == 1) {
                auto& arg = sa[0];
                // log(e^x) → x
                if (arg->type == ExprType::BINOP && arg->op == BinOp::POW
                    && is_var(arg->left) && arg->left->name == "e")
                    return arg->right;
                // log(x^n) → n * log(x)  (assuming x > 0)
                if (arg->type == ExprType::BINOP && arg->op == BinOp::POW) {
                    simplify_assume_nonzero(arg->left); // actually x > 0, but ≠0 is the key part
                    return Expr::BinOpExpr(BinOp::MUL, arg->right,
                        Expr::Call("log", {arg->left}));
                }
            }
            if (e->name == "sqrt" && sa.size() == 1) {
                auto& arg = sa[0];
                // sqrt(x^2) → abs(x)
                if (arg->type == ExprType::BINOP && arg->op == BinOp::POW
                    && is_num(arg->right) && arg->right->num == 2.0)
                    return Expr::Call("abs", {arg->left});
            }
            // abs rules
            if (e->name == "abs" && sa.size() == 1) {
                auto& arg = sa[0];
                // abs(abs(x)) → abs(x)
                if (arg->type == ExprType::FUNC_CALL && arg->name == "abs")
                    return arg;
                // abs(-x) → abs(x)
                if (is_neg(arg))
                    return Expr::Call("abs", {arg->child});
            }
            // sin(-x) → -sin(x), cos(-x) → cos(x)
            if (e->name == "sin" && sa.size() == 1 && is_neg(sa[0]))
                return Expr::Neg(Expr::Call("sin", {sa[0]->child}));
            if (e->name == "cos" && sa.size() == 1 && is_neg(sa[0]))
                return Expr::Call("cos", {sa[0]->child});
            // Inverse trig pairs: asin(sin(x)) → x, acos(cos(x)) → x, atan(tan(x)) → x
            if (e->name == "asin" && sa.size() == 1
                && sa[0]->type == ExprType::FUNC_CALL && sa[0]->name == "sin")
                return sa[0]->args[0];
            if (e->name == "acos" && sa.size() == 1
                && sa[0]->type == ExprType::FUNC_CALL && sa[0]->name == "cos")
                return sa[0]->args[0];
            if (e->name == "atan" && sa.size() == 1
                && sa[0]->type == ExprType::FUNC_CALL && sa[0]->name == "tan")
                return sa[0]->args[0];
            // Forward trig of inverse: sin(asin(x)) → x, cos(acos(x)) → x
            if (e->name == "sin" && sa.size() == 1
                && sa[0]->type == ExprType::FUNC_CALL && sa[0]->name == "asin")
                return sa[0]->args[0];
            if (e->name == "cos" && sa.size() == 1
                && sa[0]->type == ExprType::FUNC_CALL && sa[0]->name == "acos")
                return sa[0]->args[0];
            if (e->name == "tan" && sa.size() == 1
                && sa[0]->type == ExprType::FUNC_CALL && sa[0]->name == "atan")
                return sa[0]->args[0];
            return s;
        }

        case ExprType::BINOP: {
            auto l = simplify_once(e->left);
            auto r = simplify_once(e->right);
            if (is_num(l) && is_num(r))
                return Expr::Num(binop_info(e->op).eval(l->num, r->num));

            switch (e->op) {
                case BinOp::ADD: case BinOp::SUB:
                    return simplify_additive(Expr::BinOpExpr(e->op, l, r));
                case BinOp::MUL: return simplify_mul(l, r);
                case BinOp::DIV: return simplify_div(l, r);
                case BinOp::POW:
                    if (is_zero(r)) return Expr::Num(1);
                    if (is_one(r)) return l;
                    // x^0.5 → sqrt(x)
                    if (is_num(r) && r->num == 0.5)
                        return Expr::Call("sqrt", {l});
                    // e^(log(x)) → x  (assuming x > 0)
                    if (is_var(l) && l->name == "e"
                        && r->type == ExprType::FUNC_CALL && r->name == "log"
                        && !r->args.empty()) {
                        simplify_assume_nonzero(r->args[0]); // x > 0 implied
                        return r->args[0];
                    }
                    // x^(-1) → 1/x, x^(-n) → 1/x^n
                    if (is_num(r) && r->num < 0) {
                        if (r->num == -1.0)
                            return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), l);
                        return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1),
                            Expr::BinOpExpr(BinOp::POW, l, Expr::Num(-r->num)));
                    }
                    // (x^a)^b → x^(a*b)
                    if (l->type == ExprType::BINOP && l->op == BinOp::POW)
                        return Expr::BinOpExpr(BinOp::POW, l->left,
                            simplify_once(Expr::BinOpExpr(BinOp::MUL, l->right, r)));
                    return Expr::BinOpExpr(BinOp::POW, l, r);
        case BinOp::COUNT_: assert(false && "invalid BinOp"); break;
            }
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

inline ExprPtr simplify(const ExprPtr& e) {
    assert(e && "cannot simplify null expression");
    ExprPtr cur = e;
    for (int i = 0; i < SIMPLIFY_MAX_ITER; i++) {
        auto next = simplify_once(cur);
        assert(next && "simplify_once must not return null");
        if (expr_equal(next, cur)) break;
        cur = next;
    }
    return cur;
}

// ============================================================================
//  Linear solver: decompose expr into coeff * target + rest
// ============================================================================

struct LinearForm { ExprPtr coeff, rest; };

inline std::optional<LinearForm> decompose_linear(const ExprPtr& e, const std::string& t) {
    if (!e) return LinearForm{Expr::Num(0), Expr::Num(0)};

    auto ok = [](ExprPtr c, ExprPtr r) -> std::optional<LinearForm> { return LinearForm{c, r}; };
    auto fail = []() -> std::optional<LinearForm> { return std::nullopt; };
    auto S = simplify;

    switch (e->type) {
        case ExprType::NUM:
            return ok(Expr::Num(0), e);

        case ExprType::VAR:
            return (e->name == t) ? ok(Expr::Num(1), Expr::Num(0))
                                  : ok(Expr::Num(0), e);

        case ExprType::UNARY_NEG: {
            auto d = decompose_linear(e->child, t);
            return d ? ok(S(Expr::Neg(d->coeff)), S(Expr::Neg(d->rest))) : fail();
        }

        case ExprType::BINOP:
            switch (e->op) {
                case BinOp::ADD: case BinOp::SUB: {
                    auto ld = decompose_linear(e->left, t);
                    auto rd = decompose_linear(e->right, t);
                    if (!ld || !rd) return fail();
                    return ok(S(Expr::BinOpExpr(e->op, ld->coeff, rd->coeff)),
                              S(Expr::BinOpExpr(e->op, ld->rest, rd->rest)));
                }
                case BinOp::MUL: {
                    bool lh = contains_var(e->left, t), rh = contains_var(e->right, t);
                    if (lh && rh) return fail();
                    if (!lh && !rh) return ok(Expr::Num(0), e);
                    auto [side, factor] = lh ? std::pair{e->left, e->right}
                                             : std::pair{e->right, e->left};
                    auto d = decompose_linear(side, t);
                    return d ? ok(S(Expr::BinOpExpr(BinOp::MUL, factor, d->coeff)),
                                  S(Expr::BinOpExpr(BinOp::MUL, factor, d->rest))) : fail();
                }
                case BinOp::DIV: {
                    if (contains_var(e->right, t)) return fail();
                    auto d = decompose_linear(e->left, t);
                    return d ? ok(S(Expr::BinOpExpr(BinOp::DIV, d->coeff, e->right)),
                                  S(Expr::BinOpExpr(BinOp::DIV, d->rest, e->right))) : fail();
                }
                case BinOp::POW:
                    if (contains_var(e->left, t) || contains_var(e->right, t)) return fail();
                    return ok(Expr::Num(0), e);
        case BinOp::COUNT_: assert(false && "invalid BinOp"); break;
            }
            break;

        case ExprType::FUNC_CALL:
            return contains_var(e, t) ? fail() : ok(Expr::Num(0), e);
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return fail();
}

// Solution from solve_for: expression + optional domain constraint
struct Solution {
    ExprPtr expr;
    ExprPtr condition;  // if non-null: this expr >= 0 must hold (e.g., discriminant)
    std::string cond_desc;  // human-readable: "y >= 0"
};

// Inverse function table: f → f⁻¹
inline const std::map<std::string, std::string>& inverse_functions() {
    static const std::map<std::string, std::string> table = {
        {"sin",  "asin"}, {"asin", "sin"},
        {"cos",  "acos"}, {"acos", "cos"},
        {"tan",  "atan"}, {"atan", "tan"},
        {"log",  "exp"},  // log(x) → e^x (handled specially)
        {"sqrt", "sqr"},  // sqrt(x) → x² (handled specially)
    };
    return table;
}

// Try to isolate target by peeling off invertible functions and operations.
// Returns ALL solutions (e.g., abs gives two, sqrt gives one).
inline std::vector<Solution> solve_by_inversion(ExprPtr lhs, ExprPtr rhs,
        const std::string& target, int depth = 0) {
    if (depth > 20) return {};
    lhs = simplify(lhs);
    rhs = simplify(rhs);

    // Helper: recurse and propagate results
    auto recurse = [&](ExprPtr new_lhs, ExprPtr new_rhs) {
        return solve_by_inversion(new_lhs, simplify(new_rhs), target, depth + 1);
    };

    // Base case: lhs IS the target
    if (is_var(lhs) && lhs->name == target)
        return {{rhs, nullptr, ""}};

    // lhs = -expr → expr = -rhs
    if (is_neg(lhs) && contains_var(lhs->child, target))
        return recurse(lhs->child, Expr::Neg(rhs));

    // lhs = abs(inner) → inner = rhs OR inner = -rhs (both : rhs >= 0)
    if (lhs->type == ExprType::FUNC_CALL && lhs->name == "abs"
        && lhs->args.size() == 1 && contains_var(lhs->args[0], target)) {
        auto pos = solve_by_inversion(lhs->args[0], rhs, target, depth + 1);
        auto neg = solve_by_inversion(lhs->args[0], simplify(Expr::Neg(rhs)), target, depth + 1);
        // Add condition: rhs >= 0
        for (auto& s : pos) {
            s.condition = rhs;
            s.cond_desc = expr_to_string(rhs) + " >= 0";
        }
        for (auto& s : neg) {
            s.condition = rhs;
            s.cond_desc = expr_to_string(rhs) + " >= 0";
        }
        pos.insert(pos.end(), neg.begin(), neg.end());
        return pos;
    }

    // lhs = f(inner) where f has an inverse → inner = f⁻¹(rhs)
    if (lhs->type == ExprType::FUNC_CALL && lhs->args.size() == 1
        && contains_var(lhs->args[0], target)) {
        auto& inv_table = inverse_functions();
        auto it = inv_table.find(lhs->name);
        if (it != inv_table.end()) {
            ExprPtr new_rhs;
            if (it->second == "exp")
                new_rhs = Expr::BinOpExpr(BinOp::POW, Expr::Var("e"), rhs);
            else if (it->second == "sqr")
                new_rhs = Expr::BinOpExpr(BinOp::POW, rhs, Expr::Num(2));
            else
                new_rhs = Expr::Call(it->second, {rhs});
            return recurse(lhs->args[0], new_rhs);
        }
    }

    // lhs = base ^ exp
    if (lhs->type == ExprType::BINOP && lhs->op == BinOp::POW) {
        if (contains_var(lhs->left, target) && !contains_var(lhs->right, target)) {
            auto inv_exp = simplify(Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), lhs->right));
            return recurse(lhs->left, Expr::BinOpExpr(BinOp::POW, rhs, inv_exp));
        }
        if (contains_var(lhs->right, target) && !contains_var(lhs->left, target)) {
            ExprPtr new_rhs;
            if (is_var(lhs->left) && lhs->left->name == "e")
                new_rhs = Expr::Call("log", {rhs});
            else
                new_rhs = Expr::BinOpExpr(BinOp::DIV,
                    Expr::Call("log", {rhs}), Expr::Call("log", {lhs->left}));
            return recurse(lhs->right, new_rhs);
        }
    }

    // lhs = a / b
    if (lhs->type == ExprType::BINOP && lhs->op == BinOp::DIV) {
        if (contains_var(lhs->left, target) && !contains_var(lhs->right, target))
            return recurse(lhs->left, Expr::BinOpExpr(BinOp::MUL, rhs, lhs->right));
        if (contains_var(lhs->right, target) && !contains_var(lhs->left, target))
            return recurse(lhs->right, Expr::BinOpExpr(BinOp::DIV, lhs->left, rhs));
    }

    // lhs = a * b
    if (lhs->type == ExprType::BINOP && lhs->op == BinOp::MUL) {
        if (contains_var(lhs->left, target) && !contains_var(lhs->right, target))
            return recurse(lhs->left, Expr::BinOpExpr(BinOp::DIV, rhs, lhs->right));
        if (contains_var(lhs->right, target) && !contains_var(lhs->left, target))
            return recurse(lhs->right, Expr::BinOpExpr(BinOp::DIV, rhs, lhs->left));
    }

    // lhs = a + b
    if (lhs->type == ExprType::BINOP && lhs->op == BinOp::ADD) {
        if (contains_var(lhs->left, target) && !contains_var(lhs->right, target))
            return recurse(lhs->left, Expr::BinOpExpr(BinOp::SUB, rhs, lhs->right));
        if (contains_var(lhs->right, target) && !contains_var(lhs->left, target))
            return recurse(lhs->right, Expr::BinOpExpr(BinOp::SUB, rhs, lhs->left));
    }

    // lhs = a - b
    if (lhs->type == ExprType::BINOP && lhs->op == BinOp::SUB) {
        if (contains_var(lhs->left, target) && !contains_var(lhs->right, target))
            return recurse(lhs->left, Expr::BinOpExpr(BinOp::ADD, rhs, lhs->right));
        if (contains_var(lhs->right, target) && !contains_var(lhs->left, target))
            return recurse(lhs->right, Expr::BinOpExpr(BinOp::SUB, lhs->left, rhs));
    }

    return {}; // can't peel further
}


// Return ALL solutions (multiple for abs, quadratic, etc.)
inline std::vector<Solution> solve_for_all(const ExprPtr& lhs, const ExprPtr& rhs,
        const std::string& target) {
    // First try linear decomposition (fast, single solution)
    auto combined = simplify(Expr::BinOpExpr(BinOp::SUB, lhs, rhs));
    auto decomp = decompose_linear(combined, target);
    if (decomp) {
        auto sc = simplify(decomp->coeff);
        if (!(is_num(sc) && std::abs(sc->num) < EPSILON_ZERO)) {
            auto sr = simplify(decomp->rest);
            if (!(is_num(sr) && std::abs(sr->num) < EPSILON_ZERO && !is_num(sc))) {
                auto result = simplify(Expr::BinOpExpr(BinOp::DIV,
                    simplify(Expr::Neg(sr)), sc));
                return {{result, nullptr, ""}};
            }
        }
    }

    // Fall back to recursive inversion (may return multiple solutions)
    if (contains_var(lhs, target) && !contains_var(rhs, target))
        return solve_by_inversion(lhs, rhs, target);
    if (contains_var(rhs, target) && !contains_var(lhs, target))
        return solve_by_inversion(rhs, lhs, target);

    return {};
}

// Single-solution wrapper (backwards compatible — returns first solution)
inline ExprPtr solve_for(const ExprPtr& lhs, const ExprPtr& rhs, const std::string& target) {
    auto sols = solve_for_all(lhs, rhs, target);
    return sols.empty() ? nullptr : sols[0].expr;
}

// ============================================================================
//  Numeric root-finding (for nonlinear equations)
// ============================================================================

constexpr int    NUMERIC_MAX_ITER      = 200;
constexpr double NUMERIC_TOLERANCE     = 1e-10;
constexpr double NUMERIC_DEFAULT_LO    = -1000.0;
constexpr double NUMERIC_DEFAULT_HI    =  1000.0;
constexpr int    NUMERIC_DEFAULT_SAMPLES = 200;  // coarse scan points (fine = 5x)
constexpr double NUMERIC_JITTER_FRAC   = 0.1;
constexpr uint64_t NUMERIC_SEED        = 0x46'77'69'7A; // "Fwiz"

static_assert(NUMERIC_MAX_ITER > 0 && NUMERIC_MAX_ITER <= 10000);
static_assert(NUMERIC_TOLERANCE > 0 && NUMERIC_TOLERANCE < 1e-4);
static_assert(NUMERIC_DEFAULT_SAMPLES >= 10);

// Snap to nearest integer if within tolerance
inline double snap_integer(double x, double tol = EPSILON_ZERO) {
    double r = std::round(x);
    return std::abs(x - r) < tol ? r : x;
}

// Newton's method: solve f(x) = 0 starting from x0.
// Uses central finite differences for derivative.
inline std::optional<double> newton_solve(
        const std::function<double(double)>& f, double x0,
        int max_iter = NUMERIC_MAX_ITER, double tol = NUMERIC_TOLERANCE) {
    double x = x0;
    for (int i = 0; i < max_iter; i++) {
        double fx = f(x);
        if (std::isnan(fx) || std::isinf(fx)) return std::nullopt;
        if (std::abs(fx) < tol) return snap_integer(x);

        // Central difference derivative
        double h = std::max(1e-8, std::abs(x) * 1e-8);
        double fp = (f(x + h) - f(x - h)) / (2.0 * h);
        if (std::isnan(fp) || std::isinf(fp) || std::abs(fp) < 1e-15)
            return std::nullopt; // flat or singular — can't continue

        double x_new = x - fx / fp;
        if (std::isnan(x_new) || std::isinf(x_new)) return std::nullopt;

        // Divergence guard: damp step if too large
        if (std::abs(x_new) > 2.0 * std::abs(x) + 100.0)
            x_new = x - 0.5 * fx / fp;

        if (std::abs(x_new - x) < tol) return snap_integer(x_new);
        x = x_new;
    }
    // Check if final value is close enough
    double fx = f(x);
    if (!std::isnan(fx) && std::abs(fx) < tol * 100) return snap_integer(x);
    return std::nullopt;
}

// Bisection: find root of f in [lo, hi] where f(lo) and f(hi) have opposite signs.
inline std::optional<double> bisection_solve(
        const std::function<double(double)>& f, double lo, double hi,
        int max_iter = NUMERIC_MAX_ITER, double tol = NUMERIC_TOLERANCE) {
    double flo = f(lo), fhi = f(hi);
    if (std::isnan(flo) || std::isnan(fhi)) return std::nullopt;
    if (flo * fhi > 0) return std::nullopt; // no sign change

    for (int i = 0; i < max_iter; i++) {
        double mid = (lo + hi) / 2.0;
        double fmid = f(mid);
        if (std::isnan(fmid) || std::isinf(fmid)) return std::nullopt;
        if (std::abs(fmid) < tol || (hi - lo) < tol)
            return snap_integer(mid);
        if (flo * fmid < 0) { hi = mid; fhi = fmid; }
        else                { lo = mid; flo = fmid; }
    }
    return snap_integer((lo + hi) / 2.0);
}

// Adaptive grid scan: find intervals where f changes sign.
// Uses coarse pass with jitter, then refines near sign changes and high-gradient regions.
// Deterministic: uses fixed seed for reproducible jitter.
inline std::vector<std::pair<double, double>> adaptive_scan(
        const std::function<double(double)>& f, double lo, double hi,
        bool integer_only = false, int n_samples = NUMERIC_DEFAULT_SAMPLES) {
    struct Sample { double x, fx; };
    std::vector<Sample> samples;

    if (integer_only) {
        int ilo = static_cast<int>(std::ceil(lo));
        int ihi = static_cast<int>(std::floor(hi));
        for (int i = ilo; i <= ihi; i++) {
            double fx = f(static_cast<double>(i));
            if (std::isfinite(fx)) samples.push_back({static_cast<double>(i), fx});
        }
    } else {
        // Coarse pass with deterministic jitter
        std::mt19937_64 rng(NUMERIC_SEED);
        std::uniform_real_distribution<double> jitter(-NUMERIC_JITTER_FRAC, NUMERIC_JITTER_FRAC);
        double step = (hi - lo) / n_samples;
        for (int i = 0; i <= n_samples; i++) {
            double x = lo + i * step;
            if (i > 0 && i < n_samples)
                x += jitter(rng) * step; // jitter interior points
            double fx = f(x);
            if (std::isfinite(fx)) samples.push_back({x, fx});
        }

        // Find regions of interest: sign changes and high gradient
        std::vector<std::pair<double, double>> refine_regions;
        for (size_t i = 1; i < samples.size(); i++) {
            bool sign_change = samples[i-1].fx * samples[i].fx < 0;
            double gradient = std::abs(samples[i].fx - samples[i-1].fx)
                            / std::max(1e-15, samples[i].x - samples[i-1].x);
            // Refine near sign changes and steep gradients
            double avg_grad = std::abs(samples[i].fx + samples[i-1].fx)
                            / std::max(1e-15, hi - lo);
            if (sign_change || gradient > avg_grad * 10)
                refine_regions.push_back({samples[i-1].x, samples[i].x});
        }

        // Refine pass: add dense samples in regions of interest
        int fine_points = n_samples * 5;
        int points_per_region = refine_regions.empty() ? 0
            : fine_points / static_cast<int>(refine_regions.size());
        points_per_region = std::min(points_per_region, fine_points);
        for (auto& [rlo, rhi] : refine_regions) {
            // Expand region slightly
            double margin = (rhi - rlo) * 0.5;
            double elo = std::max(lo, rlo - margin);
            double ehi = std::min(hi, rhi + margin);
            double rstep = (ehi - elo) / std::max(1, points_per_region);
            for (int i = 0; i <= points_per_region; i++) {
                double x = elo + i * rstep;
                double fx = f(x);
                if (std::isfinite(fx)) samples.push_back({x, fx});
            }
        }

        // Sort all samples by x
        std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.x < b.x; });
    }

    // Collect sign-change intervals and exact zeros
    std::vector<std::pair<double, double>> intervals;
    for (size_t i = 0; i < samples.size(); i++) {
        if (std::abs(samples[i].fx) < NUMERIC_TOLERANCE) {
            // Exact zero — return degenerate interval [x, x]
            intervals.push_back({samples[i].x, samples[i].x});
        }
        if (i > 0 && samples[i-1].fx * samples[i].fx < 0) {
            intervals.push_back({samples[i-1].x, samples[i].x});
        }
    }
    return intervals;
}

// Find all numeric roots of f(x) = 0 in [lo, hi].
// Uses adaptive scan to find intervals, then refines with Newton/bisection.
inline std::vector<double> find_numeric_roots(
        const std::function<double(double)>& f, double lo, double hi,
        bool integer_only = false, int n_samples = NUMERIC_DEFAULT_SAMPLES) {
    auto intervals = adaptive_scan(f, lo, hi, integer_only, n_samples);
    std::vector<double> roots;

    for (auto& [a, b] : intervals) {
        std::optional<double> root;

        if (integer_only) {
            // For integers, just check exact values
            int ia = static_cast<int>(std::round(a));
            int ib = static_cast<int>(std::round(b));
            for (int i = ia; i <= ib; i++) {
                double fx = f(static_cast<double>(i));
                if (std::abs(fx) < NUMERIC_TOLERANCE) {
                    root = static_cast<double>(i);
                    break;
                }
            }
        } else if (std::abs(a - b) < NUMERIC_TOLERANCE) {
            // Degenerate interval — exact zero found during scan
            root = snap_integer(a);
        } else {
            // Try Newton from midpoint, fallback to bisection
            double mid = (a + b) / 2.0;
            root = newton_solve(f, mid);
            if (!root || *root < a - 1.0 || *root > b + 1.0)
                root = bisection_solve(f, a, b);
        }

        if (root) {
            // Post-validate: reject false roots (singularities)
            double fr = f(*root);
            if (std::isnan(fr) || std::abs(fr) > NUMERIC_TOLERANCE * 1000) continue;

            // Deduplicate
            bool dup = false;
            for (double r : roots)
                if (std::abs(r - *root) < EPSILON_ZERO) { dup = true; break; }
            if (!dup) roots.push_back(*root);
        }
    }

    std::sort(roots.begin(), roots.end());
    return roots;
}
