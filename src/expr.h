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

    [[nodiscard]] bool has_value()         const noexcept { return !std::isnan(val_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    // Value access. Asserts in debug on empty; UB in release
    // (assert-postcondition style, matching std::optional::operator*).
    [[nodiscard]] T value() const noexcept {
        assert(has_value() && "Checked<T>::value() called on empty");
        return val_;
    }

    // Boundary escape: hands off the raw T (quiet_NaN when empty) to code
    // outside the Checked ecosystem — specifically the numerical root-finder
    // library (find_numeric_roots, adaptive_scan, newton_solve, bisection_solve),
    // which is a pure-double algorithm layer with its own isfinite discipline.
    // Using this operator IS an explicit, reviewable statement of intent.
    [[nodiscard]] T value_or_nan() const noexcept { return val_; }
};

static_assert(sizeof(Checked<double>) == sizeof(double),
    "Checked<double> must be the same size as double — no hidden bool padding");

// ============================================================================
//  Formatting (needed by ValueSet::to_string)
// ============================================================================

[[nodiscard]] inline std::string fmt_num(double v) {
    if (std::abs(v) < 1e12 && v == static_cast<double>(static_cast<long long>(v)))
        return std::to_string(static_cast<long long>(v));
    std::ostringstream os;
    os << std::setprecision(10) << v;
    return os.str();
}

// Generic separator-join over any range. Replaces the verbose `bool first`
// flag pattern at multiple print sites (ValueSet::to_string, expr_to_string).
template<class Range, class Sep, class Fn>
[[nodiscard]] inline std::string join_with_sep(const Range& r, const Sep& sep, Fn fn) {
    std::string s; bool first = true;
    for (const auto& x : r) { if (!first) s += sep; s += fn(x); first = false; }
    return s;
}

// ============================================================
// Section: Set theory (ValueSet, Interval, PeriodicFamily, Condition)
// ============================================================

// ============================================================================
//  ValueSet — unified representation for conditions, ranges, and solutions
// ============================================================================

struct Interval {
    double low = 0, high = 0;
    bool low_inclusive = false, high_inclusive = false;

    [[nodiscard]] bool contains(double v) const {
        const bool above = low_inclusive ? (v >= low) : (v > low);
        const bool below = high_inclusive ? (v <= high) : (v < high);
        return above && below;
    }

    [[nodiscard]] bool empty() const {
        return (low > high) || (low == high && !(low_inclusive && high_inclusive));
    }
};

// Forward decls for ValueSet::periodic_ field (Expr / ExprPtr defined below).
struct Expr;
using ExprPtr = Expr*;

// PeriodicFamily — one branch of a trig solution set. `base` is the
// numeric principal-branch root (hot-path containment via fmod). `period`
// is the symbolic period (e.g. 2*pi for sin/cos, pi for tan), kept as
// ExprPtr so render emits `pi` not `3.1415...`. evaluate(*period) projects
// to a numeric for membership / dedup checks.
struct PeriodicFamily {
    double base;
    ExprPtr period;
};

// Forward decls used by ValueSet member functions defined inline below
// (contains() projects period to numeric; to_string() renders period).
[[nodiscard]] inline Checked<double> evaluate(const Expr& e);
inline std::string expr_to_string(const Expr& e);
// fmt_exact_double lives in fit.h (depends on expr_recognize_constants).
// to_string() calls it for periodic-family base rendering so `pi / 6` is
// recognized; falls back to fmt_num when no arena is active. The decl
// here is intentionally NOT marked `inline` even though the fit.h
// definition is — adding `inline` to the forward decl trips clang's
// -Wundefined-inline at the use site below (definition not yet visible
// when expr.h is parsed). C++ permits this asymmetry: an inline function
// can be declared without `inline` as long as the definition has it.
[[nodiscard]] std::string fmt_exact_double(double v,
        const std::map<std::string, double>& extra_constants);

class ValueSet {
    std::vector<Interval> intervals_;
    std::vector<double> discrete_;
    std::vector<PeriodicFamily> periodic_;

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

    // Periodic family factory — used by resolve_all when the equation is a
    // single FUNC_CALL of a periodic builtin (sin, cos, tan). Each entry in
    // `fams` is one principal-cycle root + its symbolic period.
    static ValueSet periodic(std::vector<PeriodicFamily> fams) {
        ValueSet s;
        s.periodic_ = std::move(fams);
        return s;
    }

    // Queries
    [[nodiscard]] bool empty() const {
        return intervals_.empty() && discrete_.empty() && periodic_.empty();
    }

    [[nodiscard]] bool contains(double v) const {
        if (std::any_of(intervals_.begin(), intervals_.end(),
                [v](const Interval& iv) { return iv.contains(v); })) return true;
        if (std::any_of(discrete_.begin(), discrete_.end(),
            [v](double d) { return std::abs(d - v) < EPSILON_ZERO; })) return true;
        // Periodic membership: project period to numeric, normalize the
        // residue (v - base) mod period to [-period/2, period/2), test |rem|
        // against EPSILON_ZERO * period. Skip families whose period does not
        // project finite (defensive — every in-tree period is 2*pi or pi).
        return std::any_of(periodic_.begin(), periodic_.end(),
            [v](const PeriodicFamily& pf) {
                const double p = evaluate(*pf.period).value_or_nan();
                if (!std::isfinite(p) || p <= 0) return false;
                double rem = std::fmod(v - pf.base, p);
                if (rem >= p / 2.0) rem -= p;
                else if (rem < -p / 2.0) rem += p;
                return std::abs(rem) < EPSILON_ZERO * p;
            });
    }

    [[nodiscard]] const std::vector<Interval>& intervals() const { return intervals_; }
    [[nodiscard]] const std::vector<double>& discrete() const { return discrete_; }
    [[nodiscard]] const std::vector<PeriodicFamily>& periodic() const { return periodic_; }
    [[nodiscard]] bool has_periodic() const { return !periodic_.empty(); }

    // Set operations
    [[nodiscard]] ValueSet intersect(const ValueSet& other) const {
        ValueSet result;

        // Interval ∩ Interval
        for (const auto& a : intervals_)
            for (const auto& b : other.intervals_) {
                const double lo = std::max(a.low, b.low);
                const double hi = std::min(a.high, b.high);
                const bool lo_inc = (a.low == b.low) ? (a.low_inclusive && b.low_inclusive)
                            : (lo == a.low) ? a.low_inclusive : b.low_inclusive;
                const bool hi_inc = (a.high == b.high) ? (a.high_inclusive && b.high_inclusive)
                            : (hi == a.high) ? a.high_inclusive : b.high_inclusive;
                const Interval iv{lo, hi, lo_inc, hi_inc};
                if (!iv.empty()) result.intervals_.push_back(iv);
            }

        // Discrete points: keep only those in both sets
        for (const auto& d : discrete_)
            if (other.contains(d)) result.discrete_.push_back(d);
        for (const auto& d : other.discrete_)
            if (this->contains(d)) {
                // Avoid duplicates
                const bool dup = std::any_of(result.discrete_.begin(), result.discrete_.end(),
                    [d](double rd) { return std::abs(rd - d) < EPSILON_ZERO; });
                if (!dup) result.discrete_.push_back(d);
            }

        return result;
    }

    [[nodiscard]] ValueSet unite(const ValueSet& other) const {
        ValueSet result;
        result.intervals_ = intervals_;
        result.intervals_.insert(result.intervals_.end(),
            other.intervals_.begin(), other.intervals_.end());
        result.discrete_ = discrete_;
        for (const auto& d : other.discrete_) {
            const bool dup = std::any_of(result.discrete_.begin(), result.discrete_.end(),
                [d](double rd) { return std::abs(rd - d) < EPSILON_ZERO; });
            if (!dup) result.discrete_.push_back(d);
        }
        return result;
    }

    // Filter a list of values through this set
    [[nodiscard]] std::vector<double> filter(const std::vector<double>& values) const {
        std::vector<double> result;
        std::copy_if(values.begin(), values.end(), std::back_inserter(result),
            [this](double v) { return contains(v); });
        return result;
    }

    // Is this a purely discrete set (no intervals, no periodic families)?
    [[nodiscard]] bool is_discrete() const {
        return intervals_.empty() && periodic_.empty();
    }

    // Does this set cover all real numbers (-inf, +inf)?
    // Checks if intervals + discrete points leave no gaps.
    [[nodiscard]] bool covers_reals() const {
        if (intervals_.empty() && discrete_.empty()) return false;
        // Merge all coverage into sorted intervals (discrete points become [v,v])
        std::vector<Interval> all_intervals = intervals_;
        std::transform(discrete_.begin(), discrete_.end(), std::back_inserter(all_intervals),
            [](double d) { return Interval{d, d, true, true}; });
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

    // Periodic families (Future #12). Render each dedup'd family as
    // `<base> + k * <period>  # k in Z`. base recognised via fmt_exact_double
    // (so `pi / 6` surfaces); period is symbolic from inception. Render-time
    // dedup collapses {b1,p1} ≡ {b2,p2} when periods agree numerically AND
    // (b1-b2) lies on the integer-multiple-of-p1 lattice. Pairwise; small N
    // (typically 1-2 families per equation). 12h: extracted from to_string()
    // so main.cpp dispatch can emit per-family `<alias> = <line>` shapes
    // without re-implementing dedup.
    [[nodiscard]] std::vector<std::string> periodic_render_lines() const {
        std::vector<std::string> lines;
        if (periodic_.empty()) return lines;
        std::vector<PeriodicFamily> kept;
        kept.reserve(periodic_.size());
        for (const auto& pf : periodic_) {
            const double p_new = evaluate(*pf.period).value_or_nan();
            bool dup = false;
            if (std::isfinite(p_new) && p_new > 0) {
                for (const auto& k : kept) {
                    const double p_old = evaluate(*k.period).value_or_nan();
                    if (!std::isfinite(p_old) || p_old <= 0) continue;
                    if (std::abs(p_new - p_old) >= EPSILON_ZERO * std::max(p_new, p_old))
                        continue;
                    double rem = std::fmod(pf.base - k.base, p_old);
                    if (rem >= p_old / 2.0) rem -= p_old;
                    else if (rem < -p_old / 2.0) rem += p_old;
                    if (std::abs(rem) < EPSILON_ZERO * p_old) { dup = true; break; }
                }
            }
            if (!dup) kept.push_back(pf);
        }
        // fmt_exact_double allocates Expr nodes via the arena to recognise
        // pi/6 etc. Every in-tree call site executes under an active
        // ExprArena::Scope (see main.cpp:257 solve_fmt_scope; tests use the
        // global test_arena scope).
        lines.reserve(kept.size());
        for (const auto& pf : kept) {
            const std::string base_str   = fmt_exact_double(pf.base, {});
            const std::string period_str = expr_to_string(*pf.period);
            // 12e: trailing annotation uses fwiz comment syntax (`#`)
            // so the line body is a valid fwiz expression — `x = <body>`
            // round-trips through load_string without a parse error.
            lines.push_back(base_str + " + k * " + period_str + "  # k in Z");
        }
        return lines;
    }

    // String representation
    [[nodiscard]] std::string to_string() const {
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
            const std::string s = "{" + join_with_sep(discrete_, ", ",
                [](double d) { return fmt_num(d); }) + "}";
            parts.push_back(s);
        }

        // Periodic families: delegate dedup-and-render to periodic_render_lines()
        // (12h refactor). Each rendered line goes in as one " | "-joinable part.
        if (!periodic_.empty()) {
            auto plines = periodic_render_lines();
            parts.insert(parts.end(),
                std::make_move_iterator(plines.begin()),
                std::make_move_iterator(plines.end()));
        }

        if (parts.size() == 1) return parts[0];
        return join_with_sep(parts, " | ", [](const std::string& p) { return p; });
    }
};

// ============================================================
// Section: AST definition (Expr struct, ExprType, BinOp, factories)
// ============================================================

// ============================================================================
//  Expression arena (contiguous allocation for cache locality)
// ============================================================================

enum class ExprType : uint8_t { NUM, VAR, BINOP, UNARY_NEG, FUNC_CALL, COUNT_ };
enum class BinOp   : uint8_t { ADD, SUB, MUL, DIV, POW, COUNT_ };

static_assert(static_cast<int>(ExprType::COUNT_) == 5, "ExprType has 5 real values");
static_assert(static_cast<int>(BinOp::COUNT_) == 5, "BinOp has 5 real values");
static_assert(static_cast<int>(BinOp::ADD) == 0, "BinOp values start at 0 (used as array index)");

// `struct Expr;` and `using ExprPtr = Expr*;` already forward-declared
// earlier in the file (above PeriodicFamily / ValueSet). Forward decls
// are idempotent, so re-declaring here is harmless, but keeping a single
// declaration site is cleaner — see the pair near the top of this file.

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
    [[nodiscard]] size_t size() const { return chunks.empty() ? 0 : (chunks.size()-1) * CHUNK_SIZE + next_in_chunk; }
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

[[nodiscard]] inline ExprPtr Expr::Num(double v) {
    assert(ExprArena::current() && "ExprArena::Scope must be active");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::NUM; e->num = v; return e;
}
[[nodiscard]] inline ExprPtr Expr::Var(const std::string& n) {
    assert(ExprArena::current() && "ExprArena::Scope must be active");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::VAR; e->name = n; return e;
}
[[nodiscard]] inline ExprPtr Expr::BinOpExpr(BinOp o, ExprPtr l, ExprPtr r) {
    assert(l && r && "BinOp operands must not be null");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::BINOP; e->op = o; e->left = l; e->right = r; return e;
}
[[nodiscard]] inline ExprPtr Expr::Neg(ExprPtr c) {
    assert(c && "Neg operand must not be null");
    auto e = ExprArena::current()->alloc(); e->type = ExprType::UNARY_NEG; e->child = c; return e;
}
[[nodiscard]] inline ExprPtr Expr::Call(const std::string& n, std::vector<ExprPtr> a) {
    auto e = ExprArena::current()->alloc(); e->type = ExprType::FUNC_CALL; e->name = n; e->args = std::move(a); return e;
}

// ============================================================================
//  Type predicates
// ============================================================================

// Reference versions (no null check needed)
[[nodiscard]] constexpr bool is_num(const Expr& e)     { return e.type == ExprType::NUM; }
[[nodiscard]] constexpr bool is_var(const Expr& e)     { return e.type == ExprType::VAR; }
[[nodiscard]] constexpr bool is_atomic(const Expr& e)  { return is_num(e) || is_var(e); }
[[nodiscard]] constexpr bool is_zero(const Expr& e)    { return is_num(e) && e.num == 0; }
[[nodiscard]] constexpr bool is_one(const Expr& e)     { return is_num(e) && e.num == 1; }
[[nodiscard]] constexpr bool is_neg_one(const Expr& e) { return is_num(e) && e.num == -1; }
[[nodiscard]] constexpr bool is_neg(const Expr& e)     { return e.type == ExprType::UNARY_NEG; }
[[nodiscard]] constexpr bool is_neg_num(const Expr& e) { return is_num(e) && e.num < 0; }
// Pointer versions (null-safe, for struct fields)
[[nodiscard]] inline bool is_num(const Expr* e)     { return e && is_num(*e); }
[[nodiscard]] inline bool is_var(const Expr* e)     { return e && is_var(*e); }
[[nodiscard]] inline bool is_atomic(const Expr* e)  { return e && is_atomic(*e); }
[[nodiscard]] inline bool is_zero(const Expr* e)    { return e && is_zero(*e); }
[[nodiscard]] inline bool is_one(const Expr* e)     { return e && is_one(*e); }
[[nodiscard]] inline bool is_neg_one(const Expr* e) { return e && is_neg_one(*e); }
[[nodiscard]] inline bool is_neg(const Expr* e)     { return e && is_neg(*e); }
[[nodiscard]] inline bool is_neg_num(const Expr* e) { return e && is_neg_num(*e); }

[[nodiscard]] constexpr bool is_additive(BinOp op)       { return op == BinOp::ADD || op == BinOp::SUB; }
[[nodiscard]] constexpr bool is_multiplicative(BinOp op) { return op == BinOp::MUL || op == BinOp::DIV; }

// ============================================================================
//  Rational number helpers (structural fractions)
// ============================================================================
// Rational numbers are represented as DIV(Num(a), Num(b)) in the expression tree.
// This avoids adding fields to Expr and preserves sizeof(Expr).

// Is this a double value that's an exact integer?
[[nodiscard]] inline bool is_integer_value(double v) {
    return std::abs(v) < 1e15 && v == std::floor(v);
}

// Is this a structural fraction: DIV(Num(int), Num(int))?
[[nodiscard]] inline bool is_int_frac(const Expr& e) {
    return e.type == ExprType::BINOP && e.op == BinOp::DIV
        && is_num(*e.left) && is_num(*e.right)
        && is_integer_value(e.left->num) && is_integer_value(e.right->num)
        && e.right->num != 0;
}
[[nodiscard]] inline bool is_int_frac(const ExprPtr e) { return e && is_int_frac(*e); }

// Extract rational (numer, denom) from a Num or structural fraction.
// Returns {n, 1} for plain integers, {p, q} for DIV(Num(p), Num(q)).
[[nodiscard]] inline std::pair<int64_t, int64_t> to_rational(const Expr& e) {
    if (is_int_frac(e))
        return {static_cast<int64_t>(e.left->num), static_cast<int64_t>(e.right->num)};
    if (is_num(e) && is_integer_value(e.num))
        return {static_cast<int64_t>(e.num), 1};
    return {0, 0}; // not rational
}
[[nodiscard]] inline std::pair<int64_t, int64_t> to_rational(const ExprPtr e) {
    return e ? to_rational(*e) : std::pair<int64_t, int64_t>{0, 0};
}

// GCD for normalization
[[nodiscard]] inline int64_t gcd_abs(int64_t a, int64_t b) {
    a = std::abs(a); b = std::abs(b);
    while (b) { a %= b; std::swap(a, b); }
    return a;
}

// Build a normalized rational expression: GCD-reduced, sign in numerator.
// Returns Num(n) if denominator is 1 after reduction.
[[nodiscard]] inline ExprPtr make_rational(int64_t numer, int64_t denom) {
    assert(denom != 0 && "make_rational: zero denominator");
    if (numer == 0) return Expr::Num(0);
    // Sign normalization: negative in numerator only
    if (denom < 0) { numer = -numer; denom = -denom; }
    // GCD reduction
    const int64_t g = gcd_abs(numer, denom);
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
    // C17-constexpr-eligible: requires constexpr BinOpInfo ctor (Future.md #58).
    // The eval field is a plain function pointer (non-capturing lambdas decay
    // cleanly), but BinOpInfo is an aggregate with no constexpr constructor —
    // the brace-init list above is not a constant expression in C++17 because
    // of the aggregate-init rules. C++20 fixes this with constexpr aggregate.
    // static const: aggregate-init not constexpr-able in C++17 (C++20 fixes)
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

// sign(x) — symbolic builtin for derivative of abs (x != 0). Numeric
// evaluator returns 0/1/-1 by IEEE-754 sign comparison; the simplifier
// rewrite rule `abs(x)/x = sign(x) iff x != 0` (in BUILTIN_REWRITE_RULES)
// canonicalizes `diff(abs(x), x)`. At x=0 the companion rule
// `abs(x)/x = undefined iff x = 0` keeps the boundary explicit.
inline double sign_eval(double x) {
    if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
    return (x > 0) - (x < 0);
}

inline const std::map<std::string, double(*)(double)>& builtin_functions() {
    // static const: std::map runtime-init, not constexpr-able in C++17
    static const std::map<std::string, double(*)(double)> registry = {
        {"sqrt", std::sqrt}, {"abs", std::fabs}, {"sin",  std::sin},
        {"cos",  std::cos},  {"tan", std::tan},  {"log",  std::log},
        {"asin", std::asin}, {"acos",std::acos}, {"atan", std::atan},
        {"sign", sign_eval}
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
    // static const: std::map runtime-init, not constexpr-able in C++17
    static const std::map<std::string, double> registry = {
        {"pi",  M_PI},
        {"e",   M_E},
        {"phi", (1.0 + std::sqrt(5.0)) / 2.0},  // golden ratio 1.618...
        // Imaginary unit. NaN binding intentional: complex-containing
        // expressions return empty Checked<double> via the NaN-as-empty
        // contract, matching the surface for any other domain failure.
        // Symbolic identity ships as a rewrite rule (`i ^ 2 = -1`).
        // See docs/Developer.md §"Complex numbers".
        {"i",   std::numeric_limits<double>::quiet_NaN()},
    };
    return registry;
}

// ============================================================================
//  Undefined: symbolic domain boundary
// ============================================================================

// "undefined" is represented as Var("undefined") — no parser changes needed.
// It propagates through arithmetic (like NaN) and throws at evaluation time.
[[nodiscard]] inline bool is_undefined(const ExprPtr& e) {
    return e && e->type == ExprType::VAR && e->name == "undefined";
}

// ============================================================
// Section: Symbolic algebra (simplify, evaluate, evaluate_symbolic, rewrite engine)
// ============================================================

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
[[nodiscard]] inline bool contains_var(const Expr& e, const std::string& v) {
    switch (e.type) {
        case ExprType::NUM:       return false;
        case ExprType::VAR:       return e.name == v;
        case ExprType::BINOP:     return contains_var(*e.left, v) || contains_var(*e.right, v);
        case ExprType::UNARY_NEG: return contains_var(*e.child, v);
        case ExprType::FUNC_CALL:
            return std::any_of(e.args.begin(), e.args.end(),
                [&v](const Expr* a) { return contains_var(*a, v); });
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return false;
    }
    return false;
}

// Structural equality — no allocation, used for simplifier fixpoint
[[nodiscard]] inline bool expr_equal(const Expr& a, const Expr& b) {
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
            // justified: parallel iteration over a.args and b.args
            for (size_t i = 0; i < a.args.size(); i++)
                if (!expr_equal(*a.args[i], *b.args[i])) return false;
            return true;
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return false;
    }
    return false;
}
// Pointer overloads for convenience
inline void collect_vars(const Expr* e, std::set<std::string>& out) { if (e) collect_vars(*e, out); }
[[nodiscard]] inline bool contains_var(const Expr* e, const std::string& v) { return e && contains_var(*e, v); }
[[nodiscard]] inline bool expr_equal(const Expr* a, const Expr* b) {
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
[[nodiscard]] inline constexpr bool is_commutative(BinOp op) {
    return op == BinOp::ADD || op == BinOp::MUL;
}

// Helper: is this an additive chain (ADD/SUB at top)?
[[nodiscard]] inline bool is_additive_chain(const ExprPtr& e) {
    return e && e->type == ExprType::BINOP && is_additive(e->op);
}

[[nodiscard]] inline std::optional<std::map<std::string, ExprPtr>> match_pattern(
        const ExprPtr& pattern, const ExprPtr& target) {
    if (!pattern || !target) return std::nullopt;
    std::map<std::string, ExprPtr> bindings;

    // Decompose an additive term into its multiplicative factors for matching.
    // Returns {numeric_coeff, [(base, exponent)]} where base^exponent are the factors.
    struct TermFactors {
        double coeff;
        std::vector<std::pair<ExprPtr, double>> factors; // base^exp pairs
    };

    // Recursive matcher state: holds a reference to the `bindings` map that
    // accumulates wildcard bindings across the recursion. Per-call working
    // state (factor-permutation cursors etc.) is passed as parameters.
    struct PatternMatcher {
        std::map<std::string, ExprPtr>& bindings;

        static TermFactors decompose_term(double additive_coeff, const ExprPtr& base) {
            TermFactors tf;
            tf.coeff = additive_coeff;
            if (!base) return tf;  // pure constant
            double mul_coeff = 1.0;
            flatten_multiplicative(base, mul_coeff, tf.factors);
            tf.coeff *= mul_coeff;
            return tf;
        }

        bool match(const ExprPtr& p, const ExprPtr& t) {
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
                        return backtrack(0, p_tf, t_tf, used);
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
                // justified: parallel iteration over p->args and t->args
                for (size_t i = 0; i < p->args.size(); i++)
                    if (!match(p->args[i], t->args[i])) return false;
                return true;
            }

            return false;
        }

        // Match a pattern term's multiplicative factors against a target term's factors.
        // Pattern wildcards that don't match any target factor bind to 1.
        bool match_factors(const TermFactors& p, const TermFactors& t) {
            // Separate pattern factors into wildcards (plain vars) and structural
            std::vector<size_t> p_wildcards, p_structural;
            // justified: pattern-matcher dual-cursor — i/ti/ri cross-indexed into t_used / t_remaining / t_rem_used
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
            for (const size_t si : p_structural) {
                auto& [p_base, p_exp] = p.factors[si];
                bool found = false;
                // justified: dual-cursor (ti indexes t_used skip-mask)
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

            // Collect remaining target factors as individual expressions.
            // The wildcard branch (common case) consumes t_remaining directly;
            // the no-wildcard early-exit only needs to know whether the
            // collection is empty (formerly an unused product-ExprPtr build).
            double remaining_coeff = (std::abs(p.coeff) < EPSILON_ZERO) ? 0.0 : t.coeff / p.coeff;
            std::vector<ExprPtr> t_remaining;
            // justified: dual-cursor (ti indexes t_used skip-mask)
            for (size_t ti = 0; ti < t.factors.size(); ti++) {
                if (t_used[ti]) continue;
                auto& [t_base, t_exp] = t.factors[ti];
                t_remaining.push_back((std::abs(t_exp - 1.0) < EPSILON_ZERO) ? t_base
                    : Expr::BinOpExpr(BinOp::POW, t_base, Expr::Num(t_exp)));
            }

            if (p_wildcards.empty())
                return t_remaining.empty() && std::abs(remaining_coeff - 1.0) < EPSILON_ZERO;

            // Try to assign remaining target factors to wildcards via backtracking
            // Unassigned wildcards get the numeric coefficient (or 1)
            std::vector<bool> t_rem_used(t_remaining.size(), false);
            return assign_wildcards(0, p, p_wildcards, t_remaining, t_rem_used, remaining_coeff);
        }

        bool assign_wildcards(size_t wi,
                const TermFactors& p, const std::vector<size_t>& p_wildcards,
                const std::vector<ExprPtr>& t_remaining, std::vector<bool>& t_rem_used,
                double& remaining_coeff) {
            if (wi == p_wildcards.size()) {
                // All wildcards assigned; check no unmatched target factors
                return std::all_of(t_rem_used.begin(), t_rem_used.end(),
                    [](bool u) { return u; });
            }
            auto& var_name = p.factors[p_wildcards[wi]].first->name;

            // Try matching this wildcard against each remaining target factor
            // justified: dual-cursor (ri indexes t_rem_used skip-mask)
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
                    if (assign_wildcards(wi + 1, p, p_wildcards, t_remaining, t_rem_used, remaining_coeff)) return true;
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
                    const double saved_coeff = remaining_coeff;
                    remaining_coeff = 1.0;  // consumed
                    if (assign_wildcards(wi + 1, p, p_wildcards, t_remaining, t_rem_used, remaining_coeff)) return true;
                    remaining_coeff = saved_coeff;
                }
                bindings = saved;
            }

            return false;
        }

        bool backtrack(size_t pi,
                const std::vector<TermFactors>& p_tf,
                const std::vector<TermFactors>& t_tf,
                std::vector<bool>& used) {
            if (pi == p_tf.size()) return true;
            // justified: dual-cursor (ti indexes used skip-mask)
            for (size_t ti = 0; ti < t_tf.size(); ti++) {
                if (used[ti]) continue;
                auto saved = bindings;
                if (match_factors(p_tf[pi], t_tf[ti])) {
                    used[ti] = true;
                    if (backtrack(pi + 1, p_tf, t_tf, used)) return true;
                    used[ti] = false;
                }
                bindings = saved;
            }
            return false;
        }
    };

    PatternMatcher matcher{bindings};
    if (matcher.match(pattern, target)) return bindings;
    return std::nullopt;
}

// Apply a rewrite: substitute bindings into the replacement template.
[[nodiscard]] inline ExprPtr apply_rewrite(const ExprPtr& replacement,
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
        std::transform(replacement->args.begin(), replacement->args.end(),
            std::back_inserter(args),
            [&bindings](ExprPtr a) { return apply_rewrite(a, bindings); });
        return Expr::Call(replacement->name, args);
    }
    return replacement;
}

[[nodiscard]] inline int precedence(const Expr& e) {
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
            const auto& info = binop_info(e.op);
            const int prec = info.precedence;

            auto wrap = [&](const Expr& child, bool rhs) {
                const int cp = precedence(child);
                const bool need = (cp < prec) ||
                    (cp == prec && rhs && (e.op == BinOp::SUB || e.op == BinOp::DIV));
                const auto s = expr_to_string(child);
                return need ? "(" + s + ")" : s;
            };
            return wrap(*e.left, false) + info.symbol + wrap(*e.right, true);
        }

        case ExprType::FUNC_CALL: {
            // Vec/Mat sugar (M3): `vec(a, b)` renders as `[a, b]`. `mat(...)`
            // is a vec-of-vec — each row is itself a vec, so the recursive
            // expr_to_string call on each element naturally produces nested
            // `[[a, b], [c, d]]`. No special-case branching needed for mat.
            if (e.name == "vec" || e.name == "mat") {
                return "[" + join_with_sep(e.args, ", ",
                    [](const Expr* arg) { return expr_to_string(*arg); }) + "]";
            }
            return e.name + "(" + join_with_sep(e.args, ", ",
                [](const Expr* arg) { return expr_to_string(*arg); }) + ")";
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return "?";
}
// Pointer overloads
[[nodiscard]] inline int precedence(const Expr* e) { return e ? precedence(*e) : 5; }
inline std::string expr_to_string(const Expr* e) { return e ? expr_to_string(*e) : "?"; }

// ============================================================================
//  Tree-map templates — post-order rewrite primitives
// ============================================================================
//
// Two narrow primitives shared by all leaf/full-tree rewriters in fwiz.
//
// `tree_map<Fn>` calls `fn(node)` AFTER a node's children have been rewritten.
// If `fn` returns the same pointer it received and no child changed, the
// original parent is returned without reconstruction (zero allocations on the
// no-match path). Used by callers that match subtrees of any shape.
//
// `tree_map_leaf<Fn>` calls `fn(node)` only on NUM/VAR terminals; interior
// nodes are passed through structurally with the same pointer-equality
// short-circuit. Used by callers that only ever rewrite leaves — they avoid
// the "if (!is_var(node)) return node;" guard at every call site.
//
// Both templates are function templates (implicitly inline) and forward `Fn`
// by universal reference so lambdas with captures pass through cleanly.
template<typename Fn>
[[nodiscard]] ExprPtr tree_map(ExprPtr e, Fn&& fn) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return fn(e);
        case ExprType::UNARY_NEG: {
            auto nc = tree_map(e->child, fn);
            return fn((nc == e->child) ? e : Expr::Neg(nc));
        }
        case ExprType::BINOP: {
            auto nl = tree_map(e->left, fn);
            auto nr = tree_map(e->right, fn);
            return fn((nl == e->left && nr == e->right) ? e : Expr::BinOpExpr(e->op, nl, nr));
        }
        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> na;
            bool changed = false;
            na.reserve(e->args.size());
            for (const auto& a : e->args) {
                auto ra = tree_map(a, fn);
                if (ra != a) changed = true;
                na.push_back(ra);
            }
            return fn(changed ? Expr::Call(e->name, std::move(na)) : e);
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return e;
    }
    return e;  // unreachable; switch is exhaustive
}

template<typename Fn>
[[nodiscard]] ExprPtr tree_map_leaf(ExprPtr e, Fn&& fn) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return fn(e);
        case ExprType::UNARY_NEG: {
            auto nc = tree_map_leaf(e->child, fn);
            return (nc == e->child) ? e : Expr::Neg(nc);
        }
        case ExprType::BINOP: {
            auto nl = tree_map_leaf(e->left, fn);
            auto nr = tree_map_leaf(e->right, fn);
            return (nl == e->left && nr == e->right) ? e : Expr::BinOpExpr(e->op, nl, nr);
        }
        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> na;
            bool changed = false;
            na.reserve(e->args.size());
            for (const auto& a : e->args) {
                auto ra = tree_map_leaf(a, fn);
                if (ra != a) changed = true;
                na.push_back(ra);
            }
            return changed ? Expr::Call(e->name, std::move(na)) : e;
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return e;
    }
    return e;  // unreachable; switch is exhaustive
}

// ============================================================================
//  Substitute
// ============================================================================

[[nodiscard]] inline ExprPtr substitute(ExprPtr e, const std::string& var, ExprPtr val) {
    return tree_map_leaf(e, [&](ExprPtr node) -> ExprPtr {
        if (is_var(node) && node->name == var) return val;
        return node;
    });
}

// ============================================================================
//  Structural subtree replacement — replace named subtrees with Var nodes
// ============================================================================
//
// Given an expression and an ordered list of (name, subtree) pairs, replace
// every occurrence of a subtree (matched by structural equality) with
// `Var(name)`. Two consumers in-tree: --cse derive output (CSE pipeline) and
// u-substitution in `try_u_sub_integrate` (single-pair generic replace).
//
// Walk is post-order (children first). After children are rewritten, the
// match check is performed on `e` directly (not a reconstructed node), so an
// outer match still hits even after its children have been rewritten.
//
// Pointer-equality short-circuit on the no-match path: fwiz's factory pattern
// (Expr::BinOpExpr/Neg/Call) ALWAYS reconstructs a fresh node, so without this
// guard a tree with no match still pays O(|tree|) allocations. The guard
// returns the original `e` when (a) no child changed by pointer identity AND
// (b) no replacement target equals the current node.
[[nodiscard]] inline ExprPtr replace_subtree_by_name(ExprPtr e,
        const std::vector<std::pair<std::string, ExprPtr>>& replacements) {
    return tree_map(e, [&](ExprPtr node) -> ExprPtr {
        for (auto& [name, sub] : replacements)
            if (expr_equal(node, sub)) return Expr::Var(name);
        return node;
    });
}

// Walk an expression tree and replace every Var node whose name is a builtin
// symbolic constant (pi, e, phi) with its numeric Num value. Used by the
// --approximate derive path to collapse `2 * pi * r` → `6.28... * r` after
// simplification. User-defined defaults (e.g. g = 9.81) are NOT touched —
// the source of truth is builtin_constants() which holds only the true
// mathematical constants.
[[nodiscard]] inline ExprPtr substitute_builtin_constants(ExprPtr e) {
    return tree_map_leaf(e, [](ExprPtr node) -> ExprPtr {
        if (!is_var(node)) return node;
        auto& consts = builtin_constants();
        auto it = consts.find(node->name);
        return (it != consts.end()) ? Expr::Num(it->second) : node;
    });
}

// ============================================================================
//  Evaluate
// ============================================================================

[[nodiscard]] inline Checked<double> evaluate(const Expr& e) {
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
[[nodiscard]] inline Checked<double> evaluate(const Expr* e) {
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

[[nodiscard]] inline std::vector<double> fingerprint_expr(
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
        const double d = v.value();
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

[[nodiscard]] inline std::pair<int, int> canonicity_score(ExprPtr e) {
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
[[nodiscard]] inline ExprPtr evaluate_symbolic(const Expr& e) {
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
        const bool all_num = std::all_of(e.args.begin(), e.args.end(),
            [](const Expr* a) { return is_num(a); });
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
[[nodiscard]] inline std::pair<ExprPtr, double> split_pow(ExprPtr e) {
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
[[nodiscard]] inline ExprPtr rebuild_additive(const std::vector<std::pair<double, ExprPtr>>& terms) {
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

// If e is a structural int-fraction, emit it as a single factor as-is and return true.
// Preserves DIV(Num, Num) shape that derive output relies on — must run before any
// DIV decomposition path would split it into numerator/denominator factors.
[[nodiscard]] inline bool try_emit_int_frac_factor(
    const ExprPtr& e,
    std::vector<std::pair<ExprPtr, double>>& factors)
{
    if (!is_int_frac(e)) return false;
    factors.push_back({e, 1.0});
    return true;
}

// Flatten a MUL chain into (base, exponent) factors.
// Only flattens through MUL, not DIV (to preserve division structure).
// Numeric constants are collected into a single coefficient.
inline void flatten_multiplicative(ExprPtr e,
                                   double& coeff,
                                   std::vector<std::pair<ExprPtr, double>>& factors) {
    assert(e && "flatten_multiplicative: null expression");
    if (try_emit_int_frac_factor(e, factors)) return;
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
[[nodiscard]] inline ExprPtr rebuild_multiplicative(double coeff,
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

    const bool neg = coeff < 0;
    const double abs_coeff = std::abs(coeff);

    // Build numerator: coeff (if not 1) * positive-exp factors
    ExprPtr num = nullptr;
    if (abs_coeff != 1.0 || (num_parts.empty() && denom_parts.empty())) num = Expr::Num(abs_coeff);
    // not std::accumulate: conditional first-element seeding (num is null until first non-empty assignment)
    // cppcheck-suppress useStlAlgorithm
    for (auto& f : num_parts) num = num ? Expr::BinOpExpr(BinOp::MUL, num, f) : f;
    if (!num) num = Expr::Num(1);

    // If any negative-exp factors, wrap in DIV; else just numerator
    ExprPtr result = num;
    if (!denom_parts.empty()) {
        ExprPtr denom = denom_parts.front();
        // not std::accumulate: skip-first; rewriting via std::next(begin, 1) is less readable than the indexed form
        for (size_t i = 1; i < denom_parts.size(); i++)
            // cppcheck-suppress useStlAlgorithm
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
    // justified: outer of triangular pair (j = i+1) over `items`
    for (size_t i = 0; i < items.size(); i++) {
        if (!key(items[i])) continue;
        // justified: triangular pair (j = i+1) over `items`
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

// Source of a simplification assumption: derived from a rewrite-rule condition,
// or inherent in the original expression's domain (e.g. S/S → 1 is safe because
// S was already undefined at S=0).
enum class AssumptionSource : uint8_t { Derived = 0, Inherent = 1 };

// When simplify applies rules with conditions, it records assumptions.
// Call simplify_get_assumptions() after simplify() to retrieve them.
struct SimplifyAssumption {
    ExprPtr expr;       // the expression constrained (may be null for general conditions)
    std::string desc;   // human-readable: "x - 3 != 0", "x > 0"
    AssumptionSource source = AssumptionSource::Derived;
};

inline std::vector<SimplifyAssumption>& simplify_assumptions_() {
    static thread_local std::vector<SimplifyAssumption> assumptions;
    return assumptions;
}

inline void simplify_record_assumption(const ExprPtr& expr, const std::string& desc,
                                       AssumptionSource src = AssumptionSource::Derived) {
    auto& a = simplify_assumptions_();
    if (std::any_of(a.begin(), a.end(),
            [&desc](const SimplifyAssumption& existing) { return existing.desc == desc; }))
        return;  // dedup by string
    a.push_back({expr, desc, src});
}

// Division cancellation: S/S → 1 is inherently safe (S was already undefined at S=0)
inline void simplify_assume_nonzero(const ExprPtr& expr,
                                    AssumptionSource src = AssumptionSource::Inherent) {
    if (is_num(expr)) return;
    simplify_record_assumption(expr, expr_to_string(expr) + " != 0", src);
}

[[nodiscard]] inline std::vector<SimplifyAssumption> simplify_get_assumptions() {
    auto result = std::move(simplify_assumptions_());
    simplify_assumptions_().clear();
    return result;
}

inline void simplify_clear_assumptions() {
    simplify_assumptions_().clear();
}

// ============================================================================
//  Conditions — symmetric AST shared by equations and rewrite rules
// ============================================================================

// Forward decl: Condition::to_valueset / check_condition call simplify (defined
// later in this file). Inline member-function bodies need the name visible at
// definition; the actual call resolves at the post-parse point of use.
[[nodiscard]] inline ExprPtr simplify(const ExprPtr& e);

enum class CondOp : uint8_t { GT, GE, LT, LE, EQ, NE, COUNT_ };
enum class CondLogic : uint8_t { AND, OR };

static_assert(static_cast<int>(CondOp::COUNT_) == 6, "CondOp has 6 real values");

struct CondClause {
    ExprPtr lhs;
    ExprPtr rhs;
    CondOp op;
};

// Typed-binding predicates for rule conditions (Future #53). A predicate clause
// is encoded as `CondClause{lhs=FUNC_CALL("is_*", {Var("name"), ...}),
// rhs=nullptr, op=CondOp::EQ}`. `is_predicate_clause()` recognises this shape
// structurally — no extra fields on CondClause.
//
// Current canonical set (gen-5 cycle 3a, 2026-05-15): `is_neg_num` (literal-shape
// test) and `is_in` (named-set membership). The cycle-2 names `is_int` and
// `is_in_dimension` are rewritten to `is_in(_, int)` / `is_in(_, _)` at
// parse time (system.h::parse_condition) per critic D8 SIMPLIFY — they no
// longer appear at this layer. Switch to a static set when count >= 6.
[[nodiscard]] inline bool is_predicate_clause(const CondClause& c) {
    return c.lhs && c.lhs->type == ExprType::FUNC_CALL
        && (c.lhs->name == "is_neg_num"
            || c.lhs->name == "is_in");
}

struct Condition {
    std::vector<CondClause> clauses;
    std::vector<CondLogic> connectors; // size = clauses.size() - 1

    // Convert condition to a ValueSet for a specific variable
    // Only works for simple conditions like "x > 0", "x <= 10"
    [[nodiscard]] ValueSet to_valueset(const std::string& var,
                         const std::map<std::string, double>& bindings = {}) const {
        ValueSet result = ValueSet::all();
        // justified: index needed to look up parallel `connectors[i-1]`
        for (size_t i = 0; i < clauses.size(); i++) {
            const auto& c = clauses[i];
            // Check if this clause constrains `var`
            const bool lhs_is_var = is_var(c.lhs) && c.lhs->name == var;
            const bool rhs_is_var = is_var(c.rhs) && c.rhs->name == var;
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
            auto val_opt = evaluate(*simplify(resolved));
            if (!val_opt) return ValueSet::all();
            const double val = val_opt.value();

            // Build ValueSet from operator (flip if var is on RHS)
            CondOp op = c.op;
            if (rhs_is_var) {
                // Flip: "5 > x" becomes "x < 5"
                switch (op) {
                    case CondOp::GT: op = CondOp::LT; break;
                    case CondOp::GE: op = CondOp::LE; break;
                    case CondOp::LT: op = CondOp::GT; break;
                    case CondOp::LE: op = CondOp::GE; break;
                    case CondOp::EQ: case CondOp::NE: case CondOp::COUNT_: break;
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
                case CondOp::COUNT_: assert(false && "invalid CondOp in to_valueset"); break;
            }

            // i == 0 always intersects (no prior connector); otherwise the
            // connector at i-1 decides intersect vs unite.
            if (i == 0 || connectors[i-1] == CondLogic::AND)
                result = result.intersect(clause_set);
            else
                result = result.unite(clause_set);
        }
        return result;
    }
};

// Dimension exponent algebra (gen-5 cycle 3c, Future #7b FULL).
// A DimMap maps an atomic dimension name to its integer exponent:
//   {} = dimensionless; {"mass":1} = mass; {"length":1,"time":-2} = accel.
// Zero-exponent entries are dropped (dim_zero_clean) so map-equality is the
// canonical "same dimension" test. compute_dim folds expressions into DimMaps.
using DimMap = std::map<std::string, int>;

// Per-binding type record (gen-5 cycle 3a, 2026-05-15; dim promoted cycle 3c).
// `dim` is the binding's dimension as an exponent map (DimMap). Empty if no
//   DIM_SECTION annotation. A base unit registers as a unit-vector ({name:1});
//   compute_dim composes these through MUL/DIV/POW/sqrt arithmetic.
// `sets` holds BUILTIN_PREDICATE and (cycle 3b) USER_PREDICATE memberships
//   by set name: "int", "real", "imaginary", "rational", and user-defined.
//
// Lives in expr.h (M3) so check_condition's is_in dispatch can see the full
// shape; FormulaSystem owns the per-system map<string, BindingType> instance.
struct BindingType {
    DimMap dim;                    // exponent algebra (cycle 3c); empty = dimensionless
    std::set<std::string> sets;    // membership: {"int"}, {"imaginary"}, ...
};

// ---- DimMap arithmetic (gen-5 cycle 3c, Future #7b FULL) ----
// Drop zero-exponent entries so map-equality is the canonical dimension test
// (e.g. {mass:1,mass:-1} cancels to {} = dimensionless after a divide).
inline DimMap dim_zero_clean(DimMap m) {
    for (auto it = m.begin(); it != m.end(); )
        it = (it->second == 0) ? m.erase(it) : std::next(it);
    return m;
}
// MUL: add exponent maps (m^a * m^b = m^(a+b)).
inline DimMap dim_merge_add(DimMap a, const DimMap& b) {
    for (const auto& [k, v] : b) a[k] += v;
    return dim_zero_clean(std::move(a));
}
// DIV: subtract exponent maps (m^a / m^b = m^(a-b)).
inline DimMap dim_merge_sub(DimMap a, const DimMap& b) {
    for (const auto& [k, v] : b) a[k] -= v;
    return dim_zero_clean(std::move(a));
}
// POW: scale exponents by an integer factor ((m^a)^n = m^(a*n)).
// factor==0 is handled by the general loop (all exponents → 0 → cleaned to {}).
inline DimMap dim_scale(DimMap m, int factor) {
    for (auto& [k, v] : m) v *= factor;
    return dim_zero_clean(std::move(m));
}
// Forward declaration: compute_dim is defined after builtin_meta() (its
// FUNC_CALL branch reads the registry), but check_condition's DIM_SECTION arm
// (above the registry) calls it.
[[nodiscard]] inline std::optional<DimMap>
compute_dim(const Expr& e, const std::map<std::string, BindingType>& type_map);

// Named-set registry value type (gen-5 cycle 3a, 2026-05-15).
// `kind` selects the dispatch path used by `check_condition`'s `is_in`
// predicate; `membership` is populated for BUILTIN_PREDICATE only.
//
// Forward-compat: cycles 3b (USER_PREDICATE) and 3d (FUNCTION_SECTION) each
// bump the COUNT_ static_assert and extend the dispatch switch. Per critic
// D3: dead-fields not pre-allocated — each cycle ships exactly what it
// needs.
struct SetDef {
    std::string name;
    // Enum order: BUILTIN_PREDICATE=0, USER_PREDICATE=1, DIM_SECTION=2,
    // FUNCTION_SECTION=3 — matches registration order (built-ins first via
    // load_builtins(); then user-defined via register_predicate_section();
    // then dim sections via register_dim_section(); then function sections
    // via register_function_section() in the pre-scan loop).
    enum class Kind { BUILTIN_PREDICATE, USER_PREDICATE, DIM_SECTION, FUNCTION_SECTION, COUNT_ };
    static_assert(static_cast<int>(Kind::COUNT_) == 4,
                  "SetDef::Kind: cycle 3d added FUNCTION_SECTION; update check_condition "
                  "dispatch (expr.h) AND annotation-parse switch (system.h)");
    Kind kind = Kind::BUILTIN_PREDICATE;
    bool (*membership)(double v) = nullptr;  // BUILTIN_PREDICATE only
    // USER_PREDICATE fields (cycle 3b, 2026-05-16). Empty/nullopt for other Kinds.
    // `parameter` is the formal parameter name from `[name(param)]` header;
    // for FUNCTION_SECTION (cycle 3d) it stores the single positional arg
    // name from `[name(arg) -> ret]` instead — semantically consistent (both
    // are the formal parameter name).
    std::string parameter;
    std::optional<Condition> predicate;
    // FUNCTION_SECTION field (cycle 3d, 2026-05-16). Empty for other Kinds.
    // Stores the section's return variable (e.g. "result" for
    // `[fibonacci(n) -> result]`). Used by the ExistenceChecker callback
    // wired in system.h: sub.resolve(parameter, {{function_section_name, v}}).
    std::string function_section_name;
};

// Solver context bundle (gen-5 cycle 3a) — replaces the cycle-2
// simplify_dim_map_ with a struct-extensible single thread-local. Set by
// RewriteRulesGuard during rule firing; read by check_condition's `is_in`
// predicate dispatch. Pointer fields nullable; check_condition fail-safes
// to false when context is incomplete.
struct SimplifyContext {
    const std::map<std::string, BindingType>* type_map = nullptr;
    const std::map<std::string, SetDef>*      set_defs = nullptr;
};

inline const SimplifyContext*& simplify_set_ctx_() {
    static thread_local const SimplifyContext* ctx = nullptr;
    return ctx;
}

// Forward-declare ExistenceChecker accessor (gen-5 cycle 3d): the full
// using-alias + thread-local accessor are defined near solve_func_inverter_
// (see end of this file). `check_condition`'s FUNCTION_SECTION dispatch arm
// calls this accessor; the definition can't be moved up without dragging
// FuncInverter with it (both are part of the same solver-boundary erasure
// surface, conventionally grouped at the end of expr.h).
using ExistenceChecker = std::function<bool(const std::string& set_name, double value)>;
inline ExistenceChecker& solve_existence_checker_();

// True iff any clause's lhs or rhs subexpression contains `var`.
// Used by Strategy 6 (numeric scan) in enumerate_candidates to emit
// candidates for equations like `result = n if n <= 1` when solving for
// `n` — the condition `n <= 1` contains the target, even though the RHS
// (the literal `n`, or another expression not containing the target)
// alone does not. Predicate clauses (`is_in`, `is_neg_num`) are walked
// the same way — their FUNC_CALL lhs args carry the relevant vars.
// gen-5 cycle 3h (2026-05-16, Future #92).
[[nodiscard]] inline bool contains_var_in_condition(const Condition& cond,
                                                    const std::string& var) {
    return std::any_of(cond.clauses.begin(), cond.clauses.end(),
        [&var](const CondClause& c) {
            return contains_var(c.lhs, var) || contains_var(c.rhs, var);
        });
}

// Check if a condition is satisfied given current bindings.
// Unknown clauses (variables not in bindings, non-builtin) are treated as satisfied
// for COMPARISON clauses (permissive-true). PREDICATE clauses use fail-safe
// semantics: unknown binding → false. `expr_bindings` is optional — only rule-
// condition callers (e.g. `apply_rewrite_rules`) pass it; equation-context callers
// pass nullptr and predicate clauses then short-circuit to false (Future #53).
//
// `set_ctx` (gen-5 cycle 3a) carries the type_map_ + set_definitions_ pointers
// for the `is_in(_, _)` predicate dispatch. Nullable; null → false (fail-safe).
[[nodiscard]] inline bool check_condition(const Condition& cond,
                            const std::map<std::string, double>& bindings,
                            const std::map<std::string, ExprPtr>* expr_bindings = nullptr,
                            const SimplifyContext* set_ctx = nullptr) {
    auto eval_clause = [&](const CondClause& c) -> std::optional<bool> {
        // Predicate clauses (Future #53 / gen-5 cycle 3a): typed dispatch on
        // the bound ExprPtr. Fail-safe semantics — any unknown/missing/
        // wrong-shape arg → false.
        if (is_predicate_clause(c)) {
            const std::string& name = c.lhs->name;
            if (!expr_bindings) return false;
            // is_in(v, set_name) — unified named-set membership predicate
            // (gen-5 cycle 3a, extended cycles 3b + 3d + 3f). 9 cooperating
            // locations (V6c numbering preserved):
            //   1. Parse-time rewrite (system.h::parse_condition):
            //      is_int(n) → is_in(n, int) and is_in_dimension(n, m) →
            //      is_in(n, m). is_predicate_clause and this dispatcher only
            //      recognize is_in (and is_neg_num, the literal-shape
            //      predicate that does not fit the membership pattern).
            //   9. Infix-`in` synthesis (cycle 3f, system.h::parse_condition):
            //      ` in ` string-scan BEFORE the comparison-op loop lowers
            //      `x in set` to FUNC_CALL("is_in", [Var(x), Var(set)]) —
            //      identical AST to the function-call form. Both syntaxes
            //      arrive here through the same dispatch.
            //   2. Registry populated: system.h::load_builtins (built-ins),
            //      system.h::register_dim_section (DIM_SECTION),
            //      system.h::register_predicate_section (USER_PREDICATE —
            //      cycle 3b), and system.h::register_function_section
            //      (FUNCTION_SECTION — cycle 3d).
            //   3. Section disambiguation (system.h): is_dimension_section,
            //      is_predicate_section, and is_function_section (cycle 3d)
            //      by section header shape.
            //   4. Three-pass load_with_sections (system.h): dim → predicate
            //      → function (cycle 3d) — atom availability ordering.
            //   5. Transport (this file): const SimplifyContext* via
            //      thread-local simplify_set_ctx_(), set by RewriteRulesGuard
            //      (3 sites in system.h).
            //   6. Dispatch (this block): kind-based — BUILTIN_PREDICATE
            //      projects via evaluate(); DIM_SECTION compares
            //      type_map[var].dim to set_name; USER_PREDICATE evaluates
            //      stored Condition with parameter bound (cycle 3b);
            //      FUNCTION_SECTION delegates to ExistenceChecker (cycle 3d).
            //   7. Recursion guard: thread-local evaluating_predicates_
            //      keyed on set_name; blocks self-recursion AND chains
            //      (lifted to switch-prelude in cycle 3d; shared by
            //      USER_PREDICATE + FUNCTION_SECTION).
            //   8. ExistenceChecker thread-local + Guard (cycle 3d):
            //      expr.h::solve_existence_checker_ + system.h::
            //      ExistenceCheckerGuard. Set at 3 SimplifyContext
            //      construction sites (system.h:1652, 2155, 2170 — pre-3d
            //      line numbers; see derive_all/resolve/resolve_all). Each
            //      invocation wraps `sub.resolve(parameter, {{return_var, v}})`.
            if (name == "is_in") {
                if (c.lhs->args.size() != 2) return false;
                if (!is_var(c.lhs->args[0]) || !is_var(c.lhs->args[1])) return false;
                if (!set_ctx || !set_ctx->set_defs || !set_ctx->type_map) return false;
                const std::string& wildcard_name = c.lhs->args[0]->name;
                const std::string& set_name      = c.lhs->args[1]->name;
                auto bind_it = expr_bindings->find(wildcard_name);
                if (bind_it == expr_bindings->end()) return false;
                auto sdef_it = set_ctx->set_defs->find(set_name);
                if (sdef_it == set_ctx->set_defs->end()) return false; // unknown set → fail-safe
                const SetDef& sdef = sdef_it->second;
                // Shared recursion guard for USER_PREDICATE + FUNCTION_SECTION
                // (cycle 3d: lifted from inside the USER_PREDICATE arm to
                // switch-prelude scope). Keyed on `set_name` only — conservative:
                // blocks `is_in(x, a) → is_in(y, a)` chains too. BUILTIN_PREDICATE
                // and DIM_SECTION enter harmlessly — neither can recurse into
                // is_in dispatch — so the guard cost is one set insert+erase
                // per dispatch on those paths. Value-aware dedup parked
                // (Future #86).
                static thread_local std::set<std::string> evaluating_predicates_;
                if (evaluating_predicates_.count(set_name)) return false;
                evaluating_predicates_.insert(set_name);
                struct PredGuard {
                    std::set<std::string>& s;
                    std::string k;
                    ~PredGuard() { s.erase(k); }
                } _pg{evaluating_predicates_, set_name};
                switch (sdef.kind) {
                    case SetDef::Kind::BUILTIN_PREDICATE: {
                        // D7 semantic strengthening: project bound expr to
                        // double via evaluate() — is_in(Add(1,2), int)
                        // succeeds because evaluate yields 3.0 (cycle 2's
                        // is_int returned false on non-Num bindings).
                        //
                        // value_or_nan() — not value() — is the deliberate
                        // boundary escape here. NaN is meaningful for the
                        // `imaginary` built-in (cycle 2 invariant: the `i`
                        // binding uses NaN as an imaginary-unit sentinel),
                        // and the membership predicate may legitimately want
                        // to accept NaN. The membership fn itself decides.
                        auto val = evaluate(*bind_it->second);
                        return sdef.membership && sdef.membership(val.value_or_nan());
                    }
                    case SetDef::Kind::USER_PREDICATE: {
                        // Cycle 3b: USER_PREDICATE dispatch. Project the
                        // bound expression to double via evaluate(), insert
                        // the predicate's formal parameter into the caller's
                        // `bindings` map with RAII restoration, and
                        // recursively evaluate the stored Condition.
                        // (Recursion guard armed in switch-prelude above —
                        // shared with FUNCTION_SECTION per cycle 3d.)
                        if (!sdef.predicate.has_value() || sdef.parameter.empty())
                            return false;
                        auto val = evaluate(*bind_it->second);
                        if (!val) return false;
                        // D6 SIMPLIFY: insert-then-erase on caller's
                        // bindings map with RAII restoration (saves a full
                        // map copy per dispatch). `const_cast` is safe here:
                        // no call site stores a truly-const bindings map
                        // (grep-verified 2026-05-16). If a future caller
                        // does, drop `const` from the signature instead.
                        auto& mut_bindings =
                            const_cast<std::map<std::string, double>&>(bindings);
                        // val verified non-empty above (`if (!val) return false`);
                        // use .value() per the convention (.value_or_nan() is the
                        // boundary-escape for genuinely-might-be-NaN cases).
                        const double v = val.value();
                        auto [it, inserted] = mut_bindings.try_emplace(sdef.parameter, v);
                        double old_val = 0.0;
                        if (!inserted) { old_val = it->second; it->second = v; }
                        struct BindingRestore {
                            std::map<std::string, double>& m;
                            std::string k;
                            double v;
                            bool restore;
                            ~BindingRestore() {
                                if (restore) m[k] = v;
                                else m.erase(k);
                            }
                        } _br{mut_bindings, sdef.parameter, old_val, !inserted};
                        // Build a fresh expr_bindings binding parameter →
                        // bound ExprPtr, so a nested `is_in(param, ...)`
                        // inside the predicate body finds the queried expr.
                        std::map<std::string, ExprPtr> pred_eb;
                        pred_eb[sdef.parameter] = bind_it->second;
                        return check_condition(sdef.predicate.value(),
                                               bindings, &pred_eb, set_ctx);
                    }
                    case SetDef::Kind::DIM_SECTION: {
                        // cycle 3c: lifted the is_var guard — compute_dim folds
                        // arbitrary expressions (MUL/DIV/POW/sqrt) into a DimMap.
                        // Bare Var is the common path; compound exprs are the new
                        // path. nullopt (ADD/SUB mismatch) → false (fail-safe).
                        // Atomic membership: target set_name "mass" is the unit
                        // vector {mass:1}; named compound-dim aliases are #81.
                        if (!set_ctx->type_map) return false;
                        auto computed = compute_dim(*bind_it->second, *set_ctx->type_map);
                        if (!computed) return false;
                        return *computed == DimMap{{set_name, 1}};
                    }
                    case SetDef::Kind::FUNCTION_SECTION: {
                        // Cycle 3d: existential-solve dispatch via the
                        // boundary-erased ExistenceChecker thread-local
                        // (expr.h:solve_existence_checker_). The callback
                        // (installed by FormulaSystem at every solver-entry
                        // site) wraps `sub.resolve(parameter, {{return_var, v}})`
                        // and returns true iff some n satisfies section(n) = v.
                        //
                        // Perf trap: every `is_in(x, sec)` rewrite-rule
                        // condition triggers a full solver invocation. For
                        // rules consulting function-sections in tight
                        // matching loops, this can dominate `simplify()`
                        // time. Memoization deferred — Future #85.
                        auto val = evaluate(*bind_it->second);
                        if (!val) return false;
                        const auto& checker = solve_existence_checker_();
                        if (!checker) return false;  // no FormulaSystem context — fail-safe
                        return checker(set_name, val.value());
                    }
                    case SetDef::Kind::COUNT_:
                        assert(false && "SetDef::Kind::COUNT_ unreachable in check_condition");
                        return false;
                }
                return false; // exhaustive switch fall-through guard
            }
            // 1-arg predicates: `is_neg_num(n)`.
            if (c.lhs->args.size() != 1) return false;
            if (!is_var(c.lhs->args[0])) return false;
            const std::string& var_name = c.lhs->args[0]->name;
            auto it = expr_bindings->find(var_name);
            if (it == expr_bindings->end()) return false;
            if (name == "is_neg_num") return is_neg_num(it->second);
            return false; // unknown predicate name — fail-safe
        }
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
        auto l_opt = evaluate(*simplify(lhs));
        auto r_opt = evaluate(*simplify(rhs));
        if (!l_opt || !r_opt) return std::nullopt;
        const double l = l_opt.value();
        const double r = r_opt.value();
        switch (c.op) {
            case CondOp::GT: return l > r;
            case CondOp::GE: return l >= r;
            case CondOp::LT: return l < r;
            case CondOp::LE: return l <= r;
            case CondOp::EQ: return std::abs(l - r) < EPSILON_ZERO;
            case CondOp::NE: return std::abs(l - r) >= EPSILON_ZERO;
            case CondOp::COUNT_: assert(false && "invalid CondOp"); return false;
        }
        return std::nullopt;
    };

    bool result = true;
    // justified: index needed to look up parallel `cond.connectors[i-1]`
    for (size_t i = 0; i < cond.clauses.size(); i++) {
        auto val = eval_clause(cond.clauses[i]);
        const bool clause_result = !val.has_value() || val.value(); // unknown → true (satisfied)

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

// Serialize a Condition AST to a string with bindings substituted inline.
// Used for human-readable assumption descriptions in --steps/--calc traces.
inline std::string cond_op_to_string(CondOp op) {
    switch (op) {
        case CondOp::GT: return ">";
        case CondOp::GE: return ">=";
        case CondOp::LT: return "<";
        case CondOp::LE: return "<=";
        case CondOp::EQ: return "=";
        case CondOp::NE: return "!=";
        case CondOp::COUNT_: assert(false && "invalid CondOp"); return "?";
    }
    return "?";
}

inline std::string condition_to_string(const Condition& cond,
        const std::map<std::string, ExprPtr>& bindings) {
    std::string out;
    // justified: index needed to look up parallel `cond.connectors[i-1]`
    for (size_t i = 0; i < cond.clauses.size(); i++) {
        if (i > 0) out += (cond.connectors[i-1] == CondLogic::AND) ? " && " : " || ";
        const auto& c = cond.clauses[i];
        if (is_predicate_clause(c)) {
            // Predicate clauses (Future #53): render as `is_name(arg)`.
            // `expr_to_string` already handles FUNC_CALL pretty-printing.
            out += expr_to_string(c.lhs);
            continue;
        }
        ExprPtr l = c.lhs, r = c.rhs;
        for (auto& [var, val] : bindings) {
            l = substitute(l, var, val);
            r = substitute(r, var, val);
        }
        out += expr_to_string(l) + " " + cond_op_to_string(c.op) + " " + expr_to_string(r);
    }
    return out;
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
    // Optional condition AST parsed at rule-load time. nullopt = unconditional.
    std::optional<Condition> condition;
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

// (BindingType / SetDef / SimplifyContext / simplify_set_ctx_() are defined
// above, before check_condition. See gen-5 cycle 3a M3 comments at those
// declarations for the full-stack rationale.)

inline void simplify_set_rewrite_rules(const std::vector<RewriteRule>* rules,
                                        const std::vector<bool>* exhaustive = nullptr) {
    simplify_rewrite_rules_() = rules;
    simplify_rewrite_exhaustive_() = exhaustive;
}

[[nodiscard]] inline const std::vector<RewriteRule>* simplify_get_rewrite_rules() {
    return simplify_rewrite_rules_();
}

// RAII guard: sets rewrite rules + exhaustiveness flags + bindings + custom
// functions + SimplifyContext (gen-5 cycle 3a, 2026-05-15 — was a raw
// dim_map pointer in cycle 2; now a struct so future predicate additions
// extend the struct rather than the thread-local count).
struct RewriteRulesGuard {
    explicit RewriteRulesGuard(const std::vector<RewriteRule>* rules,
                      const std::vector<bool>* exhaustive = nullptr,
                      const std::map<std::string, double>* bindings = nullptr,
                      const std::map<std::string, double(*)(double)>* custom_funcs = nullptr,
                      const SimplifyContext* ctx = nullptr) {
        simplify_set_rewrite_rules(rules, exhaustive);
        simplify_bindings_() = bindings;
        custom_functions_ptr_() = custom_funcs;
        simplify_set_ctx_() = ctx;
    }
    ~RewriteRulesGuard() {
        simplify_set_rewrite_rules(nullptr, nullptr);
        simplify_bindings_() = nullptr;
        custom_functions_ptr_() = nullptr;
        simplify_set_ctx_() = nullptr;
    }
    RewriteRulesGuard(const RewriteRulesGuard&) = delete;
    RewriteRulesGuard& operator=(const RewriteRulesGuard&) = delete;
};

// ---- Simplify: per-operator helpers ----

[[nodiscard]] inline ExprPtr simplify_additive(const ExprPtr& combined) {
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
                const int64_t n = static_cast<int64_t>(c);
                rat_num = rat_num * 1 + n * rat_den; // rat += n/1
                // (no GCD yet — normalize at end)
                has_rational = true;
            } else {
                constant += c;
            }
        } else if (is_int_frac(b) && is_integer_value(c)) {
            // Structural fraction with integer coefficient: c * (p/q)
            auto [p, q] = to_rational(b);
            const int64_t ic = static_cast<int64_t>(c);
            rat_num = rat_num * q + ic * p * rat_den;
            rat_den *= q;
            // Prevent overflow by intermediate GCD
            const int64_t g = gcd_abs(rat_num, rat_den);
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

[[nodiscard]] inline ExprPtr simplify_div(const ExprPtr& l, const ExprPtr& r); // forward decl

[[nodiscard]] inline ExprPtr simplify_mul(const ExprPtr& l, const ExprPtr& r) {
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

[[nodiscard]] inline ExprPtr simplify_div(const ExprPtr& l, const ExprPtr& r) {
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
    // branch retained: needed by tests 505/512/526/6605/6642/9712 (literal -k denominator)
    if (is_neg_num(r))
        return Expr::BinOpExpr(BinOp::DIV, Expr::Neg(l), Expr::Num(-r->num));
    // T3.4: branch 2 (is_neg(l) && is_neg(r) -> l/r) deleted — subsumed by branches 3+4 firing in sequence
    // branch retained: needed by tests 8567/8576 (sum / Neg(x) cancellation)
    if (is_neg(r))
        return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, l, r->child));
    // branch retained: needed by tests 10480/11342 (Neg(l) / k pulled out for term cancellation)
    if (is_neg(l))
        return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, l->child, r));
    // not migratable: rational arithmetic; see Future.md typed-binding-predicates
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
        bool all_terms_divide_cleanly = true;
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
                all_terms_divide_cleanly = false; break;
            }
            divided.push_back(d);
        }
        if (all_terms_divide_cleanly && !divided.empty()) {
            ExprPtr result = divided[0];
            // not std::accumulate: skip-first; rewriting via std::next(begin, 1) is less readable than the indexed form
            for (size_t i = 1; i < divided.size(); i++)
                // cppcheck-suppress useStlAlgorithm
                result = Expr::BinOpExpr(BinOp::ADD, result, divided[i]);
            return result;
        }
    }

    // Cross-term cancellation: flatten both sides, cancel matching bases
    double l_scalar = 1.0, r_scalar = 1.0;
    std::vector<std::pair<ExprPtr, double>> lf, rf;
    flatten_multiplicative(l, l_scalar, lf);
    flatten_multiplicative(r, r_scalar, rf);
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
        auto top = rebuild_multiplicative(l_scalar, lf);
        auto bot = rebuild_multiplicative(r_scalar, rf);
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
[[nodiscard]] inline ExprPtr distribute_over_sum(const ExprPtr& e) {
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

// ============================================================
// Section: Linear algebra (vec_mat_matmul, vec_mat_inv, vec_mat_transpose, vec_mat_det, helpers)
// ============================================================

// ---- Vec/Mat predicates (M3) ----
//
// `vec`/`mat` are FUNC_CALL sugar (no new ExprType). The element-wise
// simplifier hook and the `matmul`/`det`/`inv`/`transpose` dispatchers
// share these predicates. See design-proposal.md §M3.
[[nodiscard]] inline bool is_vec(const ExprPtr& e) {
    return e && e->type == ExprType::FUNC_CALL && e->name == "vec";
}
[[nodiscard]] inline bool is_mat(const ExprPtr& e) {
    return e && e->type == ExprType::FUNC_CALL && e->name == "mat";
}

// Element-wise BINOP hook for vec/mat operands (M3 step 4). Returns nullptr
// when the operands aren't vec/mat-shaped — caller falls through to ordinary
// simplifier dispatch. Shape mismatch returns Var("undefined") (fwiz idiom).
//
// Cases handled:
//   ADD/SUB(vec, vec) → vec(args[i] op args'[i])  — same-arity required
//   ADD/SUB(mat, mat) → mat(rows[i] op rows'[i])  — same row count required;
//       the recursive simplify on each row pair re-enters this hook, so
//       column-count mismatch surfaces there.
//   MUL(Num, vec/mat) and MUL(vec/mat, Num) → element-wise scaled
//
// MUL(vec, vec) and MUL(mat, mat) are NOT handled here — those go through
// `matmul(...)` explicitly per design (keeps BinOp table small).
[[nodiscard]] inline ExprPtr simplify_once(const ExprPtr& e);  // forward decl

[[nodiscard]] inline ExprPtr try_simplify_vec_mat_binop(BinOp op, const ExprPtr& l, const ExprPtr& r) {
    const bool l_vm = is_vec(l) || is_mat(l);
    const bool r_vm = is_vec(r) || is_mat(r);
    if (!l_vm && !r_vm) return nullptr;
    // ADD / SUB: both must be same kind (vec+vec or mat+mat) and same arity.
    if (op == BinOp::ADD || op == BinOp::SUB) {
        if (!l_vm || !r_vm) return Expr::Var("undefined");
        if (l->name != r->name) return Expr::Var("undefined");
        if (l->args.size() != r->args.size()) return Expr::Var("undefined");
        std::vector<ExprPtr> out;
        out.reserve(l->args.size());
        for (size_t i = 0; i < l->args.size(); i++) {
            auto piece = simplify_once(Expr::BinOpExpr(op, l->args[i], r->args[i]));
            if (is_undefined(piece)) return piece;  // propagate row-level mismatch
            out.push_back(piece);
        }
        return Expr::Call(l->name, out);
    }
    // MUL: scalar (Num) on one side, vec/mat on the other → element-wise scale.
    if (op == BinOp::MUL) {
        ExprPtr scalar = nullptr, container = nullptr;
        if (is_num(l) && r_vm)      { scalar = l; container = r; }
        else if (is_num(r) && l_vm) { scalar = r; container = l; }
        else                         return nullptr;  // non-scalar mul; matmul() handles vec*vec/mat*mat
        std::vector<ExprPtr> out;
        out.reserve(container->args.size());
        for (auto a : container->args) {
            auto piece = simplify_once(Expr::BinOpExpr(BinOp::MUL, scalar, a));
            if (is_undefined(piece)) return piece;
            out.push_back(piece);
        }
        return Expr::Call(container->name, out);
    }
    return nullptr;  // SUB handled above; DIV/POW on vec/mat: not in M3 scope
}

// ---- Vec/Mat builtin handlers (M3 step 5) ----
//
// Multi-arg matrix functions: matmul, det, inv, transpose. Dispatched by name
// from the simplifier's FUNC_CALL branch when args are vec/mat-shaped. Each
// handler returns ExprPtr (matrix result, scalar result, or Var("undefined")
// for shape failures). All handlers preserve symbolic args — det of a matrix
// of VARs returns a symbolic SUB(MUL,MUL) tree, not a fold to NaN.
//
// Scope (per design §M3 reopen triggers):
//   det:       2x2 closed form, 3x3 cofactor expansion. >3x3 → undefined.
//   inv:       2x2 only.                                 Other shapes → undefined.
//   matmul:    arbitrary rectangular A (RxK) * B (KxC).
//   transpose: arbitrary rectangular matrix or row vector.
//
// Shape inspection uses the FUNC_CALL("mat", {vec, vec, ...}) layout. Each row
// is a vec FUNC_CALL whose args are scalars. A bare vec is 1xN. The helpers
// below normalize both to "rows of vec" for uniform indexing.

// Return rows as vector<vec-ExprPtr>. For mat: args directly; for vec: wrap
// the vec itself as the single row. Returns empty vector for non-vec/mat.
[[nodiscard]] inline std::vector<ExprPtr> mat_rows(const ExprPtr& m) {
    if (is_mat(m)) return m->args;
    if (is_vec(m)) return { m };
    return {};
}

// Return cols of a rectangular mat/vec. Empty if non-uniform or non-mat/vec.
[[nodiscard]] inline size_t mat_cols(const ExprPtr& m) {
    auto rows = mat_rows(m);
    if (rows.empty()) return 0;
    if (!is_vec(rows[0])) return 0;
    const size_t c = rows[0]->args.size();
    for (size_t i = 1; i < rows.size(); i++) {
        if (!is_vec(rows[i]) || rows[i]->args.size() != c) return 0;  // non-rectangular
    }
    return c;
}

// Get matrix element (i, j) with 0-based row/col. Asserts in-bounds.
[[nodiscard]] inline ExprPtr mat_at(const ExprPtr& m, size_t i, size_t j) {
    auto rows = mat_rows(m);
    assert(i < rows.size() && "mat_at: row index OOB");
    assert(is_vec(rows[i]) && j < rows[i]->args.size() && "mat_at: col index OOB");
    return rows[i]->args[j];
}

// Build vec(elems).
[[nodiscard]] inline ExprPtr make_vec(std::vector<ExprPtr> elems) {
    return Expr::Call("vec", std::move(elems));
}
// Build mat(rows). Each row must be a vec.
[[nodiscard]] inline ExprPtr make_mat(std::vector<ExprPtr> rows) {
    return Expr::Call("mat", std::move(rows));
}

// Matrix dimension constants — `e` is reserved for Euler's number, hence
// `en` is used for the (1,1) entry in the 3x3 cofactor expansion below.
constexpr size_t MATRIX_2X2_DIM = 2;
constexpr size_t MATRIX_3X3_DIM = 3;

// 2x2 cofactor / 3x3 cofactor expansion. Returns Var("undefined") for other shapes.
[[nodiscard]] inline ExprPtr vec_mat_det(const ExprPtr& m) {
    auto rows = mat_rows(m);
    const size_t n = rows.size();
    const size_t cols = mat_cols(m);
    if (n != cols || (n != MATRIX_2X2_DIM && n != MATRIX_3X3_DIM)) return Expr::Var("undefined");
    if (n == MATRIX_2X2_DIM) {
        // a*d - b*c
        auto a = mat_at(m, 0, 0); auto b = mat_at(m, 0, 1);
        auto c = mat_at(m, 1, 0); auto d = mat_at(m, 1, 1);
        return simplify(Expr::BinOpExpr(BinOp::SUB,
            Expr::BinOpExpr(BinOp::MUL, a, d),
            Expr::BinOpExpr(BinOp::MUL, b, c)));
    }
    // 3x3 cofactor: a*(en*k - f*h) - b*(d*k - f*g) + c*(d*h - en*g)
    // (`en` for the (1,1) entry: textbook `e` collides with builtin Euler's number.)
    auto a = mat_at(m, 0, 0); auto b = mat_at(m, 0, 1); auto c = mat_at(m, 0, 2);
    auto d = mat_at(m, 1, 0); auto en = mat_at(m, 1, 1); auto f = mat_at(m, 1, 2);
    auto g = mat_at(m, 2, 0); auto h = mat_at(m, 2, 1); auto k = mat_at(m, 2, 2);
    auto term = [](const ExprPtr& x, const ExprPtr& y, const ExprPtr& u, const ExprPtr& v) {
        return Expr::BinOpExpr(BinOp::SUB,
            Expr::BinOpExpr(BinOp::MUL, x, y),
            Expr::BinOpExpr(BinOp::MUL, u, v));
    };
    auto t1 = Expr::BinOpExpr(BinOp::MUL, a, term(en, k, f, h));
    auto t2 = Expr::BinOpExpr(BinOp::MUL, b, term(d, k, f, g));
    auto t3 = Expr::BinOpExpr(BinOp::MUL, c, term(d, h, en, g));
    return simplify(Expr::BinOpExpr(BinOp::ADD,
        Expr::BinOpExpr(BinOp::SUB, t1, t2), t3));
}

// 2x2 inverse only. Returns Var("undefined") for other shapes or singular dets.
[[nodiscard]] inline ExprPtr vec_mat_inv(const ExprPtr& m) {
    auto rows = mat_rows(m);
    if (rows.size() != MATRIX_2X2_DIM || mat_cols(m) != MATRIX_2X2_DIM) return Expr::Var("undefined");
    auto a = mat_at(m, 0, 0); auto b = mat_at(m, 0, 1);
    auto c = mat_at(m, 1, 0); auto d = mat_at(m, 1, 1);
    auto det = simplify(Expr::BinOpExpr(BinOp::SUB,
        Expr::BinOpExpr(BinOp::MUL, a, d),
        Expr::BinOpExpr(BinOp::MUL, b, c)));
    if (is_zero(det)) return Expr::Var("undefined");  // singular matrix
    auto scale = [&](const ExprPtr& v) {
        return simplify(Expr::BinOpExpr(BinOp::DIV, v, det));
    };
    auto neg = [](const ExprPtr& v) { return Expr::Neg(v); };
    auto row0 = make_vec({ scale(d),       scale(neg(b)) });
    auto row1 = make_vec({ scale(neg(c)),  scale(a)      });
    return simplify(make_mat({ row0, row1 }));
}

// Transpose any rectangular mat or row-vec. Vec input is treated as 1xN.
[[nodiscard]] inline ExprPtr vec_mat_transpose(const ExprPtr& m) {
    auto rows = mat_rows(m);
    const size_t n = rows.size();
    const size_t cols = mat_cols(m);
    if (n == 0 || cols == 0) return Expr::Var("undefined");
    std::vector<ExprPtr> out_rows;
    out_rows.reserve(cols);
    for (size_t j = 0; j < cols; j++) {
        std::vector<ExprPtr> row;
        row.reserve(n);
        for (size_t i = 0; i < n; i++) row.push_back(mat_at(m, i, j));
        out_rows.push_back(make_vec(std::move(row)));
    }
    return make_mat(std::move(out_rows));
}

// matmul(A, B): A is RxK, B is KxC → RxC. Inner-dim mismatch → undefined.
[[nodiscard]] inline ExprPtr vec_mat_matmul(const ExprPtr& a, const ExprPtr& b) {
    auto a_rows = mat_rows(a);
    auto b_rows = mat_rows(b);
    const size_t R = a_rows.size();
    const size_t K_a = mat_cols(a);
    const size_t K_b = b_rows.size();
    const size_t C = mat_cols(b);
    if (R == 0 || K_a == 0 || K_b == 0 || C == 0) return Expr::Var("undefined");
    if (K_a != K_b) return Expr::Var("undefined");
    std::vector<ExprPtr> out_rows;
    out_rows.reserve(R);
    for (size_t i = 0; i < R; i++) {
        std::vector<ExprPtr> row;
        row.reserve(C);
        for (size_t j = 0; j < C; j++) {
            // sum over k: A[i][k] * B[k][j]
            ExprPtr acc = nullptr;
            for (size_t k = 0; k < K_a; k++) {
                auto term = Expr::BinOpExpr(BinOp::MUL, mat_at(a, i, k), mat_at(b, k, j));
                acc = acc ? Expr::BinOpExpr(BinOp::ADD, acc, term) : term;
            }
            row.push_back(simplify(acc));
        }
        out_rows.push_back(make_vec(std::move(row)));
    }
    return make_mat(std::move(out_rows));
}

// Name dispatch hook for the simplifier's FUNC_CALL branch. Returns nullptr
// to fall through (unrecognized name or arg shape fails the precondition).
[[nodiscard]] inline ExprPtr try_dispatch_vec_mat_builtin(const std::string& name,
                                                         const std::vector<ExprPtr>& args) {
    auto matrixy = [](const ExprPtr& a) { return is_vec(a) || is_mat(a); };
    if (name == "matmul" && args.size() == 2 && matrixy(args[0]) && matrixy(args[1]))
        return vec_mat_matmul(args[0], args[1]);
    if (name == "det" && args.size() == 1 && matrixy(args[0]))
        return vec_mat_det(args[0]);
    if (name == "inv" && args.size() == 1 && matrixy(args[0]))
        return vec_mat_inv(args[0]);
    if (name == "transpose" && args.size() == 1 && matrixy(args[0]))
        return vec_mat_transpose(args[0]);
    return nullptr;
}

// ---- Simplify: main entry ----

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

            // Vec/Mat builtin dispatch (M3 step 5): matmul / det / inv / transpose.
            // Returns nullptr when name is unrecognized or args aren't vec/mat-shaped.
            if (auto m = try_dispatch_vec_mat_builtin(e->name, sa)) return m;

            // Function-specific rules migrated to BUILTIN_REWRITE_RULES
            return s;
        }

        case ExprType::BINOP: {
            auto l = simplify_once(e->left);
            auto r = simplify_once(e->right);
            if (is_undefined(l) || is_undefined(r)) return Expr::Var("undefined");
            if (is_num(l) && is_num(r))
                return evaluate_symbolic(*Expr::BinOpExpr(e->op, l, r));

            // Vec/Mat element-wise hook (M3): handles vec+vec / mat+mat ADD/SUB
            // (same arity required; mismatch → undefined) and scalar * vec/mat MUL.
            // Returns nullptr when neither operand is vec/mat — falls through to
            // the standard scalar dispatch below.
            if (auto hook = try_simplify_vec_mat_binop(e->op, l, r)) return hook;

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
                        const int64_t exp = static_cast<int64_t>(r->num);
                        int64_t rn = 1, rd = 1;
                        for (int64_t i = 0; i < exp; i++) { rn *= n; rd *= d; }
                        return make_rational(rn, rd);
                    }
                    // x^0, x^1, x^0.5, (x^a)^b, x^(-n) now in BUILTIN_REWRITE_RULES
                    // (T3.6 migrated via `is_neg_num(n)` predicate, Future #53)
                    return Expr::BinOpExpr(BinOp::POW, l, r);
                case BinOp::COUNT_: assert(false && "invalid BinOp"); break;
            }
            break;
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return e;
}

// Apply user-defined rewrite rules to a simplified expression.
[[nodiscard]] inline ExprPtr apply_rewrite_rules(const ExprPtr& e) {
    auto* rules = simplify_get_rewrite_rules();
    if (!rules) return e;
    auto* exhaustive_flags = simplify_rewrite_exhaustive_();
    for (auto& rule : *rules) {
        if (rule.is_undefined_branch) continue;  // skip: exists for exhaustiveness only
        auto bindings = match_pattern(rule.pattern, e);
        if (!bindings) continue;
        if (rule.condition.has_value()) {
            // Resolve bound expressions to numerics where possible, then check.
            auto* global_bindings = simplify_bindings_();
            std::map<std::string, double> numeric;
            for (auto& [var, expr] : *bindings) {
                if (is_num(expr)) {
                    numeric[var] = expr->num;
                } else if (is_var(expr) && global_bindings) {
                    auto it = global_bindings->find(expr->name);
                    if (it != global_bindings->end()) numeric[var] = it->second;
                }
            }
            if (!check_condition(*rule.condition, numeric, &*bindings,
                                 simplify_set_ctx_())) continue;
            const AssumptionSource source = (exhaustive_flags && rule.group_index >= 0
                && static_cast<size_t>(rule.group_index) < exhaustive_flags->size()
                && (*exhaustive_flags)[rule.group_index])
                ? AssumptionSource::Inherent : AssumptionSource::Derived;
            simplify_record_assumption(nullptr,
                condition_to_string(*rule.condition, *bindings), source);
        }
        return apply_rewrite(rule.replacement, *bindings);
    }
    return e;
}

[[nodiscard]] inline ExprPtr simplify_once(const ExprPtr& e) {
    return apply_rewrite_rules(simplify_once_impl(e));
}

[[nodiscard]] inline ExprPtr simplify(const ExprPtr& e) {
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

// ============================================================
// Section: Symbolic calculus (symbolic_diff, symbolic_integrate, BuiltinMeta, IBP, u-sub)
// ============================================================

// ============================================================================
//  Per-builtin metadata registry (Future #49) — shared by symbolic_diff
//  (chain rule) and symbolic_integrate (direct antiderivative table).
// ============================================================================
//
// `BuiltinMeta` collapses the dual if-chain duplication that previously lived
// inside `symbolic_diff`'s FUNC_CALL case (9 entries) and `symbolic_integrate`'s
// FUNC_CALL case (3 entries) into a single shared lookup table.
//
// Callback shapes (intentionally narrow — easy for future consumers to extend):
//   - `DiffFn(u) → f'(u)`: chain rule (`* du/dvar`) is applied at the call
//     site. Examples: `sin_diff(u) → cos(u)`, `log_diff(u) → 1/u`.
//   - `IntegrateFn(var) → ∫f(var) dvar`: caller verifies the FUNC_CALL's
//     argument is exactly `Var(var)` before invoking. Generalized arguments
//     (e.g. `sin(x^2)`) are u-substitution territory (`try_u_sub_integrate`)
//     and the IBP layer below.
//
// Future consumers (Future #7 units, #9 LaTeX, #53 typed-binding rules) plug
// in by adding fields to `BuiltinMeta`; the callbacks are currently free
// functions, but their signatures are stable across the in-tree extension axis.
//
// **Why C++ today, not `.fw` rules**: typed-binding predicates (`is_num(...)`,
// Future #53) are required to express the antiderivative table's pattern guards
// — `Var(var)^n iff is_num(n)`. `BuiltinMeta` is the **4th consumer** of #53
// (after T3.5, T3.6, integration Tier 1). Migration to `.fw` rules waits on #53.

struct BuiltinMeta {
    // Derivative form: returns derivative of f(u) w.r.t. u, where the chain
    // rule (multiplying by du/dvar) is applied at the call site.
    //
    // Raw function-pointer fields (no `using` alias) match the codebase
    // convention at expr.h:669+ for `builtin_functions()` `double(*)(double)`.
    // `using DiffFn = ExprPtr(*)(...)` triggers cppcheck internalAstError
    // (parser limitation on alias-of-function-pointer); inline declaration
    // sidesteps it.
    ExprPtr (*diff)(ExprPtr u);

    // Antiderivative form: returns ∫f(var) dvar where the argument MUST be
    // exactly Var(var). The caller verifies u == Var(var) before invoking;
    // if u is a more general expression, u-substitution layer
    // (`try_u_sub_integrate`) handles it. nullptr signals "no table entry —
    // try IBP or fall through to unevaluated."
    ExprPtr (*integrate)(const std::string& var);

    // Dimension propagation callback (gen-5 cycle 3c, Future #7b FULL).
    // Nullable: nullptr = "treat this FUNC_CALL result as dimensionless" — the
    // safe default for transcendental builtins (trig/log/exp) whose argument is
    // required to be dimensionless anyway. Contract: `arg` is the computed dim
    // of the first argument (nullopt = upstream mismatch sentinel). Returns
    // nullopt to propagate that sentinel; {} for dimensionless output; a
    // non-empty DimMap for dimension-transforming builtins (sqrt halves, abs
    // passes through). Read only by compute_dim's FUNC_CALL branch.
    std::optional<DimMap> (*dim_propagate)(const std::optional<DimMap>& arg) = nullptr;
};

// ---- Per-builtin diff helpers (chain rule applied at caller) ----
inline ExprPtr sin_diff(ExprPtr u)  { return Expr::Call("cos", {u}); }
inline ExprPtr cos_diff(ExprPtr u)  { return Expr::Neg(Expr::Call("sin", {u})); }
inline ExprPtr tan_diff(ExprPtr u)  {
    // 1 + tan(u)^2  (equivalent to sec(u)^2; avoids introducing a sec builtin)
    return Expr::BinOpExpr(BinOp::ADD, Expr::Num(1),
        Expr::BinOpExpr(BinOp::POW, Expr::Call("tan", {u}), Expr::Num(2)));
}
inline ExprPtr asin_diff(ExprPtr u) {
    // 1 / sqrt(1 - u^2)
    return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1),
        Expr::Call("sqrt", {Expr::BinOpExpr(BinOp::SUB, Expr::Num(1),
            Expr::BinOpExpr(BinOp::POW, u, Expr::Num(2)))}));
}
inline ExprPtr acos_diff(ExprPtr u) {
    // -1 / sqrt(1 - u^2)
    return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, Expr::Num(1),
        Expr::Call("sqrt", {Expr::BinOpExpr(BinOp::SUB, Expr::Num(1),
            Expr::BinOpExpr(BinOp::POW, u, Expr::Num(2)))})));
}
inline ExprPtr atan_diff(ExprPtr u) {
    // 1 / (u^2 + 1)
    return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1),
        Expr::BinOpExpr(BinOp::ADD, Expr::Num(1),
            Expr::BinOpExpr(BinOp::POW, u, Expr::Num(2))));
}
inline ExprPtr log_diff(ExprPtr u)  { return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), u); }
inline ExprPtr sqrt_diff(ExprPtr u) {
    // 1 / (2 * sqrt(u))
    return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1),
        Expr::BinOpExpr(BinOp::MUL, Expr::Num(2), Expr::Call("sqrt", {u})));
}
inline ExprPtr abs_diff(ExprPtr u)  {
    // abs(u)/u — the simplifier rewrites this to sign(u) via BUILTIN_REWRITE_RULES.
    return Expr::BinOpExpr(BinOp::DIV, Expr::Call("abs", {u}), u);
}

// ---- Per-builtin antiderivative helpers (caller checks arg == Var(var)) ----
inline ExprPtr sin_integrate(const std::string& var) {
    return Expr::Neg(Expr::Call("cos", {Expr::Var(var)}));
}
inline ExprPtr cos_integrate(const std::string& var) {
    return Expr::Call("sin", {Expr::Var(var)});
}
inline ExprPtr tan_integrate(const std::string& var) {
    // -log(cos(x))
    return Expr::Neg(Expr::Call("log", {Expr::Call("cos", {Expr::Var(var)})}));
}

// ---- Per-builtin dimension-propagation helpers (gen-5 cycle 3c) ----
inline std::optional<DimMap> sqrt_dim_propagate(const std::optional<DimMap>& arg) {
    if (!arg) return std::nullopt;     // propagate upstream mismatch sentinel
    DimMap result;
    for (const auto& [k, v] : *arg) {
        if (v % 2 != 0) return DimMap{};  // odd exponent: not a clean root → dimensionless
        result[k] = v / 2;
    }
    return result;
}
inline std::optional<DimMap> abs_dim_propagate(const std::optional<DimMap>& arg) {
    return arg;                        // abs: identity on dimension (and on sentinel)
}

inline const std::map<std::string, BuiltinMeta>& builtin_meta() {
    // static const: std::map runtime-init, not constexpr-able in C++17
    static const std::map<std::string, BuiltinMeta> registry = {
        {"sin",  {sin_diff,  sin_integrate, nullptr}},
        {"cos",  {cos_diff,  cos_integrate, nullptr}},
        {"tan",  {tan_diff,  tan_integrate, nullptr}},
        {"asin", {asin_diff, nullptr, nullptr}},   // antiderivative needs IBP
        {"acos", {acos_diff, nullptr, nullptr}},   // antiderivative needs IBP
        {"atan", {atan_diff, nullptr, nullptr}},   // antiderivative needs IBP
        {"log",  {log_diff,  nullptr, nullptr}},   // antiderivative needs IBP (x*log(x) - x)
        {"sqrt", {sqrt_diff, nullptr, sqrt_dim_propagate}},  // dim: halve exponents
        {"abs",  {abs_diff,  nullptr, abs_dim_propagate}},   // dim: passthrough; antideriv deferred
    };
    return registry;
}

// ============================================================================
//  Dimension propagation (gen-5 cycle 3c, Future #7b FULL)
// ============================================================================
//
// compute_dim folds an expression tree into a DimMap exponent algebra, given a
// type_map of per-variable dimensions. It is a bespoke fold (not tree_map):
// the accumulator is a DimMap, not an ExprPtr, and ADD/SUB needs to compare
// sibling shapes. Returns:
//   - {}      for dimensionless nodes (Num, unbound Var, symbolic-exponent
//             POW, transcendental FUNC_CALL).
//   - {d:n}   for dimensioned nodes, composed through MUL/DIV/POW/sqrt.
//   - nullopt as a sentinel when an ADD/SUB node has dimensionally-mismatched
//             operands (kg + s) — the one BLOCKING invariant of #7b FULL.
//             Consumed ONLY by check_condition's DIM_SECTION arm (→ false).
// Per critic cut: no cerr warning here (simplifier hot loop would spam it);
// the nullopt sentinel IS the detection.
[[nodiscard]] inline std::optional<DimMap>
compute_dim(const Expr& e, const std::map<std::string, BindingType>& type_map) {
    switch (e.type) {
        case ExprType::NUM:
            return DimMap{};
        case ExprType::VAR: {
            auto it = type_map.find(e.name);
            return it == type_map.end() ? DimMap{} : it->second.dim;
        }
        case ExprType::UNARY_NEG:
            return e.child ? compute_dim(*e.child, type_map) : DimMap{};
        case ExprType::BINOP: {
            switch (e.op) {
                case BinOp::MUL: {
                    auto a = compute_dim(*e.left, type_map);
                    auto b = compute_dim(*e.right, type_map);
                    if (!a || !b) return std::nullopt;
                    return dim_merge_add(*a, *b);
                }
                case BinOp::DIV: {
                    auto a = compute_dim(*e.left, type_map);
                    auto b = compute_dim(*e.right, type_map);
                    if (!a || !b) return std::nullopt;
                    return dim_merge_sub(*a, *b);
                }
                case BinOp::POW: {
                    // Integer-exponent only (critic cut #4): non-integer or
                    // symbolic exponents → dimensionless. sqrt routes through
                    // dim_propagate, not POW, so m^(1/2) is intentionally {}.
                    if (!is_num(e.right)) return DimMap{};
                    const double n = e.right->num;
                    // is_integer_value bounds at < 1e15 (safely inside INT_MAX),
                    // so the narrowing cast below cannot overflow (UB guard).
                    if (!is_integer_value(n)) return DimMap{};
                    const int ni = static_cast<int>(n);
                    auto a = compute_dim(*e.left, type_map);
                    if (!a) return std::nullopt;
                    return dim_scale(*a, ni);
                }
                case BinOp::ADD:
                case BinOp::SUB: {
                    auto a = compute_dim(*e.left, type_map);
                    auto b = compute_dim(*e.right, type_map);
                    if (!a || !b) return std::nullopt;        // propagate sentinel
                    if (*a == *b) return *a;                   // matching dims OK
                    return std::nullopt;                       // mismatch sentinel
                }
                case BinOp::COUNT_:
                    assert(false && "BinOp::COUNT_ unreachable in compute_dim");
                    return DimMap{};
            }
            return DimMap{}; // exhaustive switch fall-through guard
        }
        case ExprType::FUNC_CALL: {
            const auto& reg = builtin_meta();
            auto mit = reg.find(e.name);
            if (mit == reg.end() || mit->second.dim_propagate == nullptr || e.args.empty())
                return DimMap{};  // unknown/no-callback/no-arg builtin → dimensionless
            auto arg = compute_dim(*e.args[0], type_map);
            return mit->second.dim_propagate(arg);
        }
        case ExprType::COUNT_:
            assert(false && "ExprType::COUNT_ unreachable in compute_dim");
            return DimMap{};
    }
    return DimMap{}; // exhaustive switch fall-through guard
}

// ============================================================================
//  Symbolic differentiation (Future #6)
// ============================================================================
//
// `symbolic_diff(e, var)` produces d(e)/d(var) as a fresh ExprPtr in the
// active arena. Per-AST-class switch + `BuiltinMeta` registry lookup for
// FUNC_CALL (Future #49 — registry extracted M3, 2026-05-10).
//
// Design choices (see .fwiz-workflow/design-proposal.md "Final Design"):
//   - Plain factories (no smart-builders): `simplify` already folds 0+x, 0*x,
//     1*x, x/1, etc. — pre-folding here is dead weight on a non-hot path.
//   - Per-builtin metadata via `builtin_meta()` registry — second consumer
//     (`symbolic_integrate`) shares the same table. Future #7 (units), #9
//     (LaTeX) plug in by extending `BuiltinMeta`.
//   - Returns `nullptr` for unrecognized forms (multi-arg builtins, unknown
//     function names). Callers (`symbolic_diff_simplified`, the post-load
//     resolution pass in system.h) treat null as a domain failure.
//
// `abs(x)` differentiates to `abs(x)/x`, which the simplifier rewrites to
// `sign(x)` via the `BUILTIN_REWRITE_RULES` (`abs(x)/x = sign(x) iff x != 0`
// and `abs(x)/x = undefined iff x = 0`). Empirical preflight confirmed the
// existing `x/x` matcher does NOT fire structurally on `abs(x)/x`.
[[nodiscard]] inline ExprPtr symbolic_diff(const Expr& e, const std::string& var) {
    using E = Expr;
    switch (e.type) {
        case ExprType::NUM:
            return E::Num(0);

        case ExprType::VAR:
            return E::Num(e.name == var ? 1 : 0);

        case ExprType::UNARY_NEG: {
            auto dc = symbolic_diff(*e.child, var);
            return dc ? E::Neg(dc) : nullptr;
        }

        case ExprType::BINOP: {
            auto l = e.left, r = e.right;
            auto dl = symbolic_diff(*l, var);
            auto dr = symbolic_diff(*r, var);
            if (!dl || !dr) return nullptr;
            switch (e.op) {
                case BinOp::ADD:
                case BinOp::SUB:
                    return E::BinOpExpr(e.op, dl, dr);
                case BinOp::MUL:
                    // (l*r)' = l'*r + l*r'
                    return E::BinOpExpr(BinOp::ADD,
                        E::BinOpExpr(BinOp::MUL, dl, r),
                        E::BinOpExpr(BinOp::MUL, l, dr));
                case BinOp::DIV:
                    // (l/r)' = (l'*r - l*r') / r^2
                    return E::BinOpExpr(BinOp::DIV,
                        E::BinOpExpr(BinOp::SUB,
                            E::BinOpExpr(BinOp::MUL, dl, r),
                            E::BinOpExpr(BinOp::MUL, l, dr)),
                        E::BinOpExpr(BinOp::POW, r, E::Num(2)));
                case BinOp::POW: {
                    // (l^r)' = l^r * (r' * log(l) + r * l' / l)
                    auto pow_expr = E::BinOpExpr(BinOp::POW, l, r);
                    auto log_l    = E::Call("log", {l});
                    auto term1    = E::BinOpExpr(BinOp::MUL, dr, log_l);
                    auto term2    = E::BinOpExpr(BinOp::DIV,
                        E::BinOpExpr(BinOp::MUL, r, dl), l);
                    return E::BinOpExpr(BinOp::MUL, pow_expr,
                        E::BinOpExpr(BinOp::ADD, term1, term2));
                }
                case BinOp::COUNT_: assert(false && "invalid BinOp"); break;
            }
            return nullptr;
        }

        case ExprType::FUNC_CALL: {
            // Only single-arg builtins are differentiable in v1.
            if (e.args.size() != 1) return nullptr;
            auto u  = e.args[0];
            auto du = symbolic_diff(*u, var);
            if (!du) return nullptr;
            // Registry lookup (Future #49): per-builtin derivative table.
            // The callback returns f'(u); chain rule (`* du`) is applied here.
            const auto& reg = builtin_meta();
            const auto it = reg.find(e.name);
            if (it == reg.end() || it->second.diff == nullptr) return nullptr;
            const ExprPtr fp = it->second.diff(u);
            return E::BinOpExpr(BinOp::MUL, fp, du);
        }

        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return nullptr;
}

// Convenience wrapper: differentiate then simplify. Returns nullptr if the
// underlying `symbolic_diff` returns nullptr (signal for callers).
[[nodiscard]] inline ExprPtr symbolic_diff_simplified(const Expr& e, const std::string& var) {
    auto raw = symbolic_diff(e, var);
    return raw ? simplify(raw) : nullptr;
}

// ============================================================================
//  Symbolic integration (Future #16, M1 — indefinite Tier 1; M2 — u-sub + definite)
// ============================================================================
//
// Numeric counterpart: `adaptive_simpson` (defined later in §Numerical solvers,
// near `newton_solve`) — the definite-integral fallback when symbolic_integrate
// returns nullptr. Dispatch lives in `resolve_integral_calls` (system.h).
//
// `symbolic_integrate(e, var)` produces ∫e d(var) as a fresh ExprPtr in the
// active arena. Per-AST-class switch + per-builtin if-chain for FUNC_CALL.
// Mirrors `symbolic_diff` exactly: same return-on-`nullptr`-on-miss contract.
//
// Scope (Tier 1, ~25 atomic patterns):
//   - Constants and VAR (linearity-trivial cases).
//   - ADD/SUB linearity.
//   - MUL: `c * f` where `c` is constant w.r.t. var; M2 derivative-divides u-sub.
//   - DIV: `f / c` (constant denom), `c / x` (constant over var), `1 / x`.
//   - POW: `Var(var)^n` (power rule, n ≠ -1), `Var(var)^(-1) → log`,
//     `e^Var(var)` (exponential).
//   - UNARY_NEG: integrate child, negate.
//   - FUNC_CALL: sin, cos, tan only when arg == Var(var).
//
// Returns `nullptr` for any form outside this list. Callers (the post-load
// `resolve_integral_in_equations` pass) treat null as "preserve the original
// integral(...) call symbolic" — same convention as diff.
//
// M2 additions (this cycle):
//   - Derivative-divides u-substitution in MUL: enumerate candidate g(x)
//     subexpressions, compute g'(x), cancel g' out of the integrand, integrate
//     the residual w.r.t. u, back-substitute g for u.
//   - Definite integral 4-arg form `integral(f, x, a, b)` is dispatched by
//     `resolve_integral_calls` in system.h — symbolic F(b)-F(a) primary,
//     `adaptive_simpson` fallback (defined below alongside `newton_solve`).
//
// M3 additions (this cycle):
//   - Integration by parts via LIATE heuristic (`try_ibp_integrate`). Depth
//     limit ≤ 3 enforced via thread-local counter (no cyclic detection — the
//     `e^x*sin(x)` family returns unevaluated by design, captured by the depth
//     guard). LIATE priority: Logarithmic > Inverse-trig > Algebraic >
//     Trigonometric > Exponential. When integrating MUL(u, dv), the operand
//     with HIGHER LIATE rank becomes `u`; the other is `dv`. Single FUNC_CALL
//     integrands at L/I rank (e.g. `atan(x)`, `log(x)`) without an
//     antiderivative-table entry get treated as `f(x) * 1` for IBP.
//   - `BuiltinMeta` registry extracted (Future #49) — diff and integrate share
//     the same per-builtin metadata table. See `builtin_meta()` above.
//
// Out of scope:
//   - Cyclic IBP detection (`e^x * sin(x)` family — depth limit catches it).
//     Reopen trigger: see master-plan.md cross-arc reopen triggers.
//   - `+ C` constant of integration — never (would not round-trip).
//   - Domain-aware antiderivative (`log(abs(x))`) — gated on Future #31.

// `try_cancel(expr, factor)` — symbolic division with a cancellation check.
// Computes `simplify(DIV(expr, factor))` then walks the result to verify the
// `factor` subtree no longer appears anywhere; returns the quotient if so,
// nullptr otherwise. Heuristic — perfect cancellation is hard. This catches
// the "obvious factor matches" case used by derivative-divides u-sub.
[[nodiscard]] inline ExprPtr try_cancel(const ExprPtr& expr, const ExprPtr& factor) {
    if (!expr || !factor || is_zero(factor)) return nullptr;
    auto quotient = simplify(Expr::BinOpExpr(BinOp::DIV, expr, factor));
    if (!quotient) return nullptr;
    // Walk quotient checking no subtree equals factor structurally. Pure
    // pass-through for atomic NUM/VAR; recurses through BINOP/UNARY_NEG/
    // FUNC_CALL children. Early-exits on first match via the `factor_remains`
    // flag. Local struct keeps recursion stack-allocated (no heap via
    // std::function type erasure).
    struct FactorWalker {
        // const Expr* (pointee not mutated): factor identity captured for expr_equal compare
        const Expr* factor;
        bool factor_remains = false;
        void operator()(const Expr* n) {
            if (factor_remains || !n) return;
            if (expr_equal(*n, *factor)) { factor_remains = true; return; }
            switch (n->type) {
                case ExprType::NUM:
                case ExprType::VAR:
                    break;
                case ExprType::BINOP:
                    (*this)(n->left); (*this)(n->right); break;
                case ExprType::UNARY_NEG:
                    (*this)(n->child); break;
                case ExprType::FUNC_CALL:
                    for (const auto* a : n->args) (*this)(a);
                    break;
                case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
            }
        }
    };
    FactorWalker walker{factor, false};
    walker(quotient);
    return walker.factor_remains ? nullptr : quotient;
}

// Forward declarations: helpers called from `symbolic_integrate`'s MUL /
// FUNC_CALL cases. Defined below.
[[nodiscard]] inline ExprPtr try_u_sub_integrate(const Expr& e, const std::string& var);
[[nodiscard]] inline ExprPtr try_ibp_integrate(const Expr& e, const std::string& var);
[[nodiscard]] inline ExprPtr canonicalize_ibp_product(
    const ExprPtr& u, const ExprPtr& V, const std::string& var);

// LIATE priority — used by `try_ibp_integrate` to choose `u` vs `dv` when
// applying integration by parts to a MUL integrand. Higher rank → preferred
// `u` (we want u to differentiate to something simpler; we want dv to have a
// known antiderivative).
//
// Pure-numeric and var-free constants get rank `Algebraic` so that `dv = 1`
// in `atan(x)*1`-style synthesised IBP entries is well-defined.
enum class LiateRank : int {
    None          = 0,  // does not qualify for LIATE
    Exponential   = 1,  // e^Var(var)
    Trigonometric = 2,  // sin / cos / tan
    Algebraic     = 3,  // x, x^n, c*x, c (var-free or numeric)
    InverseTrig   = 4,  // asin / acos / atan
    Logarithmic   = 5,  // log(...)
};
// Threshold at which a single FUNC_CALL with no antiderivative-table entry is
// promoted to a synthesised `f(x) * 1` IBP candidate (covers atan/asin/acos/log).
constexpr int LIATE_MIN_RANK_FOR_IBP_SYNTHESIS = static_cast<int>(LiateRank::InverseTrig);
// Pin LIATE_MIN_RANK_FOR_IBP_SYNTHESIS semantics: must be >= Trigonometric (2) to exclude
// pure-trig synthesis (which would loop); InverseTrig (4) is the documented floor.
static_assert(LIATE_MIN_RANK_FOR_IBP_SYNTHESIS >= static_cast<int>(LiateRank::Trigonometric),
              "LIATE_MIN_RANK_FOR_IBP_SYNTHESIS must exclude pure-trig synthesis");

[[nodiscard]] inline int liate_priority(const Expr& e, const std::string& var) {
    if (e.type == ExprType::FUNC_CALL && e.args.size() == 1) {
        if (e.name == "log") return static_cast<int>(LiateRank::Logarithmic);
        if (e.name == "asin" || e.name == "acos" || e.name == "atan")
            return static_cast<int>(LiateRank::InverseTrig);
        if (e.name == "sin"  || e.name == "cos"  || e.name == "tan")
            return static_cast<int>(LiateRank::Trigonometric);
    }
    if (e.type == ExprType::BINOP && e.op == BinOp::POW) {
        // e^Var(var) — exponential. Var("e") is the base.
        if (is_var(e.left)  && e.left->name == "e"
            && is_var(e.right) && e.right->name == var)
            return static_cast<int>(LiateRank::Exponential);
        // Var(var)^n with n constant — algebraic.
        if (is_var(e.left)  && e.left->name == var
            && !contains_var(*e.right, var))
            return static_cast<int>(LiateRank::Algebraic);
    }
    // Algebraic atoms / linear forms: Var(var), Num, var-free expressions, NEG of same.
    if (is_var(e) && e.name == var) return static_cast<int>(LiateRank::Algebraic);
    if (!contains_var(e, var))      return static_cast<int>(LiateRank::Algebraic);
    if (e.type == ExprType::UNARY_NEG && e.child)
        return liate_priority(*e.child, var);
    if (e.type == ExprType::BINOP && e.op == BinOp::MUL) {
        // c*x (one side var-free) — algebraic.
        const bool l_has = contains_var(*e.left, var);
        const bool r_has = contains_var(*e.right, var);
        if (!l_has && r_has) return liate_priority(*e.right, var);
        if (l_has && !r_has) return liate_priority(*e.left,  var);
    }
    return static_cast<int>(LiateRank::None);
}

[[nodiscard]] inline ExprPtr symbolic_integrate(const Expr& e, const std::string& var) {
    using E = Expr;
    switch (e.type) {
        case ExprType::NUM:
            // ∫c dx = c*x
            return E::BinOpExpr(BinOp::MUL, E::Num(e.num), E::Var(var));

        case ExprType::VAR:
            // ∫x dx = x^2/2;  ∫y dx = y*x  (y constant w.r.t. x)
            if (e.name == var)
                return E::BinOpExpr(BinOp::DIV,
                    E::BinOpExpr(BinOp::POW, E::Var(var), E::Num(2)),
                    E::Num(2));
            return E::BinOpExpr(BinOp::MUL, E::Var(e.name), E::Var(var));

        case ExprType::UNARY_NEG: {
            auto ic = symbolic_integrate(*e.child, var);
            return ic ? E::Neg(ic) : nullptr;
        }

        case ExprType::BINOP: {
            auto l = e.left, r = e.right;
            switch (e.op) {
                case BinOp::ADD:
                case BinOp::SUB: {
                    // Linearity: ∫(l ± r) = ∫l ± ∫r
                    auto il = symbolic_integrate(*l, var);
                    auto ir = symbolic_integrate(*r, var);
                    if (!il || !ir) return nullptr;
                    return E::BinOpExpr(e.op, il, ir);
                }
                case BinOp::MUL: {
                    // Constant-times-f (linearity over a numeric or var-free factor).
                    const bool l_has = contains_var(*l, var);
                    const bool r_has = contains_var(*r, var);
                    if (!l_has) {
                        auto ir = symbolic_integrate(*r, var);
                        return ir ? E::BinOpExpr(BinOp::MUL, l, ir) : nullptr;
                    }
                    if (!r_has) {
                        auto il = symbolic_integrate(*l, var);
                        return il ? E::BinOpExpr(BinOp::MUL, il, r) : nullptr;
                    }
                    // Both contain var — try derivative-divides u-substitution
                    // (M2) first, then integration by parts via LIATE (M3).
                    if (auto u_sub = try_u_sub_integrate(e, var)) return u_sub;
                    return try_ibp_integrate(e, var);
                }
                case BinOp::DIV: {
                    const bool l_has = contains_var(*l, var);
                    const bool r_has = contains_var(*r, var);
                    if (!r_has) {
                        // f / c → ∫f / c
                        auto il = symbolic_integrate(*l, var);
                        return il ? E::BinOpExpr(BinOp::DIV, il, r) : nullptr;
                    }
                    // c / x or c / Var(var) — log form. Only when r is exactly
                    // Var(var) and l is constant w.r.t. var.
                    if (!l_has && is_var(r) && r->name == var) {
                        if (is_one(*l)) return E::Call("log", {E::Var(var)});
                        return E::BinOpExpr(BinOp::MUL, l, E::Call("log", {E::Var(var)}));
                    }
                    // c / (k * Var(var)) — `c/k * log(Var(var))`. Reaches here from
                    // the IBP recursive `∫V*du` case for `atan(x)` (`1/(2*u)`).
                    if (!l_has && r->type == ExprType::BINOP && r->op == BinOp::MUL) {
                        const bool ll = contains_var(*r->left,  var);
                        const bool lr = contains_var(*r->right, var);
                        if (!ll && is_var(r->right) && r->right->name == var) {
                            // c / (k * x) = (c / k) * log(x)
                            return E::BinOpExpr(BinOp::MUL,
                                E::BinOpExpr(BinOp::DIV, l, r->left),
                                E::Call("log", {E::Var(var)}));
                        }
                        if (!lr && is_var(r->left) && r->left->name == var) {
                            // c / (x * k) = (c / k) * log(x)
                            return E::BinOpExpr(BinOp::MUL,
                                E::BinOpExpr(BinOp::DIV, l, r->right),
                                E::Call("log", {E::Var(var)}));
                        }
                    }
                    // Both numerator and denominator contain var — try u-sub
                    // (M3 path; reaches `∫x/(x^2+1) dx = log(x^2+1)/2` for atan IBP).
                    // r_has is true here (the !r_has branch returned above),
                    // so the surviving condition is just l_has.
                    if (l_has) {
                        return try_u_sub_integrate(e, var);
                    }
                    return nullptr;
                }
                case BinOp::POW: {
                    // Var(var)^n (n constant numeric, n != -1) → x^(n+1)/(n+1)
                    // The n == -1 branch is reachable only via the post-simplify()
                    // form `POW(Var(var), Num(-1))` (e.g. through unfold_formula_call_body
                    // whose body re-simplifies before reaching this dispatcher); the
                    // direct parse path `x^(-1)` produces POW(Var(var), UNARY_NEG(Num(1)))
                    // and falls through to nullptr → the DIV `1/x` branch above handles
                    // user-typed `1/x` directly.
                    if (is_var(l) && l->name == var && is_num(r)) {
                        if (is_neg_one(*r)) return E::Call("log", {E::Var(var)});
                        const double n = r->num;
                        return E::BinOpExpr(BinOp::DIV,
                            E::BinOpExpr(BinOp::POW, E::Var(var), E::Num(n + 1)),
                            E::Num(n + 1));
                    }
                    // e^Var(var) → e^Var(var)  (Var("e") base)
                    if (is_var(l) && l->name == "e" && is_var(r) && r->name == var) {
                        return E::BinOpExpr(BinOp::POW, E::Var("e"), E::Var(var));
                    }
                    return nullptr;
                }
                case BinOp::COUNT_: assert(false && "invalid BinOp"); break;
            }
            return nullptr;
        }

        case ExprType::FUNC_CALL: {
            // Single-arg builtins only, and only when arg == Var(var). The
            // chain-rule path (e.g. `sin(x^2)`) is u-substitution territory —
            // handled at the MUL branch via `try_u_sub_integrate`.
            if (e.args.size() != 1) return nullptr;
            const Expr* u = e.args[0];
            if (!is_var(u) || u->name != var) return nullptr;
            // Registry lookup (Future #49): per-builtin antiderivative table.
            const auto& reg = builtin_meta();
            const auto it = reg.find(e.name);
            if (it != reg.end() && it->second.integrate != nullptr)
                return it->second.integrate(var);
            // No table entry. If the function sits at L or I in LIATE
            // (rank ≥ LIATE_MIN_RANK_FOR_IBP_SYNTHESIS), synthesise `f(x) * 1`
            // and let IBP handle it (`atan(x)`, `log(x)`, `asin(x)`, `acos(x)`).
            // Trig builtins (rank Trigonometric) all have table entries; the
            // synthesised path is unreachable for them.
            if (liate_priority(e, var) >= LIATE_MIN_RANK_FOR_IBP_SYNTHESIS) {
                // const_cast: arena nodes are mutably owned even when held through
                // const Expr&; the substitute path doesn't mutate.
                const auto* const product = E::BinOpExpr(BinOp::MUL, const_cast<Expr*>(&e), E::Num(1));
                return try_ibp_integrate(*product, var);
            }
            return nullptr;
        }

        case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
    }
    return nullptr;
}

// Convenience wrapper: integrate then simplify. Returns nullptr if the
// underlying `symbolic_integrate` returns nullptr (signal for callers).
[[nodiscard]] inline ExprPtr symbolic_integrate_simplified(const Expr& e, const std::string& var) {
    auto raw = symbolic_integrate(e, var);
    return raw ? simplify(raw) : nullptr;
}

// Derivative-divides u-substitution (M2). For an integrand `e` (typically a
// product where both factors mention `var`), enumerate candidate sub-expressions
// `g` of `e` to a bounded depth, compute `g' = symbolic_diff(g, var)`, attempt
// to cancel `g'` out of `e`, and — if the cancelled residual is expressible as
// a function of `g` alone (no direct `var` outside `g`) — integrate w.r.t.
// `u = g` and back-substitute. Returns nullptr when no candidate works.
//
// Depth bound: we descend at most U_SUB_DEPTH levels into the integrand to
// gather candidates, which keeps cost O(small) for the typical 2-3-factor
// product. Linear `g = c*x` cases are caught when `g` is a sub-expression of
// the integrand (e.g. `cos(2*x)` has `2*x` as a candidate via the FUNC_CALL
// arg path).
[[nodiscard]] inline ExprPtr try_u_sub_integrate(const Expr& e, const std::string& var) {
    constexpr int U_SUB_DEPTH = 2;

    // Collect distinct candidate subexpressions to a bounded depth. Excludes
    // pure-numeric and var-free nodes (g' = 0 is unhelpful) and the trivial
    // `Var(var)` case (g' = 1 gives back the original integrand). Skip the
    // root `&e` itself — picking g = e produces a pathological "cancel
    // against my own derivative" loop and leaves `log(e)` artifacts via the
    // general POW-derivative rule for `e^...` integrands.
    // gather: root's children visited at d = U_SUB_DEPTH - 1; grandchildren at d = U_SUB_DEPTH - 2.
    // Local struct (vs std::function) keeps the recursion stack-allocated.
    std::vector<ExprPtr> candidates;
    struct Gatherer {
        const std::string& var;
        std::vector<ExprPtr>& candidates;
        void operator()(const Expr* n, int d, bool is_root) {
            if (!n || d < 0) return;
            if (!is_root && contains_var(*n, var) && !(is_var(*n) && n->name == var)) {
                const bool dup = std::any_of(candidates.begin(), candidates.end(),
                    [&](const ExprPtr& c) { return expr_equal(*c, *n); });
                if (!dup) candidates.push_back(const_cast<Expr*>(n));
            }
            switch (n->type) {
                case ExprType::NUM: case ExprType::VAR: break;
                case ExprType::BINOP:
                    (*this)(n->left, d - 1, false); (*this)(n->right, d - 1, false); break;
                case ExprType::UNARY_NEG:
                    (*this)(n->child, d - 1, false); break;
                case ExprType::FUNC_CALL:
                    for (const auto* a : n->args) (*this)(a, d - 1, false);
                    break;
                case ExprType::COUNT_: assert(false && "invalid ExprType"); break;
            }
        }
    };
    Gatherer gather{var, candidates};
    gather(&e, U_SUB_DEPTH, true);

    // Sort candidates ascending by leaf count — try the simplest g first.
    // For `x*e^(x^2)` this picks `x^2` (3 leaves) before `e^(x^2)` (5 leaves),
    // avoiding the chain-rule diff-of-`e^...` that produces a log(e) artifact.
    std::sort(candidates.begin(), candidates.end(),
        [](const ExprPtr& a, const ExprPtr& b) {
            return canonicity_score(a) < canonicity_score(b);
        });

    // Use a placeholder name unlikely to collide with user vars. The literal
    // string is contained to this function; back-substitution restores `g`.
    const std::string u_name = "_u_sub_";

    ExprPtr e_ptr = const_cast<Expr*>(&e);
    for (const auto& g : candidates) {
        auto g_prime = symbolic_diff_simplified(*g, var);
        if (!g_prime || is_zero(g_prime)) continue;
        auto residual = try_cancel(e_ptr, g_prime);
        if (!residual) continue;
        // Express residual in terms of u — replace each g-subtree with Var(u_name).
        const ExprPtr residual_in_u = replace_subtree_by_name(residual, {{u_name, g}});
        // If residual still references `var` directly, this candidate fails:
        // u-sub requires the integrand reduce to f(u) du.
        if (contains_var(*residual_in_u, var)) continue;
        auto antideriv_in_u = symbolic_integrate(*residual_in_u, u_name);
        if (!antideriv_in_u) continue;
        // Back-substitute: replace Var(u_name) with g.
        auto result = substitute(antideriv_in_u, u_name, g);
        return simplify(result);
    }
    return nullptr;
}

// Integration by parts via LIATE (M3). Called from `symbolic_integrate`'s MUL
// branch (after u-sub fails) and synthesised from a single FUNC_CALL at L/I
// rank with no antiderivative-table entry (`atan(x)`, `log(x)`, etc.).
//
// Algorithm (∫u dv = u*V - ∫V*du):
//   1. From the MUL operands, pick `u` = side with HIGHER LIATE rank (and
//      rank > 0); the other becomes `dv`. If neither operand qualifies (both
//      rank 0, or rank-tie), return nullptr.
//   2. Compute `V = symbolic_integrate(dv, var)`. If null, return nullptr.
//   3. Compute `du = symbolic_diff_simplified(u, var)`. If null, return nullptr.
//   4. Recursively integrate `V * du`. If that returns null, return nullptr —
//      no closed-form result. (Cyclic IBP cases like `e^x * sin(x)` blow the
//      depth limit on the recursive call and bail out here.)
//   5. Result: simplify(u*V - ∫V*du).
//
// Depth bound (≤ 3) is enforced via a thread-local counter — the recursion can
// pass through `symbolic_integrate` and back, so an in-band parameter would
// require widening every layer's signature. `IBP_MAX_DEPTH = 3` matches the
// design's stated bound.
constexpr int IBP_MAX_DEPTH = 3;
static_assert(IBP_MAX_DEPTH >= 1 && IBP_MAX_DEPTH <= 10);
inline thread_local int ibp_depth_ = 0;

// Helper: build `a * b` while preserving a structural `DIV(num, denom)` factor.
// When one side is a DIV, returns `DIV(MUL(num_of_div_side, other), denom)`;
// when both are DIVs, combines numerators and denominators. This avoids the
// simplifier prematurely flattening `(x^3/3) * (1/x)` into `0.333 * x^2` (the
// rebuilder collapses the rational coefficient to a double); the `(x^3 * 1) /
// (3 * x)` form simplifies cleanly to `x^2 / 3`. The numerator-of-DIV always
// renders FIRST in the resulting MUL — this keeps the algebraic dv-derived
// term ahead of the function `u` in IBP results (e.g., `x^3 * log(x) / 3`).
[[nodiscard]] inline ExprPtr mul_through_div(const ExprPtr& a, const ExprPtr& b) {
    const bool a_is_div = a && a->type == ExprType::BINOP && a->op == BinOp::DIV;
    const bool b_is_div = b && b->type == ExprType::BINOP && b->op == BinOp::DIV;
    if (a_is_div && b_is_div) {
        return Expr::BinOpExpr(BinOp::DIV,
            Expr::BinOpExpr(BinOp::MUL, a->left, b->left),
            Expr::BinOpExpr(BinOp::MUL, a->right, b->right));
    }
    if (a_is_div) {
        return Expr::BinOpExpr(BinOp::DIV,
            Expr::BinOpExpr(BinOp::MUL, a->left, b), a->right);
    }
    if (b_is_div) {
        return Expr::BinOpExpr(BinOp::DIV,
            Expr::BinOpExpr(BinOp::MUL, a, b->left), b->right);
    }
    return Expr::BinOpExpr(BinOp::MUL, a, b);
}

// Canonical operand order for the u*V term in integration-by-parts output.
// IBP produces u*V - ∫V*du; the operand order of u*V matters for
// downstream rendering (the simplifier respects the syntactic order):
//   - V is a DIV (e.g. dv = x^2 → V = x^3/3): build `V_num * u / V_denom`
//     so the algebraic dv-derived term renders before the function `u`,
//     and the simplifier preserves the structural fraction (avoids
//     `0.333 * log(x) * x^3`).
//   - V is exactly `Var(var)` (e.g. dv = 1 → V = x), and u is a FUNC_CALL:
//     emit `V * u` (algebraic before function — `x * atan(x)`).
//   - Otherwise plain `MUL(u, V)` — for `x*e^x` (u=x, V=e^x) yields
//     `x * e^x` not `e^x * x`.
[[nodiscard]] inline ExprPtr canonicalize_ibp_product(
    const ExprPtr& u, const ExprPtr& V, const std::string& var) {
    const bool V_is_div  = V && V->type == ExprType::BINOP && V->op == BinOp::DIV;
    const bool V_is_var  = V && is_var(*V) && V->name == var;
    const bool u_is_call = u && u->type == ExprType::FUNC_CALL;
    if (V_is_div)              return mul_through_div(V, u);
    if (V_is_var && u_is_call) return Expr::BinOpExpr(BinOp::MUL, V, u);
    return Expr::BinOpExpr(BinOp::MUL, u, V);
}

[[nodiscard]] inline ExprPtr try_ibp_integrate(const Expr& e, const std::string& var) {
    if (e.type != ExprType::BINOP || e.op != BinOp::MUL) return nullptr;
    if (ibp_depth_ >= IBP_MAX_DEPTH) return nullptr;

    // Pick u (higher LIATE rank), dv (the other operand). Bail if neither
    // qualifies or ranks tie (no LIATE preference).
    const ExprPtr lhs = e.left;
    const ExprPtr rhs = e.right;
    const int rl = liate_priority(*lhs, var);
    const int rr = liate_priority(*rhs, var);
    if (rl == 0 && rr == 0) return nullptr;
    if (rl == rr) return nullptr;
    const ExprPtr u  = (rl > rr) ? lhs : rhs;
    const ExprPtr dv = (rl > rr) ? rhs : lhs;

    // V = ∫dv dx
    ExprPtr V = symbolic_integrate(*dv, var);
    if (!V) return nullptr;
    V = simplify(V);

    // du = d(u)/dx
    ExprPtr du = symbolic_diff_simplified(*u, var);
    if (!du) return nullptr;

    // ∫V*du dx — recursive. Increment depth around the call to bound recursion.
    // Build V*du via `mul_through_div` to preserve structural fractions;
    // canonical-render dv's numerator (algebraic, e.g. x^3) FIRST so the
    // simplifier emits `x^3 * <other> / 3` rather than `<other> * x^3 / 3`.
    const ExprPtr V_du = simplify(mul_through_div(V, du));
    ExprPtr int_V_du = nullptr;
    {
        ++ibp_depth_;
        int_V_du = symbolic_integrate(*V_du, var);
        --ibp_depth_;
    }
    if (!int_V_du) return nullptr;

    // u*V - ∫V*du
    const ExprPtr u_V        = canonicalize_ibp_product(u, V, var);
    const ExprPtr result_raw = Expr::BinOpExpr(BinOp::SUB, u_V, int_V_du);
    return simplify(result_raw);
}

// ============================================================================
//  Linear solver: decompose expr into coeff * target + rest
// ============================================================================

struct LinearForm { ExprPtr coeff, rest; };

[[nodiscard]] inline std::optional<LinearForm> decompose_linear(const ExprPtr& e, const std::string& t) {
    if (!e) return LinearForm{Expr::Num(0), Expr::Num(0)};

    auto ok = [](ExprPtr c, ExprPtr r) -> std::optional<LinearForm> { return LinearForm{c, r}; };
    auto fail = []() -> std::optional<LinearForm> { return std::nullopt; };

    switch (e->type) {
        case ExprType::NUM:
            return ok(Expr::Num(0), e);

        case ExprType::VAR:
            return (e->name == t) ? ok(Expr::Num(1), Expr::Num(0))
                                  : ok(Expr::Num(0), e);

        case ExprType::UNARY_NEG: {
            auto d = decompose_linear(e->child, t);
            return d ? ok(simplify(Expr::Neg(d->coeff)), simplify(Expr::Neg(d->rest))) : fail();
        }

        case ExprType::BINOP:
            switch (e->op) {
                case BinOp::ADD: case BinOp::SUB: {
                    auto ld = decompose_linear(e->left, t);
                    auto rd = decompose_linear(e->right, t);
                    if (!ld || !rd) return fail();
                    return ok(simplify(Expr::BinOpExpr(e->op, ld->coeff, rd->coeff)),
                              simplify(Expr::BinOpExpr(e->op, ld->rest, rd->rest)));
                }
                case BinOp::MUL: {
                    const bool lh = contains_var(e->left, t);
                    const bool rh = contains_var(e->right, t);
                    if (lh && rh) return fail();
                    if (!lh && !rh) return ok(Expr::Num(0), e);
                    auto [side, factor] = lh ? std::pair{e->left, e->right}
                                             : std::pair{e->right, e->left};
                    auto d = decompose_linear(side, t);
                    return d ? ok(simplify(Expr::BinOpExpr(BinOp::MUL, factor, d->coeff)),
                                  simplify(Expr::BinOpExpr(BinOp::MUL, factor, d->rest))) : fail();
                }
                case BinOp::DIV: {
                    if (contains_var(e->right, t)) return fail();
                    auto d = decompose_linear(e->left, t);
                    return d ? ok(simplify(Expr::BinOpExpr(BinOp::DIV, d->coeff, e->right)),
                                  simplify(Expr::BinOpExpr(BinOp::DIV, d->rest, e->right))) : fail();
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
// Returns the inverted RHS expression(s), or empty vector if no inverse is
// known. May return MULTIPLE inverses when the sub-system defines multiple
// inverse equations for the same input variable (e.g., sin(x) → both
// `x = asin(result)` AND `x = pi - asin(result)`). Used by solve_by_inversion
// to emit one Solution per branch — required by Periodicity Detection (Future
// #12) and by abs / sqrt-style multi-branch inverses.
// Set by FormulaSystem to resolve via .fw sub-system definitions.
using FuncInverter = std::function<std::vector<ExprPtr>(const std::string& func_name, const ExprPtr& rhs)>;  // std::function: boundary erasure — typed thread_local registered from system.h, must be storable in a typed variable

inline FuncInverter& solve_func_inverter_() {
    static thread_local FuncInverter inverter;
    return inverter;
}

inline void solve_set_func_inverter(FuncInverter fn) {
    solve_func_inverter_() = std::move(fn);
}

// Existence checker (gen-5 cycle 3d, 2026-05-16): boundary-erased callback
// answering "does there exist n such that section(n) = value?". Wired by
// FormulaSystem at every solver-entry site via ExistenceCheckerGuard (3
// sites: derive_all, resolve, resolve_all). Read by `check_condition`'s
// FUNCTION_SECTION dispatch arm.
//
// Matches FuncInverter precedent immediately above. If profiled hot, both
// std::function carriers should migrate to fn-ptr+opaque together — single
// trigger, single migration. See Future #89.
//
// `using ExistenceChecker = ...` + forward-decl of solve_existence_checker_
// live up at the SimplifyContext block (so check_condition can call it
// without a layout change). Definition lives here next to FuncInverter.

inline ExistenceChecker& solve_existence_checker_() {
    static thread_local ExistenceChecker checker;
    return checker;
}

inline void solve_set_existence_checker(ExistenceChecker fn) {
    solve_existence_checker_() = std::move(fn);
}

// Try to isolate target by peeling off invertible functions and operations.
// Returns ALL solutions (e.g., abs gives two, sqrt gives one).
[[nodiscard]] inline std::vector<Solution> solve_by_inversion(ExprPtr lhs, ExprPtr rhs,
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
    // FuncInverter returns ALL inverse branches (sin → 2 branches, cos → 2,
    // tan → 1, others → 1). One Solution emitted per branch.
    if (lhs->type == ExprType::FUNC_CALL && lhs->args.size() == 1
        && contains_var(lhs->args[0], target)) {
        const auto& inverter = solve_func_inverter_();
        if (inverter) {
            auto branches = inverter(lhs->name, rhs);
            std::vector<Solution> all;
            for (auto& new_rhs : branches) {
                if (!new_rhs) continue;
                auto sols = recurse(lhs->args[0], new_rhs);
                all.insert(all.end(), sols.begin(), sols.end());
            }
            if (!all.empty()) return all;
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
[[nodiscard]] inline ExprPtr expand_for_var(const ExprPtr& e, const std::string& var) {
    if (!e || !contains_var(e, var)) return e;
    if (e->type == ExprType::BINOP) {
        auto l = expand_for_var(e->left, var);
        auto r = expand_for_var(e->right, var);
        if (e->op == BinOp::MUL) {
            const bool l_sum = l->type == ExprType::BINOP &&
                (l->op == BinOp::ADD || l->op == BinOp::SUB);
            const bool r_sum = r->type == ExprType::BINOP &&
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
[[nodiscard]] inline std::vector<Solution> solve_for_all(const ExprPtr& lhs, const ExprPtr& rhs,
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

                const std::string cond = expr_to_string(disc) + " >= 0";
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
[[nodiscard]] inline ExprPtr solve_for(const ExprPtr& lhs, const ExprPtr& rhs, const std::string& target) {
    auto sols = solve_for_all(lhs, rhs, target);
    return sols.empty() ? nullptr : sols[0].expr;
}

// ============================================================
// Section: Numerical solvers (newton_solve, bisection, adaptive_simpson, find_numeric_roots, adaptive_scan)
// ============================================================

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
[[nodiscard]] inline double snap_integer(double x, double tol = EPSILON_ZERO) {
    const double r = std::round(x);
    return std::abs(x - r) < tol ? r : x;
}

// Newton's method: solve f(x) = 0 starting from x0.
// Optional `fp_fn` supplies the analytic derivative; when null, central finite
// differences are used (default behavior, byte-identical to the pre-M4 path).
template<class F>
[[nodiscard]] inline std::optional<double> newton_solve(
        F&& f, double x0,
        int max_iter = NUMERIC_MAX_ITER, double tol = NUMERIC_TOLERANCE,
        const std::function<double(double)>* fp_fn = nullptr) {  // std::function: optional derivative callback; nullable pointer pattern is the right shape for "derivative may not be available"
    double x = x0;
    for (int i = 0; i < max_iter; i++) {
        const double fx = f(x);
        if (std::isnan(fx) || std::isinf(fx)) return std::nullopt;
        if (std::abs(fx) < tol) return snap_integer(x);

        double fp;
        if (fp_fn) {
            fp = (*fp_fn)(x);
        } else {
            // Central difference derivative
            const double h = std::max(1e-8, std::abs(x) * 1e-8);
            fp = (f(x + h) - f(x - h)) / (2.0 * h);
        }
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
    const double fx = f(x);
    if (!std::isnan(fx) && std::abs(fx) < tol * 100) return snap_integer(x);
    return std::nullopt;
}

// Bisection: find root of f in [lo, hi] where f(lo) and f(hi) have opposite signs.
template<class F>
[[nodiscard]] inline std::optional<double> bisection_solve(
        F&& f, double lo, double hi,
        int max_iter = NUMERIC_MAX_ITER, double tol = NUMERIC_TOLERANCE) {
    double flo = f(lo), fhi = f(hi);
    if (std::isnan(flo) || std::isnan(fhi)) return std::nullopt;
    if (flo * fhi > 0) return std::nullopt; // no sign change

    for (int i = 0; i < max_iter; i++) {
        const double mid = (lo + hi) / 2.0;
        const double fmid = f(mid);
        if (std::isnan(fmid) || std::isinf(fmid)) return std::nullopt;
        if (std::abs(fmid) < tol || (hi - lo) < tol)
            return snap_integer(mid);
        // Note: fhi is not tracked inside the loop — only flo participates in the sign test (flo * fmid).
        if (flo * fmid < 0) { hi = mid; }
        else                { lo = mid; flo = fmid; }
    }
    return snap_integer((lo + hi) / 2.0);
}

// Adaptive Simpson's rule on [a, b] with recursive bisection. Used as the
// numeric fallback for definite `integral(f, x, a, b)` when the symbolic
// antiderivative path returns nullptr (Future #16, M2). Paired with
// `symbolic_integrate` (defined earlier in §Symbolic integration); dispatch
// lives in `resolve_integral_calls` (system.h). Standard estimator:
// |S(a,b) - S(a,m) - S(m,b)| / 15 ≈ error of S(a,b). When the estimate is
// below tolerance, accept S(a,m) + S(m,b) as the integral; otherwise recurse
// on each half with halved tolerance, capped by `max_depth` to prevent
// runaway. NaN at any sample short-circuits to NaN (caller surfaces the
// unevaluated `integral(...)` when this fires).
constexpr int ADAPTIVE_SIMPSON_MAX_DEPTH = 30;
static_assert(ADAPTIVE_SIMPSON_MAX_DEPTH >= 10
              && ADAPTIVE_SIMPSON_MAX_DEPTH <= 60);

// Integration tolerance — semantically distinct from NUMERIC_TOLERANCE
// (which is a root-finding absolute residual tolerance, 1e-10). Adaptive
// Simpson's convergence test is |S(a,b) - S(a,m) - S(m,b)| <= 15*tol —
// this controls absolute integral error, NOT residual. 1e-7 matches the
// realistic precision of double-precision arithmetic on smooth integrands
// after cancellation; tighter tolerances drive recursion to near-max-depth
// without buying useful precision. Reviewer Cycle 2 (M2) 2026-05-10.
constexpr double INTEGRATION_TOLERANCE = 1e-7;
static_assert(INTEGRATION_TOLERANCE > 0.0 && INTEGRATION_TOLERANCE < 1e-3);

template<class F>
[[nodiscard]] inline double adaptive_simpson_recurse(F&& fn,
        double a, double b, double fa, double fb, double fm, double whole,
        double tol, int depth) {
    const double m  = 0.5 * (a + b);
    const double lm = 0.5 * (a + m);
    const double rm = 0.5 * (m + b);
    const double flm = fn(lm);
    const double frm = fn(rm);
    if (std::isnan(flm) || std::isnan(frm)) return std::nan("");
    const double left  = (m - a) / 6.0 * (fa + 4.0 * flm + fm);
    const double right = (b - m) / 6.0 * (fm + 4.0 * frm + fb);
    const double sum   = left + right;
    if (depth <= 0 || std::abs(sum - whole) <= 15.0 * tol)
        return sum + (sum - whole) / 15.0;
    const double half = 0.5 * tol;
    const double l = adaptive_simpson_recurse(fn, a, m, fa, fm, flm, left,  half, depth - 1);
    if (std::isnan(l)) return l;
    const double r = adaptive_simpson_recurse(fn, m, b, fm, fb, frm, right, half, depth - 1);
    return l + r;
}

template<class F>
[[nodiscard]] inline double adaptive_simpson(F&& fn, double a, double b,
        double tol = INTEGRATION_TOLERANCE,
        int max_depth = ADAPTIVE_SIMPSON_MAX_DEPTH) {
    if (a == b) return 0.0;
    if (a > b) {
        // Reverse: ∫[a→b] = -∫[b→a]. Recurse to keep the recursive helper's
        // (a < b) precondition.
        const double v = adaptive_simpson(fn, b, a, tol, max_depth);
        return std::isnan(v) ? v : -v;
    }
    const double fa = fn(a);
    const double fb = fn(b);
    const double m  = 0.5 * (a + b);
    const double fm = fn(m);
    if (std::isnan(fa) || std::isnan(fb) || std::isnan(fm)) return std::nan("");
    const double whole = (b - a) / 6.0 * (fa + 4.0 * fm + fb);
    return adaptive_simpson_recurse(fn, a, b, fa, fb, fm, whole, tol, max_depth);
}

// Adaptive grid scan: find intervals where f changes sign.
// Uses coarse pass with jitter, then refines near sign changes and high-gradient regions.
// Deterministic: uses fixed seed for reproducible jitter.
template<class F>
[[nodiscard]] inline std::vector<std::pair<double, double>> adaptive_scan(
        F&& f, double lo, double hi,
        bool integer_only = false, int n_samples = NUMERIC_DEFAULT_SAMPLES) {
    struct Sample { double x, fx; };
    std::vector<Sample> samples;

    if (integer_only) {
        const int ilo = static_cast<int>(std::ceil(lo));
        const int ihi = static_cast<int>(std::floor(hi));
        for (int i = ilo; i <= ihi; i++) {
            const double fx = f(static_cast<double>(i));
            if (std::isfinite(fx)) samples.push_back({static_cast<double>(i), fx});
        }
    } else {
        // Coarse pass with deterministic jitter — reproducibility is intentional:
        // users must see the same numeric probe sequence every run for the solver output to be deterministic.
        // NOLINTNEXTLINE(bugprone-random-generator-seed)
        std::mt19937_64 rng(NUMERIC_SEED);
        std::uniform_real_distribution<double> jitter(-NUMERIC_JITTER_FRAC, NUMERIC_JITTER_FRAC);
        const double step = (hi - lo) / n_samples;
        for (int i = 0; i <= n_samples; i++) {
            double x = lo + i * step;
            if (i > 0 && i < n_samples)
                x += jitter(rng) * step; // jitter interior points
            const double fx = f(x);
            if (std::isfinite(fx)) samples.push_back({x, fx});
        }

        // Find regions of interest: sign changes and high gradient
        std::vector<std::pair<double, double>> refine_regions;
        // justified: window comparison samples[i-1] vs samples[i]
        for (size_t i = 1; i < samples.size(); i++) {
            const bool sign_change = samples[i-1].fx * samples[i].fx < 0;
            const double gradient = std::abs(samples[i].fx - samples[i-1].fx)
                            / std::max(1e-15, samples[i].x - samples[i-1].x);
            // Refine near sign changes and steep gradients
            const double avg_grad = std::abs(samples[i].fx + samples[i-1].fx)
                            / std::max(1e-15, hi - lo);
            if (sign_change || gradient > avg_grad * 10)
                refine_regions.push_back({samples[i-1].x, samples[i].x});
        }

        // Refine pass: add dense samples in regions of interest
        const int fine_points = n_samples * 5;
        int points_per_region = refine_regions.empty() ? 0
            : fine_points / static_cast<int>(refine_regions.size());
        points_per_region = std::min(points_per_region, fine_points);
        for (auto& [rlo, rhi] : refine_regions) {
            // Expand region slightly
            const double margin = (rhi - rlo) * 0.5;
            const double elo = std::max(lo, rlo - margin);
            const double ehi = std::min(hi, rhi + margin);
            const double rstep = (ehi - elo) / std::max(1, points_per_region);
            for (int i = 0; i <= points_per_region; i++) {
                const double x = elo + i * rstep;
                const double fx = f(x);
                if (std::isfinite(fx)) samples.push_back({x, fx});
            }
        }

        // Sort all samples by x
        std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.x < b.x; });
    }

    // Collect sign-change intervals and exact zeros
    std::vector<std::pair<double, double>> intervals;
    // justified: window comparison samples[i-1] vs samples[i]
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
// Optional `fp_fn` is the analytic derivative passed through to newton_solve;
// null falls back to central finite differences.
template<class F>
[[nodiscard]] inline std::vector<double> find_numeric_roots(
        F&& f, double lo, double hi,
        bool integer_only = false, int n_samples = NUMERIC_DEFAULT_SAMPLES,
        const std::function<double(double)>* fp_fn = nullptr) {  // std::function: optional derivative callback passed through to newton_solve; nullable pointer matches "derivative may not be available"
    auto intervals = adaptive_scan(f, lo, hi, integer_only, n_samples);
    std::vector<double> roots;

    for (auto& [a, b] : intervals) {
        std::optional<double> root;

        if (integer_only) {
            // For integers, just check exact values
            const int ia = static_cast<int>(std::round(a));
            const int ib = static_cast<int>(std::round(b));
            for (int i = ia; i <= ib; i++) {
                const double fx = f(static_cast<double>(i));
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
            const double mid = (a + b) / 2.0;
            root = newton_solve(f, mid, NUMERIC_MAX_ITER, NUMERIC_TOLERANCE, fp_fn);
            if (!root || *root < a - 1.0 || *root > b + 1.0)
                root = bisection_solve(f, a, b);
        }

        if (root) {
            // Post-validate: reject false roots (singularities)
            const double fr = f(*root);
            if (std::isnan(fr) || std::abs(fr) > NUMERIC_TOLERANCE * 1000) continue;

            // Deduplicate
            const bool dup = std::any_of(roots.begin(), roots.end(),
                [&root](double r) { return std::abs(r - *root) < EPSILON_ZERO; });
            if (!dup) roots.push_back(*root);
        }
    }

    std::sort(roots.begin(), roots.end());
    return roots;
}
