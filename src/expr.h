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
constexpr int    RATIONAL_POW_MAX_EXP = 20; // max integer exponent for (p/q)^n structural expansion — larger exponents fall back to double POW (int64 overflow risk for non-tiny bases)

// Scale factor for hashing fingerprint doubles into int64 buckets in derive_all.
// Tied to EPSILON_REL so two values within EPSILON_REL map to the same bucket
// (see fingerprint_expr / canonicity_score in this file, and the dedup pipeline
// in system.h).
constexpr int64_t FINGERPRINT_SCALE = 1000000000;  // = 1 / EPSILON_REL

static_assert(EPSILON_ZERO > 0 && EPSILON_ZERO < 1e-6, "EPSILON_ZERO must be a small positive value");
static_assert(EPSILON_REL > 0 && EPSILON_REL < 1e-3, "EPSILON_REL must be a small positive value");
static_assert(SIMPLIFY_MAX_ITER > 0 && SIMPLIFY_MAX_ITER < 1000, "SIMPLIFY_MAX_ITER must be reasonable");
static_assert(RATIONAL_POW_MAX_EXP > 0 && RATIONAL_POW_MAX_EXP < 64, "RATIONAL_POW_MAX_EXP must fit comfortably within int64 iteration");
static_assert(static_cast<double>(FINGERPRINT_SCALE) * EPSILON_REL == 1.0,
              "FINGERPRINT_SCALE must equal 1/EPSILON_REL so bucket size matches tolerance");

// ============================================================================
//  Checked<T> — NaN-sentinel wrapper for floating-point evaluate() results.
//  Empty (no-value) state is encoded as quiet_NaN.
//  sizeof(Checked<double>) == sizeof(double) — no hidden bool discriminant.
//
//  Requires std::numeric_limits<T>::has_quiet_NaN.
//
//  Intentional API constraints:
//    - No operator*         — prevents silent crash on empty dereference
//    - No value_or(default) — prevents trivial bypass of check discipline
//    - No operator==        — no call site needs it; avoids NaN-identity trap
//    - operator bool is explicit — matches std::optional; `if (v)` works
//    - value() asserts on empty — debug abort, not exception
//    - value_or_nan() — named boundary escape for symbolic→numeric handoffs
//      (e.g. into find_numeric_roots, which is a pure-double consumer with
//      its own isfinite checks). Using this IS a deliberate statement.
//
//  NaN IS the empty sentinel: passing a NaN into the engaged constructor
//  (e.g. from eval_div(1,0) propagating) yields an empty Checked. This is
//  intentional — legitimate IEEE-754 NaN propagation must not assert-fail.
// ============================================================================

template<typename T>
class Checked {
    static_assert(std::numeric_limits<T>::has_quiet_NaN,
        "Checked<T> requires a floating-point type with a quiet NaN sentinel");

    T val_;

public:
    // Empty state — stores quiet_NaN.
    Checked() noexcept
        : val_(std::numeric_limits<T>::quiet_NaN()) {}

    // Engaged construction — implicit from T intentional.
    // If v is NaN (legitimate IEEE-754 propagation, e.g. from eval_div(1,0)),
    // the result is empty per the "NaN IS the empty sentinel" contract. No
    // assert — NaN-in is a total, deliberate part of the contract.
    // cppcheck-suppress noExplicitConstructor
    /*implicit*/ Checked(T v) noexcept : val_(v) {} // NOLINT(google-explicit-constructor)

    bool has_value()         const noexcept { return !std::isnan(val_); }
    explicit operator bool() const noexcept { return has_value(); }

    // Value access. Asserts in debug on empty; UB in release
    // (assert-postcondition style, matching std::optional::operator*).
    T value() const noexcept {
        assert(has_value() && "Checked<T>::value() called on empty");
        return val_;
    }

    // Boundary escape: hands off the raw T (quiet_NaN when empty) to code
    // outside the Checked ecosystem — specifically the numerical root-finder
    // library (find_numeric_roots, adaptive_scan, newton_solve, bisection_solve),
    // which is a pure-double algorithm layer with its own isfinite discipline.
    // Using this operator IS an explicit, reviewable statement of intent.
    T value_or_nan() const noexcept { return val_; }
};

static_assert(sizeof(Checked<double>) == sizeof(double),
    "Checked<double> must be the same size as double — no hidden bool padding");

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
        for (const auto& iv : intervals_)
            if (iv.contains(v)) return true;
        for (const auto& d : discrete_)
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
        for (const auto& a : intervals_)
            for (const auto& b : other.intervals_) {
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
        for (const auto& d : discrete_)
            if (other.contains(d)) result.discrete_.push_back(d);
        for (const auto& d : other.discrete_)
            if (this->contains(d)) {
                // Avoid duplicates
                bool dup = false;
                for (const auto& rd : result.discrete_)
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
        for (const auto& d : other.discrete_) {
            bool dup = false;
            for (const auto& rd : result.discrete_)
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

    // Does this set cover all real numbers (-inf, +inf)?
    // Checks if intervals + discrete points leave no gaps.
    bool covers_reals() const {
        if (intervals_.empty() && discrete_.empty()) return false;
        // Merge all coverage into sorted intervals (discrete points become [v,v])
        std::vector<Interval> all_intervals = intervals_;
        for (double d : discrete_)
            all_intervals.push_back({d, d, true, true});
        std::sort(all_intervals.begin(), all_intervals.end(),
            [](const auto& a, const auto& b) {
                return a.low < b.low || (a.low == b.low && a.low_inclusive > b.low_inclusive);
            });
        // Walk intervals tracking coverage boundary and its inclusivity
        constexpr double INF = std::numeric_limits<double>::infinity();
        double covered_to = -INF;
        bool covered_inclusive = false;  // is covered_to itself included?
        for (const auto& iv : all_intervals) {
            // Can this interval extend from where we left off?
            if (iv.low > covered_to) return false;  // numeric gap
            if (iv.low == covered_to && covered_to != -INF
                && !covered_inclusive && !iv.low_inclusive)
                return false;  // both sides open at the boundary
            // Extend coverage
            if (iv.high > covered_to) {
                covered_to = iv.high;
                covered_inclusive = iv.high_inclusive;
            } else if (iv.high == covered_to) {
                covered_inclusive = covered_inclusive || iv.high_inclusive;
            }
        }
        return covered_to == INF;
    }

    // String representation
    std::string to_string() const {
        if (empty()) return "{}";

        std::vector<std::string> parts;

        for (const auto& iv : intervals_) {
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
        explicit Scope(ExprArena& a) : prev(current_) { current_ = &a; }
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
// Guard against accidental field additions — sizeof(Expr) is a cache/arena
// concern. If this fails, reconsider whether the new field belongs in Expr
// or in an auxiliary map keyed by ExprPtr. See docs/Developer.md for the rationale.
static_assert(sizeof(Expr) == 96, "sizeof(Expr) changed — update static_assert and audit cache/arena impact");

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
inline bool is_num(const Expr* e)     { return e && is_num(*e); }
inline bool is_var(const Expr* e)     { return e && is_var(*e); }
inline bool is_atomic(const Expr* e)  { return e && is_atomic(*e); }
inline bool is_zero(const Expr* e)    { return e && is_zero(*e); }
inline bool is_one(const Expr* e)     { return e && is_one(*e); }
inline bool is_neg_one(const Expr* e) { return e && is_neg_one(*e); }
inline bool is_neg(const Expr* e)     { return e && is_neg(*e); }
inline bool is_neg_num(const Expr* e) { return e && is_neg_num(*e); }

constexpr bool is_additive(BinOp op)       { return op == BinOp::ADD || op == BinOp::SUB; }
constexpr bool is_multiplicative(BinOp op) { return op == BinOp::MUL || op == BinOp::DIV; }

// ============================================================================
//  Rational number helpers (structural fractions)
// ============================================================================
// Rational numbers are represented as DIV(Num(a), Num(b)) in the expression tree.
// This avoids adding fields to Expr and preserves sizeof(Expr).

// Is this a double value that's an exact integer?
inline bool is_integer_value(double v) {
    return std::abs(v) < 1e15 && v == std::floor(v);
}

// Is this a structural fraction: DIV(Num(int), Num(int))?
inline bool is_int_frac(const Expr& e) {
    return e.type == ExprType::BINOP && e.op == BinOp::DIV
        && is_num(*e.left) && is_num(*e.right)
        && is_integer_value(e.left->num) && is_integer_value(e.right->num)
        && e.right->num != 0;
}
inline bool is_int_frac(const ExprPtr e) { return e && is_int_frac(*e); }

// Extract rational (numer, denom) from a Num or structural fraction.
// Returns {n, 1} for plain integers, {p, q} for DIV(Num(p), Num(q)).
inline std::pair<int64_t, int64_t> to_rational(const Expr& e) {
    if (is_int_frac(e))
        return {static_cast<int64_t>(e.left->num), static_cast<int64_t>(e.right->num)};
    if (is_num(e) && is_integer_value(e.num))
        return {static_cast<int64_t>(e.num), 1};
    return {0, 0}; // not rational
}
inline std::pair<int64_t, int64_t> to_rational(const ExprPtr e) {
    return e ? to_rational(*e) : std::pair<int64_t, int64_t>{0, 0};
}

// GCD for normalization
inline int64_t gcd_abs(int64_t a, int64_t b) {
    a = std::abs(a); b = std::abs(b);
    while (b) { a %= b; std::swap(a, b); }
    return a;
}

// Build a normalized rational expression: GCD-reduced, sign in numerator.
// Returns Num(n) if denominator is 1 after reduction.
inline ExprPtr make_rational(int64_t numer, int64_t denom) {
    assert(denom != 0 && "make_rational: zero denominator");
    if (numer == 0) return Expr::Num(0);
    // Sign normalization: negative in numerator only
    if (denom < 0) { numer = -numer; denom = -denom; }
    // GCD reduction
    int64_t g = gcd_abs(numer, denom);
    numer /= g; denom /= g;
    if (denom == 1) return Expr::Num(static_cast<double>(numer));
    return Expr::BinOpExpr(BinOp::DIV,
        Expr::Num(static_cast<double>(numer)),
        Expr::Num(static_cast<double>(denom)));
}

// ============================================================================
//  BinOp metadata
// ============================================================================

struct BinOpInfo {
    const char* symbol;
    int precedence;
    double (*eval)(double, double);
};

inline double eval_div(double l, double r) {
    return r == 0 ? std::numeric_limits<double>::quiet_NaN() : l / r;
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

// Thread-local custom function registry (set by FormulaSystem for per-system functions)
inline const std::map<std::string, double(*)(double)>*& custom_functions_ptr_() {
    static thread_local const std::map<std::string, double(*)(double)>* p = nullptr;
    return p;
}

// Look up a function by name: custom first, then builtin
inline double(*lookup_function(const std::string& name))(double) {
    if (auto* custom = custom_functions_ptr_()) {
        auto it = custom->find(name);
        if (it != custom->end()) return it->second;
    }
    auto& builtins = builtin_functions();
    auto it = builtins.find(name);
    return (it != builtins.end()) ? it->second : nullptr;
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
//  Undefined: symbolic domain boundary
// ============================================================================

// "undefined" is represented as Var("undefined") — no parser changes needed.
// It propagates through arithmetic (like NaN) and throws at evaluation time.
inline bool is_undefined(const ExprPtr& e) {
    return e && e->type == ExprType::VAR && e->name == "undefined";
}

// ============================================================================
//  Tree queries
// ============================================================================

inline void collect_vars(const Expr& e, std::set<std::string>& out) {
    switch (e.type) {
        case ExprType::NUM:       break;
        case ExprType::VAR:       if (e.name != "undefined") out.insert(e.name); break;
        case ExprType::BINOP:     collect_vars(*e.left, out); collect_vars(*e.right, out); break;
        case ExprType::UNARY_NEG: collect_vars(*e.child, out); break;
        case ExprType::FUNC_CALL: for (const auto* a : e.args) collect_vars(*a, out); break;
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
        case ExprType::FUNC_CALL: for (const auto* a : e.args) if (contains_var(*a, v)) return true;
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
inline void collect_vars(const Expr* e, std::set<std::string>& out) { if (e) collect_vars(*e, out); }
inline bool contains_var(const Expr* e, const std::string& v) { return e && contains_var(*e, v); }
inline bool expr_equal(const Expr* a, const Expr* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return expr_equal(*a, *b);
}

// ============================================================================
//  Pattern matching for rewrite rules
// ============================================================================

// Match a pattern expression against a target expression.
// Variables in the pattern are wildcards — they bind to any sub-expression.
// Numbers and operators must match exactly.
// Returns bindings map on success, nullopt on failure.
// Forward declarations for flattened matching
inline void flatten_additive(ExprPtr e, double sign,
                             std::vector<std::pair<double, ExprPtr>>& terms);
inline void flatten_multiplicative(ExprPtr e, double& coeff,
                                   std::vector<std::pair<ExprPtr, double>>& factors);

// Helper: is this op commutative?
inline constexpr bool is_commutative(BinOp op) {
    return op == BinOp::ADD || op == BinOp::MUL;
}

// Helper: is this an additive chain (ADD/SUB at top)?
inline bool is_additive_chain(const ExprPtr& e) {
    return e && e->type == ExprType::BINOP && is_additive(e->op);
}

inline std::optional<std::map<std::string, ExprPtr>> match_pattern(
        const ExprPtr& pattern, const ExprPtr& target) {
    if (!pattern || !target) return std::nullopt;
    std::map<std::string, ExprPtr> bindings;

    // Decompose an additive term into its multiplicative factors for matching.
    // Returns {numeric_coeff, [(base, exponent)]} where base^exponent are the factors.
    struct TermFactors {
        double coeff;
        std::vector<std::pair<ExprPtr, double>> factors; // base^exp pairs
    };

    auto decompose_term = [](double additive_coeff, const ExprPtr& base) -> TermFactors {
        TermFactors tf;
        tf.coeff = additive_coeff;
        if (!base) return tf;  // pure constant
        double mul_coeff = 1.0;
        flatten_multiplicative(base, mul_coeff, tf.factors);
        tf.coeff *= mul_coeff;
        return tf;
    };

    std::function<bool(const ExprPtr&, const ExprPtr&)> match;

    // Match a pattern term's multiplicative factors against a target term's factors.
    // Pattern wildcards that don't match any target factor bind to 1.
    auto match_factors = [&](const TermFactors& p, const TermFactors& t) -> bool {
        // Separate pattern factors into wildcards (plain vars) and structural
        std::vector<size_t> p_wildcards, p_structural;
        for (size_t i = 0; i < p.factors.size(); i++) {
            auto& [base, exp] = p.factors[i];
            if (base->type == ExprType::VAR && !builtin_constants().count(base->name)
                && std::abs(exp - 1.0) < EPSILON_ZERO)
                p_wildcards.push_back(i);
            else
                p_structural.push_back(i);
        }

        // Match structural pattern factors against target factors
        std::vector<bool> t_used(t.factors.size(), false);
        for (size_t si : p_structural) {
            auto& [p_base, p_exp] = p.factors[si];
            bool found = false;
            for (size_t ti = 0; ti < t.factors.size(); ti++) {
                if (t_used[ti]) continue;
                auto& [t_base, t_exp] = t.factors[ti];
                if (std::abs(p_exp - t_exp) > EPSILON_ZERO) continue;
                auto saved = bindings;
                if (match(p_base, t_base)) {
                    t_used[ti] = true;
                    found = true;
                    break;
                }
                bindings = saved;
            }
            if (!found) return false;
        }

        // Collect remaining target factors into a product
        double remaining_coeff = (std::abs(p.coeff) < EPSILON_ZERO) ? 0.0 : t.coeff / p.coeff;
        ExprPtr remaining = nullptr;
        for (size_t ti = 0; ti < t.factors.size(); ti++) {
            if (t_used[ti]) continue;
            auto& [t_base, t_exp] = t.factors[ti];
            auto factor = (std::abs(t_exp - 1.0) < EPSILON_ZERO) ? t_base
                : Expr::BinOpExpr(BinOp::POW, t_base, Expr::Num(t_exp));
            remaining = remaining ? Expr::BinOpExpr(BinOp::MUL, remaining, factor) : factor;
        }

        if (p_wildcards.empty())
            return !remaining && std::abs(remaining_coeff - 1.0) < EPSILON_ZERO;

        // Collect remaining target factors as individual expressions
        std::vector<ExprPtr> t_remaining;
        for (size_t ti = 0; ti < t.factors.size(); ti++) {
            if (t_used[ti]) continue;
            auto& [t_base, t_exp] = t.factors[ti];
            t_remaining.push_back((std::abs(t_exp - 1.0) < EPSILON_ZERO) ? t_base
                : Expr::BinOpExpr(BinOp::POW, t_base, Expr::Num(t_exp)));
        }

        // Try to assign remaining target factors to wildcards via backtracking
        // Unassigned wildcards get the numeric coefficient (or 1)
        std::vector<bool> t_rem_used(t_remaining.size(), false);
        std::function<bool(size_t)> assign_wildcards = [&](size_t wi) -> bool {
            if (wi == p_wildcards.size()) {
                // All wildcards assigned; check no unmatched target factors
                for (bool u : t_rem_used) if (!u) return false;
                return true;
            }
            auto& var_name = p.factors[p_wildcards[wi]].first->name;

            // Try matching this wildcard against each remaining target factor
            for (size_t ri = 0; ri < t_remaining.size(); ri++) {
                if (t_rem_used[ri]) continue;
                auto saved = bindings;
                auto it = bindings.find(var_name);
                bool ok = false;
                if (it != bindings.end())
                    ok = expr_equal(it->second, t_remaining[ri]);
                else {
                    bindings[var_name] = t_remaining[ri];
                    ok = true;
                }
                if (ok) {
                    t_rem_used[ri] = true;
                    if (assign_wildcards(wi + 1)) return true;
                    t_rem_used[ri] = false;
                }
                bindings = saved;
            }

            // Try binding this wildcard to the numeric coefficient
            if (std::abs(remaining_coeff - 1.0) > EPSILON_ZERO || t_remaining.empty()) {
                auto saved = bindings;
                auto it = bindings.find(var_name);
                bool ok = false;
                if (it != bindings.end())
                    ok = is_num(it->second) && std::abs(it->second->num - remaining_coeff) < EPSILON_ZERO;
                else {
                    bindings[var_name] = Expr::Num(remaining_coeff);
                    ok = true;
                }
                if (ok) {
                    double saved_coeff = remaining_coeff;
                    remaining_coeff = 1.0;  // consumed
                    if (assign_wildcards(wi + 1)) return true;
                    remaining_coeff = saved_coeff;
                }
                bindings = saved;
            }

            return false;
        };
        return assign_wildcards(0);
    };

    match = [&](const ExprPtr& p, const ExprPtr& t) -> bool {
        if (!p || !t) return p == t;

        // Variable in pattern: builtin constants match literally, others are wildcards
        if (p->type == ExprType::VAR) {
            if (builtin_constants().count(p->name))
                return t->type == ExprType::VAR && t->name == p->name;
            auto it = bindings.find(p->name);
            if (it != bindings.end())
                return expr_equal(it->second, t); // already bound — must match
            bindings[p->name] = t;
            return true;
        }

        // Number must match exactly
        if (p->type == ExprType::NUM)
            return t->type == ExprType::NUM && std::abs(p->num - t->num) < EPSILON_ZERO;

        // Negation
        if (p->type == ExprType::UNARY_NEG)
            return t->type == ExprType::UNARY_NEG && match(p->child, t->child);

        // Binary op — with commutativity and flattened matching
        if (p->type == ExprType::BINOP) {
            if (t->type != ExprType::BINOP) return false;

            // Flattened additive matching: when both are additive chains,
            // flatten into term lists and match by permutation
            if (is_additive(p->op) && is_additive(t->op)
                && (is_additive_chain(p->left) || is_additive_chain(p->right)
                    || is_additive_chain(t->left) || is_additive_chain(t->right))) {
                std::vector<std::pair<double, ExprPtr>> p_terms, t_terms;
                flatten_additive(p, 1.0, p_terms);
                flatten_additive(t, 1.0, t_terms);
                if (p_terms.size() == t_terms.size() && p_terms.size() > 1) {
                    // Decompose each term into multiplicative factors
                    std::vector<TermFactors> p_tf, t_tf;
                    for (auto& [c, b] : p_terms) p_tf.push_back(decompose_term(c, b));
                    for (auto& [c, b] : t_terms) t_tf.push_back(decompose_term(c, b));

                    // Backtracking permutation search over additive terms
                    std::vector<bool> used(t_tf.size(), false);
                    std::function<bool(size_t)> backtrack = [&](size_t pi) -> bool {
                        if (pi == p_tf.size()) return true;
                        for (size_t ti = 0; ti < t_tf.size(); ti++) {
                            if (used[ti]) continue;
                            auto saved = bindings;
                            if (match_factors(p_tf[pi], t_tf[ti])) {
                                used[ti] = true;
                                if (backtrack(pi + 1)) return true;
                                used[ti] = false;
                            }
                            bindings = saved;
                        }
                        return false;
                    };
                    return backtrack(0);
                }
            }

            // Standard binary match with commutativity
            if (p->op != t->op && !(is_additive(p->op) && is_additive(t->op)))
                return false;
            // For ADD/SUB: both are additive, handled above for chains.
            // For same-op: try direct, then swapped for commutative ops.
            if (p->op == t->op) {
                auto saved = bindings;
                if (match(p->left, t->left) && match(p->right, t->right))
                    return true;
                bindings = saved;
                if (is_commutative(p->op))
                    return match(p->left, t->right) && match(p->right, t->left);
                return false;
            }
            return false;
        }

        // Function call — name and all args must match
        if (p->type == ExprType::FUNC_CALL) {
            if (t->type != ExprType::FUNC_CALL || p->name != t->name) return false;
            if (p->args.size() != t->args.size()) return false;
            for (size_t i = 0; i < p->args.size(); i++)
                if (!match(p->args[i], t->args[i])) return false;
            return true;
        }

        return false;
    };

    if (match(pattern, target)) return bindings;
    return std::nullopt;
}

// Apply a rewrite: substitute bindings into the replacement template.
inline ExprPtr apply_rewrite(const ExprPtr& replacement,
        const std::map<std::string, ExprPtr>& bindings) {
    if (!replacement) return nullptr;

    if (replacement->type == ExprType::VAR) {
        auto it = bindings.find(replacement->name);
        return (it != bindings.end()) ? it->second : replacement;
    }
    if (replacement->type == ExprType::NUM) return replacement;
    if (replacement->type == ExprType::UNARY_NEG)
        return Expr::Neg(apply_rewrite(replacement->child, bindings));
    if (replacement->type == ExprType::BINOP)
        return Expr::BinOpExpr(replacement->op,
            apply_rewrite(replacement->left, bindings),
            apply_rewrite(replacement->right, bindings));
    if (replacement->type == ExprType::FUNC_CALL) {
        std::vector<ExprPtr> args;
        args.reserve(replacement->args.size());
        for (auto& a : replacement->args)
            args.push_back(apply_rewrite(a, bindings));
        return Expr::Call(replacement->name, args);
    }
    return replacement;
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
inline int precedence(const Expr* e) { return e ? precedence(*e) : 5; }
inline std::string expr_to_string(const Expr* e) { return e ? expr_to_string(*e) : "?"; }

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
//  CSE substitution — replace structural-equal subtrees with named Var nodes
// ============================================================================
//
// Used by --cse derive output: given an expression and an ordered list of
// (helper_name, helper_subtree) pairs, replace every occurrence of a helper
// subtree (matched by structural equality) with `Var(helper_name)`.
//
// Walk is post-order (children first), preserving SymPy `cse()` let* semantics:
// a helper's RHS is matched against the ORIGINAL subtree, not the substituted
// one. The current node is checked AFTER its children so an outer helper still
// matches its original shape if its inner helper has not yet rewritten it.
//
// Pointer-equality short-circuit on the no-match path: fwiz's factory pattern
// (Expr::BinOpExpr/Neg/Call) ALWAYS reconstructs a fresh node, so without this
// guard a tree with no helper matches still pays O(|tree|) allocations. The
// guard returns the original `e` when (a) no child changed by pointer identity
// AND (b) no helper subtree equals the current node.
inline ExprPtr cse_replace(ExprPtr e,
        const std::vector<std::pair<std::string, ExprPtr>>& helpers) {
    if (!e) return e;
    // Walk children first (post-order).
    ExprPtr new_left = nullptr, new_right = nullptr, new_child = nullptr;
    std::vector<ExprPtr> new_args;
    bool args_changed = false;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            break;  // atomic; no children to walk
        case ExprType::UNARY_NEG:
            new_child = cse_replace(e->child, helpers);
            break;
        case ExprType::BINOP:
            new_left  = cse_replace(e->left,  helpers);
            new_right = cse_replace(e->right, helpers);
            break;
        case ExprType::FUNC_CALL:
            new_args.reserve(e->args.size());
            for (auto& a : e->args) {
                auto na = cse_replace(a, helpers);
                if (na != a) args_changed = true;
                new_args.push_back(na);
            }
            break;
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    // Now check helpers against the current subtree. We use post-order so a
    // helper always matches the original shape (helpers themselves never
    // contain Var(t_i) for any helper name).
    for (auto& [name, sub] : helpers) {
        if (expr_equal(e, sub)) return Expr::Var(name);
    }
    // Pointer-equality short-circuit: nothing changed in children AND no helper
    // matched at this node → return original (avoids O(|tree|) rebuild).
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return e;
        case ExprType::UNARY_NEG:
            if (new_child == e->child) return e;
            return Expr::Neg(new_child);
        case ExprType::BINOP:
            if (new_left == e->left && new_right == e->right) return e;
            return Expr::BinOpExpr(e->op, new_left, new_right);
        case ExprType::FUNC_CALL:
            if (!args_changed) return e;
            return Expr::Call(e->name, new_args);
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

// Walk an expression tree and replace every Var node whose name is a builtin
// symbolic constant (pi, e, phi) with its numeric Num value. Used by the
// --approximate derive path to collapse `2 * pi * r` → `6.28... * r` after
// simplification. User-defined defaults (e.g. g = 9.81) are NOT touched —
// the source of truth is builtin_constants() which holds only the true
// mathematical constants.
inline ExprPtr substitute_builtin_constants(ExprPtr e) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:       return e;
        case ExprType::VAR: {
            auto& consts = builtin_constants();
            auto it = consts.find(e->name);
            if (it != consts.end()) return Expr::Num(it->second);
            return e;
        }
        case ExprType::UNARY_NEG: return Expr::Neg(substitute_builtin_constants(e->child));
        case ExprType::BINOP:     return Expr::BinOpExpr(e->op,
                                      substitute_builtin_constants(e->left),
                                      substitute_builtin_constants(e->right));
        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> a;
            a.reserve(e->args.size());
            for (auto& arg : e->args) a.push_back(substitute_builtin_constants(arg));
            return Expr::Call(e->name, a);
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

// ============================================================================
//  Evaluate
// ============================================================================

inline Checked<double> evaluate(const Expr& e) {
    switch (e.type) {
        case ExprType::NUM: return e.num;
        case ExprType::VAR: {
            if (e.name == "undefined") return {};
            auto& consts = builtin_constants();
            auto it = consts.find(e.name);
            if (it != consts.end()) return it->second;
            return {};
        }
        case ExprType::UNARY_NEG: {
            auto v = evaluate(*e.child);
            if (!v) return {};
            return -v.value();
        }
        case ExprType::BINOP: {
            auto l = evaluate(*e.left);
            if (!l) return {};
            auto r = evaluate(*e.right);
            if (!r) return {};
            return binop_info(e.op).eval(l.value(), r.value());
        }
        case ExprType::FUNC_CALL: {
            if (e.args.size() != 1) return {};
            auto fn = lookup_function(e.name);
            if (!fn) return {};
            auto v = evaluate(*e.args[0]);
            if (!v) return {};
            return fn(v.value());
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return {};
}
inline Checked<double> evaluate(const Expr* e) {
    if (!e) return {};
    return evaluate(*e);
}

// ============================================================================
//  Semantic fingerprint primitive — evaluate an expression at a set of test
//  points with specified free-variable values. Used by derive_all to dedupe
//  numerically-equivalent candidates (commutative shuffles, algebraic
//  identities) without needing exact structural equality.
//
//  Points at which evaluate() returns empty (NaN, unresolved variable,
//  division by zero, etc.) are SKIPPED — not recorded. The returned vector
//  may therefore be shorter than test_points. An all-empty fingerprint
//  (returned as empty vector) signals a candidate whose domain excludes
//  every test point; callers must treat such candidates as non-merging
//  (usually by assigning a unique sentinel key).
// ============================================================================

inline std::vector<double> fingerprint_expr(
        ExprPtr e,
        const std::vector<std::string>& free_vars,
        const std::vector<std::map<std::string, double>>& test_points) {
    std::vector<double> result;
    if (!e) return result;
    result.reserve(test_points.size());
    for (const auto& point : test_points) {
        ExprPtr substituted = e;
        for (const auto& name : free_vars) {
            auto it = point.find(name);
            if (it == point.end()) continue;
            substituted = substitute(substituted, name, Expr::Num(it->second));
        }
        auto v = evaluate(substituted);
        if (!v) continue;
        double d = v.value();
        if (!std::isfinite(d)) continue;
        result.push_back(d);
    }
    return result;
}

// ============================================================================
//  Canonicity score — pair<leaf_count, non_integer_num_count>.
//  Lower is "more canonical" (lex compare via built-in pair ordering).
//  Size first (fewer leaves = simpler), canonical form second (integer NUM
//  leaves are NOT penalized — they're the cleanest form). Used by derive_all
//  to pick the best representative when two candidates share a fingerprint,
//  and to sort the emit list ascending from simple to complex.
// ============================================================================

inline std::pair<int, int> canonicity_score(ExprPtr e) {
    if (!e) return {0, 0};
    switch (e->type) {
        case ExprType::NUM:
            return {1, is_integer_value(e->num) ? 0 : 1};
        case ExprType::VAR:
            return {1, 0};
        case ExprType::UNARY_NEG:
            return canonicity_score(e->child);
        case ExprType::BINOP: {
            auto l = canonicity_score(e->left);
            auto r = canonicity_score(e->right);
            return {l.first + r.first, l.second + r.second};
        }
        case ExprType::FUNC_CALL: {
            std::pair<int, int> acc{0, 0};
            for (auto& a : e->args) {
                auto s = canonicity_score(a);
                acc.first += s.first;
                acc.second += s.second;
            }
            return acc;
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType");
    }
    return {0, 0};  // unreachable; pacify -Wreturn-type under future enum changes
}

// Symbolic sibling of evaluate(): preserves exact arithmetic in the returned
// tree (e.g. 1/3 stays as DIV(Num(1), Num(3))) instead of collapsing to a
// double. Used by the simplifier's constant-folding paths to centralize the
// rational-preservation guard.
//
// Rationals are the only non-real case handled today. Complex numbers,
// matrices, and other number types will extend the dispatch here without
// touching call sites. See docs/Future.md "Extending evaluate_symbolic".
//
// Numeric callers (Newton/bisection grid scan, condition comparisons,
// verify-mode equality, CLI arg parsing, solve_recursive bindings commit)
// must keep using `double evaluate()` — they intrinsically need real values
// with ordering.
inline ExprPtr evaluate_symbolic(const Expr& e) {
    if (e.type == ExprType::BINOP && is_num(e.left) && is_num(e.right)) {
        if (e.op == BinOp::DIV && e.right->num != 0
            && is_integer_value(e.left->num)
            && is_integer_value(e.right->num)) {
            return make_rational(static_cast<int64_t>(e.left->num),
                                 static_cast<int64_t>(e.right->num));
        }
        return Expr::Num(binop_info(e.op).eval(e.left->num, e.right->num));
    }
    if (e.type == ExprType::FUNC_CALL && lookup_function(e.name)) {
        bool all_num = true;
        for (const auto* a : e.args) if (!is_num(a)) { all_num = false; break; }
        // evaluate() can still return empty here (e.g. multi-arg function
        // with args.size() != 1) — fall through to tree-as-is on failure.
        if (all_num) {
            if (auto v = evaluate(e)) return Expr::Num(v.value());
        }
    }
    // Fall-through: not fully numeric-foldable — return the tree as-is
    // (arena-allocated copy so the caller can treat the result uniformly).
    // Unreachable from current simplifier call sites, which pre-guard on the
    // same predicates (is_num(l) && is_num(r) for BINOP, all_num for FUNC_CALL).
    // Reachable from direct callers exercising the public contract.
    auto out = ExprArena::current()->alloc();
    *out = e;
    return out;
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
    // Structural fraction first — must short-circuit before DIV decomposition would split it apart
    if (is_int_frac(e)) {
        factors.push_back({e, 1.0});
        return;
    }
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
    } else if (e->type == ExprType::BINOP && e->op == BinOp::DIV
               && e->right->type != ExprType::NUM) {
        // a / b → flatten a, then flatten b with negated exponents
        flatten_multiplicative(e->left, coeff, factors);
        double denom_coeff = 1.0;
        std::vector<std::pair<ExprPtr, double>> denom_factors;
        flatten_multiplicative(e->right, denom_coeff, denom_factors);
        if (std::abs(denom_coeff) > EPSILON_ZERO) coeff /= denom_coeff;
        for (auto& [base, exp] : denom_factors)
            factors.push_back({base, -exp});
    } else if (e->type == ExprType::BINOP && e->op == BinOp::POW
               && e->right->type == ExprType::NUM) {
        factors.push_back({e->left, e->right->num});
    } else {
        factors.push_back({e, 1.0});
    }
}

// Reconstruct an expression from multiplicative factors. Splits factors by
// exponent sign: positive-exp factors form the numerator, negative-exp factors
// (with sign flipped) form the denominator. Emits `DIV(num, denom)` when any
// negative-exp factors are present, avoiding `POW(_, Num(-n))` rendering.
inline ExprPtr rebuild_multiplicative(double coeff,
                                      const std::vector<std::pair<ExprPtr, double>>& factors) {
    auto make_factor = [](const ExprPtr& base, double exp) -> ExprPtr {
        if (exp == 1.0) return base;
        return Expr::BinOpExpr(BinOp::POW, base, Expr::Num(exp));
    };

    // Split factors by exponent sign: positive → numerator, negative → denominator
    std::vector<ExprPtr> num_parts, denom_parts;
    for (auto& [base, exp] : factors) {
        if (std::abs(exp) < EPSILON_ZERO) continue;  // base^0 = 1, skip
        if (exp > 0) num_parts.push_back(make_factor(base, exp));
        else         denom_parts.push_back(make_factor(base, -exp));
    }

    bool neg = coeff < 0;
    double abs_coeff = std::abs(coeff);

    // Build numerator: coeff (if not 1) * positive-exp factors
    ExprPtr num = nullptr;
    if (abs_coeff != 1.0 || (num_parts.empty() && denom_parts.empty())) num = Expr::Num(abs_coeff);
    for (auto& f : num_parts) num = num ? Expr::BinOpExpr(BinOp::MUL, num, f) : f;
    if (!num) num = Expr::Num(1);

    // If any negative-exp factors, wrap in DIV; else just numerator
    ExprPtr result = num;
    if (!denom_parts.empty()) {
        ExprPtr denom = denom_parts.front();
        for (size_t i = 1; i < denom_parts.size(); i++)
            denom = Expr::BinOpExpr(BinOp::MUL, denom, denom_parts[i]);
        result = Expr::BinOpExpr(BinOp::DIV, num, denom);
    }

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
        [](const auto& t) { return t.second; },
        // cppcheck-suppress constParameterReference
        [](auto& t) -> double& { return t.first; },
        [](auto& t) { t.first = 0; t.second = nullptr; });
}

inline void group_multiplicative(std::vector<std::pair<ExprPtr, double>>& factors) {
    group_like(factors,
        [](const auto& f) { return f.first; },
        // cppcheck-suppress constParameterReference
        [](auto& f) -> double& { return f.second; },
        [](auto& f) { f.second = 0; f.first = nullptr; });
}

// ============================================================================
//  Simplification assumptions (conditions generated by simplification)
// ============================================================================

// When simplify applies rules with conditions, it records assumptions.
// Call simplify_get_assumptions() after simplify() to retrieve them.
struct SimplifyAssumption {
    ExprPtr expr;       // the expression constrained (may be null for general conditions)
    std::string desc;   // human-readable: "x - 3 != 0", "x > 0"
    bool inherent = false;  // true = original was also undefined here (safe to always apply)
};

inline std::vector<SimplifyAssumption>& simplify_assumptions_() {
    static thread_local std::vector<SimplifyAssumption> assumptions;
    return assumptions;
}

inline void simplify_record_assumption(const ExprPtr& expr, const std::string& desc,
                                       bool inherent = false) {
    auto& a = simplify_assumptions_();
    for (auto& existing : a)
        if (existing.desc == desc) return;  // dedup by string
    a.push_back({expr, desc, inherent});
}

// Division cancellation: S/S → 1 is inherently safe (S was already undefined at S=0)
inline void simplify_assume_nonzero(const ExprPtr& expr, bool inherent = true) {
    if (is_num(expr)) return;
    simplify_record_assumption(expr, expr_to_string(expr) + " != 0", inherent);
}

inline std::vector<SimplifyAssumption> simplify_get_assumptions() {
    auto result = std::move(simplify_assumptions_());
    simplify_assumptions_().clear();
    return result;
}

inline void simplify_clear_assumptions() {
    simplify_assumptions_().clear();
}

// ============================================================================
//  Rewrite rules — thread-local access for simplifier
// ============================================================================

// Rewrite rule: pattern → replacement (e.g., cos(-x) → cos(x))
// Variables in pattern are wildcards that match any sub-expression.
struct RewriteRule {
    ExprPtr pattern;      // e.g., cos(Neg(Var("x")))
    ExprPtr replacement;  // e.g., cos(Var("x"))
    std::string desc;     // human-readable: "cos(-x) = cos(x)"
    // Condition template: pattern variables are substituted with bound expressions.
    // e.g., "x != 0" with binding x→(a+b) becomes "(a + b) != 0"
    std::string condition;
    bool is_undefined_branch = false;  // true when replacement is "undefined"
    int group_index = -1;              // index into rewrite_rule_groups_ (-1 = ungrouped)
};

inline const std::vector<RewriteRule>*& simplify_rewrite_rules_() {
    static thread_local const std::vector<RewriteRule>* rules = nullptr;
    return rules;
}

// Exhaustiveness flags indexed by group_index (parallel to rewrite_rule_groups_)
inline const std::vector<bool>*& simplify_rewrite_exhaustive_() {
    static thread_local const std::vector<bool>* flags = nullptr;
    return flags;
}

// Numeric bindings — the simplifier uses these to check rewrite rule conditions
inline const std::map<std::string, double>*& simplify_bindings_() {
    static thread_local const std::map<std::string, double>* b = nullptr;
    return b;
}

inline void simplify_set_rewrite_rules(const std::vector<RewriteRule>* rules,
                                        const std::vector<bool>* exhaustive = nullptr) {
    simplify_rewrite_rules_() = rules;
    simplify_rewrite_exhaustive_() = exhaustive;
}

inline const std::vector<RewriteRule>* simplify_get_rewrite_rules() {
    return simplify_rewrite_rules_();
}

// RAII guard: sets rewrite rules + exhaustiveness flags + bindings + custom functions
struct RewriteRulesGuard {
    explicit RewriteRulesGuard(const std::vector<RewriteRule>* rules,
                      const std::vector<bool>* exhaustive = nullptr,
                      const std::map<std::string, double>* bindings = nullptr,
                      const std::map<std::string, double(*)(double)>* custom_funcs = nullptr) {
        simplify_set_rewrite_rules(rules, exhaustive);
        simplify_bindings_() = bindings;
        custom_functions_ptr_() = custom_funcs;
    }
    ~RewriteRulesGuard() {
        simplify_set_rewrite_rules(nullptr, nullptr);
        simplify_bindings_() = nullptr;
        custom_functions_ptr_() = nullptr;
    }
    RewriteRulesGuard(const RewriteRulesGuard&) = delete;
    RewriteRulesGuard& operator=(const RewriteRulesGuard&) = delete;
};

// ---- Simplify: per-operator helpers ----

inline ExprPtr simplify_additive(const ExprPtr& combined) {
    std::vector<std::pair<double, ExprPtr>> terms;
    flatten_additive(combined, 1.0, terms);
    double constant = 0;
    std::vector<std::pair<double, ExprPtr>> symbolic;
    // Rational accumulator: combine integers and structural fractions
    int64_t rat_num = 0, rat_den = 1;
    bool has_rational = false;
    for (auto& [c, b] : terms) {
        if (!b) {
            // Pure constant — try to add as rational
            if (is_integer_value(c)) {
                int64_t n = static_cast<int64_t>(c);
                rat_num = rat_num * 1 + n * rat_den; // rat += n/1
                // (no GCD yet — normalize at end)
                has_rational = true;
            } else {
                constant += c;
            }
        } else if (is_int_frac(b) && is_integer_value(c)) {
            // Structural fraction with integer coefficient: c * (p/q)
            auto [p, q] = to_rational(b);
            int64_t ic = static_cast<int64_t>(c);
            rat_num = rat_num * q + ic * p * rat_den;
            rat_den *= q;
            // Prevent overflow by intermediate GCD
            int64_t g = gcd_abs(rat_num, rat_den);
            if (g > 1) { rat_num /= g; rat_den /= g; }
            has_rational = true;
        } else {
            symbolic.push_back({c, b});
        }
    }
    group_additive(symbolic);
    // Emit rational accumulator
    if (has_rational && (rat_num != 0 || symbolic.empty())) {
        if (rat_den == 1 || rat_num == 0) {
            // Integer or zero — add as floating constant
            constant += static_cast<double>(rat_num);
        } else {
            symbolic.push_back({1.0, make_rational(rat_num, rat_den)});
        }
    }
    if (std::abs(constant) >= EPSILON_ZERO)
        symbolic.push_back({constant, nullptr});
    return rebuild_additive(symbolic);
}

inline ExprPtr simplify_div(const ExprPtr& l, const ExprPtr& r); // forward decl

inline ExprPtr simplify_mul(const ExprPtr& l, const ExprPtr& r) {
    if (is_zero(l) || is_zero(r)) return Expr::Num(0);
    // Rational * Rational: exact arithmetic
    auto [ln, ld] = to_rational(l);
    auto [rn, rd] = to_rational(r);
    if (ld != 0 && rd != 0)
        return make_rational(ln * rn, ld * rd);
    // Rational * symbolic: emit as (n * sym) / d
    if (ld != 0 && ld != 1 && rd == 0) {
        // (ln/ld) * r → simplify_mul(Num(ln), r) then wrap with /ld
        auto top = simplify_mul(Expr::Num(static_cast<double>(ln)), r);
        return simplify_div(top, Expr::Num(static_cast<double>(ld)));
    }
    if (rd != 0 && rd != 1 && ld == 0) {
        auto top = simplify_mul(l, Expr::Num(static_cast<double>(rn)));
        return simplify_div(top, Expr::Num(static_cast<double>(rd)));
    }
    auto combined = Expr::BinOpExpr(BinOp::MUL, l, r);
    double coeff = 1.0;
    std::vector<std::pair<ExprPtr, double>> factors;
    flatten_multiplicative(combined, coeff, factors);
    group_multiplicative(factors);
    return rebuild_multiplicative(coeff, factors);
}

inline ExprPtr simplify_div(const ExprPtr& l, const ExprPtr& r) {
    if (is_zero(l) && !is_zero(r)) return Expr::Num(0);
    // a / 0 is undefined — keep structural DIV so later evaluate() yields
    // empty Checked via the NaN sentinel. Do NOT fold to 0 or NaN here.
    // This also covers 0/0: both operands preserved symbolically.
    if (is_zero(r)) return Expr::BinOpExpr(BinOp::DIV, l, r);
    if (is_one(r)) return l;
    if (is_neg_one(r)) return Expr::Neg(l);
    // Rational division: (a/b) / (c/d) = (a*d) / (b*c)
    {
        auto [ln, ld] = to_rational(l);
        auto [rn, rd] = to_rational(r);
        if (ld != 0 && rd != 0 && rn != 0)
            return make_rational(ln * rd, ld * rn);
    }
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
        if (is_num(l->right)) {
            if (is_integer_value(l->right->num) && is_integer_value(r->num)) {
                auto rat = make_rational(
                    static_cast<int64_t>(l->right->num), static_cast<int64_t>(r->num));
                // Don't recurse into simplify_mul — just emit MUL(sym, fraction)
                if (is_one(rat)) return l->left;
                return Expr::BinOpExpr(BinOp::MUL, l->left, rat);
            }
            return Expr::BinOpExpr(BinOp::MUL, l->left, Expr::Num(l->right->num / r->num));
        }
        if (is_num(l->left)) {
            if (is_integer_value(l->left->num) && is_integer_value(r->num)) {
                auto rat = make_rational(
                    static_cast<int64_t>(l->left->num), static_cast<int64_t>(r->num));
                if (is_one(rat)) return l->right;
                return Expr::BinOpExpr(BinOp::MUL, rat, l->right);
            }
            return Expr::BinOpExpr(BinOp::MUL, Expr::Num(l->left->num / r->num), l->right);
        }
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
                // Context-aware: don't cancel if the term is known to be zero
                bool is_zero_term = false;
                if (auto* bindings = simplify_bindings_()) {
                    auto resolved = lb;
                    for (auto& [var, val] : *bindings)
                        resolved = substitute(resolved, var, Expr::Num(val));
                    if (auto v = evaluate(*resolved)) {
                        if (std::abs(v.value()) < EPSILON_ZERO) is_zero_term = true;
                    }
                }
                if (is_zero_term) continue;  // skip: would be 0/0
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

// Distribute division over addition for derive-output simplification only:
//   (a + b) / k   →   a/k + b/k
//   (a - b) / k   →   a/k - b/k
// Applies only when k is a numeric literal (is_num). For symbolic k, distributing
// bloats expressions unnecessarily and would churn the general simplifier — that
// is why this is NOT called from simplify_div. Intended call site: format_derived
// (system.h), once, before re-simplification and string formatting. The subsequent
// simplify() call collapses like-terms across the now-visible individual quotients
// (e.g., -b/2 + b/2 → 0), which is the whole point of the pass.
inline ExprPtr distribute_over_sum(const ExprPtr& e) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return e;
        case ExprType::UNARY_NEG: {
            auto c = distribute_over_sum(e->child);
            return c == e->child ? e : Expr::Neg(c);
        }
        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> new_args;
            new_args.reserve(e->args.size());
            bool changed = false;
            for (auto& a : e->args) {
                auto na = distribute_over_sum(a);
                if (na != a) changed = true;
                new_args.push_back(na);
            }
            return changed ? Expr::Call(e->name, new_args) : e;
        }
        case ExprType::BINOP: {
            auto l = distribute_over_sum(e->left);
            auto r = distribute_over_sum(e->right);
            if (e->op == BinOp::DIV && is_num(r) && !is_zero(r)
                && l->type == ExprType::BINOP && is_additive(l->op)) {
                // (A ± B) / k   →   A/k ± B/k
                // Recurse on the new quotients so nested additive chains split
                // all the way down — e.g. ((a + b) + c) / k splits fully to
                // a/k + b/k + c/k rather than stopping at (a + b)/k + c/k.
                auto new_left  = distribute_over_sum(Expr::BinOpExpr(BinOp::DIV, l->left, r));
                auto new_right = distribute_over_sum(Expr::BinOpExpr(BinOp::DIV, l->right, r));
                return Expr::BinOpExpr(l->op, new_left, new_right);
            }
            if (l == e->left && r == e->right) return e;
            return Expr::BinOpExpr(e->op, l, r);
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

// ---- Simplify: main entry ----

inline ExprPtr simplify_once(const ExprPtr& e);  // forward decl — impl calls wrapper recursively

inline ExprPtr simplify_once_impl(const ExprPtr& e) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return e;

        case ExprType::UNARY_NEG: {
            auto c = simplify_once(e->child);
            if (is_undefined(c)) return c;
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
                if (is_undefined(sa.back())) return sa.back();  // propagate
                if (!is_num(sa.back())) all_num = false;
            }
            auto s = Expr::Call(e->name, sa);
            if (all_num && lookup_function(e->name)) return evaluate_symbolic(*s);

            // Function-specific rules migrated to BUILTIN_REWRITE_RULES
            return s;
        }

        case ExprType::BINOP: {
            auto l = simplify_once(e->left);
            auto r = simplify_once(e->right);
            if (is_undefined(l) || is_undefined(r)) return Expr::Var("undefined");
            if (is_num(l) && is_num(r))
                return evaluate_symbolic(*Expr::BinOpExpr(e->op, l, r));

            switch (e->op) {
                case BinOp::ADD: case BinOp::SUB:
                    return simplify_additive(Expr::BinOpExpr(e->op, l, r));
                case BinOp::MUL: return simplify_mul(l, r);
                case BinOp::DIV: return simplify_div(l, r);
                case BinOp::POW:
                    // Rational base ^ integer exponent: (a/b)^n = a^n / b^n
                    if (is_int_frac(l) && is_num(r) && is_integer_value(r->num)
                        && r->num > 0 && r->num <= RATIONAL_POW_MAX_EXP) {
                        auto [n, d] = to_rational(l);
                        int64_t exp = static_cast<int64_t>(r->num);
                        int64_t rn = 1, rd = 1;
                        for (int64_t i = 0; i < exp; i++) { rn *= n; rd *= d; }
                        return make_rational(rn, rd);
                    }
                    // x^(-n) → 1/x^n — stays in C++ (needs numeric check)
                    if (is_num(r) && r->num < 0) {
                        if (r->num == -1.0)
                            return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), l);
                        return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1),
                            Expr::BinOpExpr(BinOp::POW, l, Expr::Num(-r->num)));
                    }
                    // x^0, x^1, x^0.5, (x^a)^b now in BUILTIN_REWRITE_RULES
                    return Expr::BinOpExpr(BinOp::POW, l, r);
        case BinOp::COUNT_: assert(false && "invalid BinOp"); break;
            }
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

// Apply user-defined rewrite rules to a simplified expression
// Substitute pattern variable names in a condition string with bound expressions
inline std::string substitute_condition(const std::string& cond,
        const std::map<std::string, ExprPtr>& bindings) {
    std::string result = cond;
    // Replace longest variable names first to avoid partial matches
    std::vector<std::pair<std::string, std::string>> replacements;
    replacements.reserve(bindings.size());
    for (auto& [var, expr] : bindings)
        replacements.push_back({var, expr_to_string(expr)});
    std::sort(replacements.begin(), replacements.end(),
        [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    for (auto& [var, str] : replacements) {
        size_t pos = 0;
        while ((pos = result.find(var, pos)) != std::string::npos) {
            // Only replace whole identifiers (not substrings of longer names)
            bool before_ok = (pos == 0 || !std::isalnum(result[pos-1]));
            bool after_ok = (pos + var.size() >= result.size()
                || !std::isalnum(result[pos + var.size()]));
            if (before_ok && after_ok) {
                result.replace(pos, var.size(), str);
                pos += str.size();
            } else {
                pos += var.size();
            }
        }
    }
    return result;
}

// Check if a rewrite rule condition is violated by numeric bindings.
// Returns true if the condition is definitely false (should skip rule).
// Returns false if condition holds or can't be determined (should apply rule).
// Works by substituting the condition string and checking simple patterns.
inline bool condition_violated(const std::string& cond,
        const std::map<std::string, ExprPtr>& bindings) {
    // Try to resolve each bound expression to a number
    // Use both the expression itself and the global bindings
    auto* global_bindings = simplify_bindings_();
    std::map<std::string, double> numeric;
    for (auto& [var, expr] : bindings) {
        if (is_num(expr)) {
            numeric[var] = expr->num;
        } else if (is_var(expr) && global_bindings) {
            auto it = global_bindings->find(expr->name);
            if (it != global_bindings->end())
                numeric[var] = it->second;
            // else: symbolic, can't resolve this variable
        }
        // else: complex symbolic expression, can't check
    }

    // Check simple clause: "VAR CMP VALUE"
    auto check_clause = [&](const std::string& clause) -> int {
        // Returns: 1 = true, 0 = false, -1 = can't determine
        // Find the comparison operator
        struct { const char* str; size_t len; } ops[] = {
            {"!=", 2}, {">=", 2}, {"<=", 2}, {"==", 2}, {">", 1}, {"<", 1}, {"=", 1}
        };
        for (auto& [opstr, oplen] : ops) {
            auto p = clause.find(opstr);
            if (p == std::string::npos) continue;
            // Skip if this = is part of a longer operator
            if (oplen == 1 && opstr[0] == '=' && p > 0
                && (clause[p-1] == '!' || clause[p-1] == '>' || clause[p-1] == '<'))
                continue;
            if (oplen == 1 && opstr[0] == '=' && p + 1 < clause.size() && clause[p+1] == '=')
                continue;
            if (oplen == 1 && opstr[0] == '>' && p + 1 < clause.size() && clause[p+1] == '=')
                continue;
            if (oplen == 1 && opstr[0] == '<' && p + 1 < clause.size() && clause[p+1] == '=')
                continue;

            // Extract LHS and RHS variable/value
            auto lhs_s = clause.substr(0, p);
            auto rhs_s = clause.substr(p + oplen);
            while (!lhs_s.empty() && lhs_s.back() == ' ') lhs_s.pop_back();
            while (!lhs_s.empty() && lhs_s.front() == ' ') lhs_s.erase(lhs_s.begin());
            while (!rhs_s.empty() && rhs_s.back() == ' ') rhs_s.pop_back();
            while (!rhs_s.empty() && rhs_s.front() == ' ') rhs_s.erase(rhs_s.begin());

            // Resolve to numbers
            auto resolve = [&](const std::string& s) -> std::optional<double> {
                auto it = numeric.find(s);
                if (it != numeric.end()) return it->second;
                // NOLINTNEXTLINE(bugprone-empty-catch) — std::stod throws on non-numeric input; fall back to nullopt
                try { return std::stod(s); } catch (const std::invalid_argument&) {} catch (const std::out_of_range&) {}
                return std::nullopt;
            };
            auto lval = resolve(lhs_s);
            auto rval = resolve(rhs_s);
            if (!lval || !rval) return -1;  // can't determine

            std::string op(opstr, oplen);
            if (op == "!=") return std::abs(*lval - *rval) > EPSILON_ZERO ? 1 : 0;
            if (op == "=" || op == "==") return std::abs(*lval - *rval) <= EPSILON_ZERO ? 1 : 0;
            if (op == ">") return *lval > *rval + EPSILON_ZERO ? 1 : 0;
            if (op == ">=") return *lval >= *rval - EPSILON_ZERO ? 1 : 0;
            if (op == "<") return *lval < *rval - EPSILON_ZERO ? 1 : 0;
            if (op == "<=") return *lval <= *rval + EPSILON_ZERO ? 1 : 0;
        }
        return -1;
    };

    // Split by && and check each clause
    std::string remaining = cond;
    while (!remaining.empty()) {
        auto pos = remaining.find("&&");
        std::string clause = (pos != std::string::npos)
            ? remaining.substr(0, pos) : remaining;
        remaining = (pos != std::string::npos) ? remaining.substr(pos + 2) : "";
        int result = check_clause(clause);
        if (result == 0) return true;   // clause is false → condition violated
    }
    return false;  // all clauses passed or undetermined
}

inline ExprPtr apply_rewrite_rules(const ExprPtr& e) {
    auto* rules = simplify_get_rewrite_rules();
    if (!rules) return e;
    auto* exhaustive_flags = simplify_rewrite_exhaustive_();
    for (auto& rule : *rules) {
        if (rule.is_undefined_branch) continue;  // skip: exists for exhaustiveness only
        auto bindings = match_pattern(rule.pattern, e);
        if (!bindings) continue;
        if (!rule.condition.empty()) {
            // Context-aware: if bound values are numeric, check condition
            if (condition_violated(rule.condition, *bindings)) continue;
            bool inherent = exhaustive_flags && rule.group_index >= 0
                && static_cast<size_t>(rule.group_index) < exhaustive_flags->size()
                && (*exhaustive_flags)[rule.group_index];
            simplify_record_assumption(nullptr,
                substitute_condition(rule.condition, *bindings), inherent);
        }
        return apply_rewrite(rule.replacement, *bindings);
    }
    return e;
}

inline ExprPtr simplify_once(const ExprPtr& e) {
    return apply_rewrite_rules(simplify_once_impl(e));
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

// Function inverter: given f(inner) = rhs, produce inner = f⁻¹(rhs).
// Returns the inverted RHS expression, or nullptr if no inverse is known.
// Set by FormulaSystem to resolve via .fw sub-system definitions.
using FuncInverter = std::function<ExprPtr(const std::string& func_name, const ExprPtr& rhs)>;

inline FuncInverter& solve_func_inverter_() {
    static thread_local FuncInverter inverter;
    return inverter;
}

inline void solve_set_func_inverter(FuncInverter fn) {
    solve_func_inverter_() = std::move(fn);
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
        const auto& inverter = solve_func_inverter_();
        if (inverter) {
            auto new_rhs = inverter(lhs->name, rhs);
            if (new_rhs) return recurse(lhs->args[0], new_rhs);
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


// Expand products a*(b+c) -> a*b+a*c when target variable spans both factors.
// Enables quadratic decomposition for substituted expressions like w*(p-2w).
inline ExprPtr expand_for_var(const ExprPtr& e, const std::string& var) {
    if (!e || !contains_var(e, var)) return e;
    if (e->type == ExprType::BINOP) {
        auto l = expand_for_var(e->left, var);
        auto r = expand_for_var(e->right, var);
        if (e->op == BinOp::MUL) {
            bool l_sum = l->type == ExprType::BINOP &&
                (l->op == BinOp::ADD || l->op == BinOp::SUB);
            bool r_sum = r->type == ExprType::BINOP &&
                (r->op == BinOp::ADD || r->op == BinOp::SUB);
            if (r_sum && contains_var(l, var) && contains_var(r, var)) {
                // a * (b +/- c) -> a*b +/- a*c
                auto op = r->op;
                return simplify(Expr::BinOpExpr(op,
                    expand_for_var(Expr::BinOpExpr(BinOp::MUL, l, r->left), var),
                    expand_for_var(Expr::BinOpExpr(BinOp::MUL, l, r->right), var)));
            }
            if (l_sum && contains_var(l, var) && contains_var(r, var)) {
                auto op = l->op;
                return simplify(Expr::BinOpExpr(op,
                    expand_for_var(Expr::BinOpExpr(BinOp::MUL, l->left, r), var),
                    expand_for_var(Expr::BinOpExpr(BinOp::MUL, l->right, r), var)));
            }
        }
        if (l != e->left || r != e->right)
            return Expr::BinOpExpr(e->op, l, r);
    }
    if (e->type == ExprType::UNARY_NEG) {
        auto inner = expand_for_var(e->child, var);
        return inner != e->child ? Expr::Neg(inner) : e;
    }
    return e;
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

    // Try recursive inversion first (handles x^2=y, sin(x)=y, etc. cleanly)
    {
        std::vector<Solution> inv_results;
        if (contains_var(lhs, target) && !contains_var(rhs, target))
            inv_results = solve_by_inversion(lhs, rhs, target);
        else if (contains_var(rhs, target) && !contains_var(lhs, target))
            inv_results = solve_by_inversion(rhs, lhs, target);
        if (!inv_results.empty()) return inv_results;
    }

    // Expand products involving target to enable quadratic decomposition
    combined = simplify(expand_for_var(combined, target));

    // Try quadratic decomposition: ax² + bx + c = 0
    // Flatten into additive terms, classify each by degree in target variable
    if (contains_var(combined, target)) {
        std::vector<std::pair<double, ExprPtr>> terms;
        flatten_additive(combined, 1.0, terms);
        ExprPtr a_expr = Expr::Num(0), b_expr = Expr::Num(0), c_expr = Expr::Num(0);
        bool is_quadratic = false;
        bool too_complex = false;

        for (auto& [coeff, base] : terms) {
            if (!base || !contains_var(base, target)) {
                // Constant term (no target variable)
                c_expr = simplify(Expr::BinOpExpr(BinOp::ADD, c_expr,
                    base ? Expr::BinOpExpr(BinOp::MUL, Expr::Num(coeff), base)
                         : Expr::Num(coeff)));
            } else {
                // Contains target — classify by degree
                // Check for target^2 or target*target patterns
                double mc = 1.0;
                std::vector<std::pair<ExprPtr, double>> factors;
                flatten_multiplicative(base, mc, factors);
                mc *= coeff;

                double target_degree = 0;
                ExprPtr non_target = nullptr;
                bool valid = true;
                for (auto& [fb, fe] : factors) {
                    if (is_var(fb) && fb->name == target) {
                        target_degree += fe;
                    } else if (contains_var(fb, target)) {
                        valid = false; break;  // target inside function/power base
                    } else {
                        auto f = (std::abs(fe - 1.0) < EPSILON_ZERO) ? fb
                            : Expr::BinOpExpr(BinOp::POW, fb, Expr::Num(fe));
                        non_target = non_target
                            ? Expr::BinOpExpr(BinOp::MUL, non_target, f) : f;
                    }
                }
                if (!valid) { too_complex = true; break; }

                auto term_coeff = non_target
                    ? simplify(Expr::BinOpExpr(BinOp::MUL, Expr::Num(mc), non_target))
                    : Expr::Num(mc);

                if (std::abs(target_degree - 2.0) < EPSILON_ZERO) {
                    a_expr = simplify(Expr::BinOpExpr(BinOp::ADD, a_expr, term_coeff));
                    is_quadratic = true;
                } else if (std::abs(target_degree - 1.0) < EPSILON_ZERO) {
                    b_expr = simplify(Expr::BinOpExpr(BinOp::ADD, b_expr, term_coeff));
                } else if (std::abs(target_degree) < EPSILON_ZERO) {
                    c_expr = simplify(Expr::BinOpExpr(BinOp::ADD, c_expr, term_coeff));
                } else {
                    too_complex = true; break;  // cubic or fractional degree
                }
            }
        }

        if (is_quadratic && !too_complex) {
            auto a = simplify(a_expr);
            auto b = simplify(b_expr);
            auto c = simplify(c_expr);
            // Verify a != 0
            if (!(is_num(a) && std::abs(a->num) < EPSILON_ZERO)) {
                // discriminant: b² - 4ac
                auto disc = simplify(Expr::BinOpExpr(BinOp::SUB,
                    Expr::BinOpExpr(BinOp::POW, b, Expr::Num(2)),
                    Expr::BinOpExpr(BinOp::MUL, Expr::Num(4),
                        Expr::BinOpExpr(BinOp::MUL, a, c))));
                auto neg_b = simplify(Expr::Neg(b));
                auto two_a = simplify(Expr::BinOpExpr(BinOp::MUL, Expr::Num(2), a));
                auto sqrt_disc = Expr::Call("sqrt", {disc});

                auto sol1 = simplify(Expr::BinOpExpr(BinOp::DIV,
                    Expr::BinOpExpr(BinOp::ADD, neg_b, sqrt_disc), two_a));
                auto sol2 = simplify(Expr::BinOpExpr(BinOp::DIV,
                    Expr::BinOpExpr(BinOp::SUB, neg_b, sqrt_disc), two_a));

                std::string cond = expr_to_string(disc) + " >= 0";
                std::vector<Solution> results;
                results.push_back({sol1, disc, cond});
                if (!expr_equal(sol1, sol2))
                    results.push_back({sol2, disc, cond});
                return results;
            }
        }
    }

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
        // Note: fhi is not tracked inside the loop — only flo participates in the sign test (flo * fmid).
        if (flo * fmid < 0) { hi = mid; }
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
        // Coarse pass with deterministic jitter — reproducibility is intentional:
        // users must see the same numeric probe sequence every run for the solver output to be deterministic.
        // NOLINTNEXTLINE(bugprone-random-generator-seed)
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
