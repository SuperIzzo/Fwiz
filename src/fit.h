#pragma once
#include "expr.h"
#include <numeric>

// ============================================================================
//  Constants
// ============================================================================

constexpr int    FIT_DEFAULT_SAMPLES    = 200;
constexpr int    FIT_MAX_DEGREE         = 10;
constexpr double FIT_R2_THRESHOLD       = 0.9999;
constexpr double FIT_PERFECT_THRESHOLD  = 1e-10;
constexpr double FIT_COEFF_SNAP_TOL     = 1e-6;

// Upper bound on denominators considered by recognize_fraction when the
// recognizer is scanning for a rational multiplier. 360 covers degree↔radian
// (pi/180), common trig denominators, and small integer ratios; user-defined
// aliases (e.g. `eng = pi/1000`) handle the long tail via the extra_constants
// parameter threaded through expr_recognize_constants → fmt_exact_double.
// Was 12 before the 2026-04-19 dedup cycle — too tight to catch pi/180 in
// triangle output.
constexpr int    RECOGNIZE_FRACTION_MAX_DEN = 360;

static_assert(FIT_DEFAULT_SAMPLES >= 50);
static_assert(FIT_MAX_DEGREE >= 1 && FIT_MAX_DEGREE <= 20);
static_assert(FIT_R2_THRESHOLD > 0.99 && FIT_R2_THRESHOLD <= 1.0);
static_assert(RECOGNIZE_FRACTION_MAX_DEN > 0 && RECOGNIZE_FRACTION_MAX_DEN < 10000);

// ============================================================================
//  Sampling
// ============================================================================

struct FitSample { double x, y; };

// Evenly spaced samples of f over [lo, hi] with deterministic jitter.
// Skips NaN/Inf results.
inline std::vector<FitSample> sample_function(
        const std::function<double(double)>& f,
        double lo, double hi, int n_points = FIT_DEFAULT_SAMPLES) {
    std::vector<FitSample> samples;
    if (n_points < 2 || lo >= hi) return samples;

    // Deterministic seed — fit output must be reproducible across runs.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937_64 rng(NUMERIC_SEED);
    std::uniform_real_distribution<double> jitter(-NUMERIC_JITTER_FRAC, NUMERIC_JITTER_FRAC);
    double step = (hi - lo) / n_points;

    for (int i = 0; i <= n_points; i++) {
        double x = lo + i * step;
        if (i > 0 && i < n_points)
            x += jitter(rng) * step;
        double y = f(x);
        if (std::isfinite(y)) samples.push_back({x, y});
    }
    return samples;
}

// ============================================================================
//  Small dense matrix operations (for Vandermonde systems up to ~11x11)
// ============================================================================

using FitMatrix = std::vector<std::vector<double>>;

// Build Vandermonde matrix A[i][j] = x_i^j for j = 0..degree
inline FitMatrix vandermonde(const std::vector<FitSample>& samples, int degree) {
    FitMatrix A(samples.size(), std::vector<double>(degree + 1));
    for (size_t i = 0; i < samples.size(); i++) {
        double xp = 1.0;
        for (int j = 0; j <= degree; j++) {
            A[i][j] = xp;
            xp *= samples[i].x;
        }
    }
    return A;
}

// Least squares solve via normal equations: (AᵀA)x = Aᵀb
// A is m×n (m >= n). Returns the n-element solution vector.
// Uses Gaussian elimination with partial pivoting on the normal equations.
// Stable enough for Vandermonde systems up to degree ~10.
inline std::vector<double> least_squares_solve(const FitMatrix& A, const std::vector<double>& b) {
    int m = static_cast<int>(A.size());
    int n = static_cast<int>(A[0].size());
    assert(m >= n);
    assert(static_cast<int>(b.size()) == m);

    // Form AᵀA (n×n) and Aᵀb (n×1)
    FitMatrix AtA(n, std::vector<double>(n, 0.0));
    std::vector<double> Atb(n, 0.0);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++)
            for (int k = 0; k < m; k++)
                AtA[i][j] += A[k][i] * A[k][j];
        for (int k = 0; k < m; k++)
            Atb[i] += A[k][i] * b[k];
    }

    // Gaussian elimination with partial pivoting
    for (int col = 0; col < n; col++) {
        // Find pivot
        int pivot = col;
        for (int row = col + 1; row < n; row++)
            if (std::abs(AtA[row][col]) > std::abs(AtA[pivot][col]))
                pivot = row;
        if (pivot != col) {
            std::swap(AtA[col], AtA[pivot]);
            std::swap(Atb[col], Atb[pivot]);
        }
        if (std::abs(AtA[col][col]) < 1e-15) continue;

        // Eliminate below
        for (int row = col + 1; row < n; row++) {
            double factor = AtA[row][col] / AtA[col][col];
            for (int j = col; j < n; j++)
                AtA[row][j] -= factor * AtA[col][j];
            Atb[row] -= factor * Atb[col];
        }
    }

    // Back-substitution
    std::vector<double> x(n, 0.0);
    for (int i = n - 1; i >= 0; i--) {
        double sum = Atb[i];
        for (int j = i + 1; j < n; j++) sum -= AtA[i][j] * x[j];
        if (std::abs(AtA[i][i]) < 1e-15) { x[i] = 0; continue; }
        x[i] = sum / AtA[i][i];
    }
    return x;
}

// ============================================================================
//  Polynomial fitting
// ============================================================================

struct FitResult {
    std::vector<double> coefficients;  // c[0] + c[1]*x + c[2]*x² + ...
    double r_squared = 0;
    double max_error = 0;
    int degree = 0;
    bool exact = false;
    ExprPtr expr = nullptr;
};

// Evaluate polynomial at x
inline double poly_eval(const std::vector<double>& c, double x) {
    double result = 0, xp = 1.0;
    for (size_t i = 0; i < c.size(); i++) {
        result += c[i] * xp;
        xp *= x;
    }
    return result;
}

// Compute R² and max error for any prediction function
inline void compute_fit_stats(FitResult& result, const std::vector<FitSample>& samples,
        const std::function<double(double)>& predict) {
    double y_mean = 0;
    for (const auto& s : samples) y_mean += s.y;
    y_mean /= static_cast<double>(samples.size());
    double ss_res = 0, ss_tot = 0;
    result.max_error = 0;
    for (const auto& s : samples) {
        double predicted = predict(s.x);
        if (!std::isfinite(predicted)) { result.r_squared = -1; return; }
        double residual = s.y - predicted;
        ss_res += residual * residual;
        ss_tot += (s.y - y_mean) * (s.y - y_mean);
        result.max_error = std::max(result.max_error, std::abs(residual));
    }
    result.r_squared = (ss_tot < 1e-30) ? 1.0 : (1.0 - ss_res / ss_tot);
    result.exact = result.max_error < FIT_PERFECT_THRESHOLD;
}

// Snap coefficient to nearest integer if within tolerance
inline double snap_coeff(double c) {
    double r = std::round(c);
    return std::abs(c - r) < FIT_COEFF_SNAP_TOL ? r : c;
}

// Fit a polynomial of given degree to samples
inline FitResult fit_polynomial(const std::vector<FitSample>& samples, int degree) {
    FitResult result;
    result.degree = degree;

    if (samples.size() <= static_cast<size_t>(degree)) {
        result.r_squared = 0;
        return result;
    }

    auto A = vandermonde(samples, degree);
    std::vector<double> b;
    b.reserve(samples.size());
    for (const auto& s : samples) b.push_back(s.y);

    result.coefficients = least_squares_solve(A, b);

    // Snap coefficients to integers
    for (auto& c : result.coefficients) c = snap_coeff(c);

    compute_fit_stats(result, samples, [&result](double x) {
        return poly_eval(result.coefficients, x);
    });
    return result;
}

// Auto-select best polynomial degree: tries 1 through max_degree.
// Picks lowest degree where R² > threshold, or the best overall.
inline FitResult fit_polynomial_auto(
        const std::vector<FitSample>& samples,
        int max_degree = FIT_MAX_DEGREE) {
    FitResult best;
    best.r_squared = -1;

    for (int d = 1; d <= max_degree; d++) {
        if (samples.size() <= static_cast<size_t>(d)) break;
        auto result = fit_polynomial(samples, d);
        if (result.r_squared > best.r_squared) best = result;
        if (result.r_squared >= FIT_R2_THRESHOLD) break;
    }
    return best;
}

// ============================================================================
//  Rational and irrational number recognition
// ============================================================================

struct Fraction { int p, q; }; // numerator, denominator

// Try to express x as p/q with denominator q ≤ max_den (numerator bound is implicit: |p| ≈ |x| * max_den).
// Callers that need a separate numerator bound must apply it after the call.
inline std::optional<Fraction> recognize_fraction(double x,
        int max_den = 12, double tol = 1e-9) {
    if (!std::isfinite(x)) return std::nullopt;
    for (int q = 1; q <= max_den; q++) {
        double p_exact = x * q;
        int p = static_cast<int>(std::round(p_exact));
        if (std::abs(p_exact - p) < tol * q)
            return Fraction{p, q};
    }
    return std::nullopt;
}

struct ConstantForm {
    int p = 1, q = 1;           // rational part (p/q)
    std::string constant;        // "pi", "e", "phi", "" if pure rational
    int power = 1;               // constant^power (1, 2, -1)
};

// Try to express x as (p/q) * constant^power
// Tests against builtin constants plus any additional constants provided.
// Returns nullopt if x is a simple rational number or not recognizable.
inline std::optional<ConstantForm> recognize_constant(double x,
        const std::map<std::string, double>& extra_constants = {},
        double tol = 1e-9) {
    if (!std::isfinite(x) || std::abs(x) < tol) return std::nullopt;

    // If it's already a simple fraction, no constant needed
    if (recognize_fraction(x, RECOGNIZE_FRACTION_MAX_DEN, tol)) return std::nullopt;

    // Build combined constant table: builtins + common roots + extras
    auto& builtins = builtin_constants();
    std::map<std::string, double> all_constants = builtins;
    // Common irrational roots (output as sqrt(N) via constant_form_to_expr)
    all_constants["sqrt(2)"] = std::sqrt(2.0);
    all_constants["sqrt(3)"] = std::sqrt(3.0);
    all_constants["sqrt(5)"] = std::sqrt(5.0);
    // Common logarithms (output as log(N) via constant_form_to_expr)
    all_constants["log(2)"] = std::log(2.0);
    all_constants["log(3)"] = std::log(3.0);
    all_constants["log(10)"] = std::log(10.0);
    for (auto& [k, v] : extra_constants)
        if (!all_constants.count(k)) all_constants[k] = v;

    // Try each constant at various powers
    static const int powers[] = {1, 2, -1};
    for (auto& [name, val] : all_constants) {
        for (int pw : powers) {
            double cv = (pw == 1) ? val : (pw == 2) ? val * val : 1.0 / val;
            double quotient = x / cv;
            auto frac = recognize_fraction(quotient, RECOGNIZE_FRACTION_MAX_DEN, tol);
            if (frac) return ConstantForm{frac->p, frac->q, name, pw};
        }
    }
    return std::nullopt;
}

// Build an ExprPtr from a ConstantForm: (p/q) * constant^power
inline ExprPtr constant_form_to_expr(const ConstantForm& cf) {
    // Build constant expression — handle func(N) names as function calls
    ExprPtr cexpr;
    auto paren = cf.constant.find('(');
    if (paren != std::string::npos && cf.constant.back() == ')') {
        // Parse "func(N)" → Expr::Call("func", {Expr::Num(N)})
        std::string func = cf.constant.substr(0, paren);
        double n = std::stod(cf.constant.substr(paren + 1, cf.constant.size() - paren - 2));
        cexpr = Expr::Call(func, {Expr::Num(n)});
    } else {
        cexpr = Expr::Var(cf.constant);
    }
    if (cf.power == 2)
        cexpr = Expr::BinOpExpr(BinOp::POW, cexpr, Expr::Num(2));
    else if (cf.power == -1) {
        // For p/q * constant^(-1): emit (p/q) / constant directly
        if (cf.p == 1 && cf.q == 1) return Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), cexpr);
        if (cf.p == -1 && cf.q == 1) return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, Expr::Num(1), cexpr));
        ExprPtr numer = (cf.q == 1) ? Expr::Num(static_cast<double>(cf.p))
            : Expr::BinOpExpr(BinOp::DIV,
                Expr::Num(static_cast<double>(cf.p)),
                Expr::Num(static_cast<double>(cf.q)));
        return Expr::BinOpExpr(BinOp::DIV, numer, cexpr);
    }

    // Build rational multiplier
    if (cf.p == 1 && cf.q == 1) return cexpr;
    if (cf.p == -1 && cf.q == 1) return Expr::Neg(cexpr);

    ExprPtr coeff;
    if (cf.q == 1)
        coeff = Expr::Num(static_cast<double>(cf.p));
    else
        coeff = Expr::BinOpExpr(BinOp::DIV,
            Expr::Num(static_cast<double>(cf.p)),
            Expr::Num(static_cast<double>(cf.q)));

    // Don't simplify — simplifier would evaluate sqrt(3)*5 → 8.66...
    return Expr::BinOpExpr(BinOp::MUL, coeff, cexpr);
}

// ============================================================================
//  Expression tree: recognize constants in NUM nodes
// ============================================================================

// Walk an expression tree and replace floating-point NUM nodes with recognized
// symbolic forms (fractions, constants like log(2), pi, etc.).
// Used by derive output to produce clean symbolic expressions.
inline ExprPtr expr_recognize_constants(const ExprPtr& e,
        const std::map<std::string, double>& extra_constants = {}) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM: {
            if (is_integer_value(e->num)) return e; // already clean
            if (is_zero(*e)) return e;
            // Try fraction first
            auto frac = recognize_fraction(e->num, RECOGNIZE_FRACTION_MAX_DEN);
            if (frac) return make_rational(frac->p, frac->q);
            // Try constant recognition
            auto cf = recognize_constant(e->num, extra_constants);
            if (cf) return constant_form_to_expr(*cf);
            return e;
        }
        case ExprType::VAR: return e;
        case ExprType::UNARY_NEG:
            return Expr::Neg(expr_recognize_constants(e->child, extra_constants));
        case ExprType::BINOP: {
            auto l = expr_recognize_constants(e->left, extra_constants);
            auto r = expr_recognize_constants(e->right, extra_constants);
            return Expr::BinOpExpr(e->op, l, r);
        }
        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> args;
            args.reserve(e->args.size());
            for (auto& a : e->args)
                args.push_back(expr_recognize_constants(a, extra_constants));
            return Expr::Call(e->name, std::move(args));
        }
        case ExprType::COUNT_: assert(false && "invalid ExprType"); return e;
    }
    return e;
}

// Shared "double → exact pretty string" formatter used by both solve and
// derive output paths. Wraps the NUM in a leaf, runs constant/fraction
// recognition, and stringifies. If nothing recognizes, falls back to fmt_num.
// Requires an active ExprArena::Scope (like expr_recognize_constants).
// extra_constants: user-defined aliases (e.g. {"deg", pi/180}); these are
// threaded through into recognition so derive/solve output surfaces user
// names instead of raw decimals.
inline std::string fmt_exact_double(double v,
        const std::map<std::string, double>& extra_constants = {}) {
    const auto* e = expr_recognize_constants(Expr::Num(v), extra_constants);
    return (e->type == ExprType::NUM) ? fmt_num(v) : expr_to_string(e);
}

// ============================================================================
//  Expression tree construction
// ============================================================================

// Build an ExprPtr for a single coefficient value.
// Tries: integer snap → constant recognition → raw number.
inline ExprPtr coeff_to_expr(double c,
        const std::map<std::string, double>& extra_constants = {}) {
    // Try integer snap
    double snapped = snap_coeff(c);
    if (snapped == std::round(snapped) && std::abs(snapped) < 1e15)
        return Expr::Num(snapped);

    // Try constant recognition (pi, e, etc.)
    auto cf = recognize_constant(c, extra_constants);
    if (cf) return constant_form_to_expr(*cf);

    // Raw number
    return Expr::Num(c);
}

// Build ExprPtr from polynomial coefficients: c[0] + c[1]*x + c[2]*x² + ...
// Requires active ExprArena::Scope. Drops near-zero coefficients.
// Uses constant recognition to express coefficients symbolically where possible.
inline ExprPtr poly_to_expr(const std::vector<double>& coeffs, const std::string& var,
        const std::map<std::string, double>& extra_constants = {}) {
    ExprPtr result = nullptr;

    for (size_t i = 0; i < coeffs.size(); i++) {
        if (std::abs(coeffs[i]) < EPSILON_ZERO) continue;

        ExprPtr term;
        if (i == 0) {
            term = coeff_to_expr(coeffs[i], extra_constants);
        } else {
            ExprPtr x_part;
            if (i == 1) {
                x_part = Expr::Var(var);
            } else {
                x_part = Expr::BinOpExpr(BinOp::POW, Expr::Var(var), Expr::Num(static_cast<double>(i)));
            }

            double c = snap_coeff(coeffs[i]);
            if (c == 1.0) {
                term = x_part;
            } else if (c == -1.0) {
                term = Expr::Neg(x_part);
            } else {
                auto cf = recognize_constant(coeffs[i], extra_constants);
                ExprPtr cexpr = cf ? constant_form_to_expr(*cf) : Expr::Num(c);
                term = Expr::BinOpExpr(BinOp::MUL, cexpr, x_part);
            }
        }

        if (!result) {
            result = term;
        } else {
            result = Expr::BinOpExpr(BinOp::ADD, result, term);
        }
    }

    if (!result) result = Expr::Num(0);
    // Note: don't simplify — the simplifier would evaluate sqrt(3) → 1.732...
    // The expression is already clean from construction (zero coeffs dropped, etc.)
    return result;
}

// ============================================================================
//  Template fitting (power law, exponential, logarithmic, sinusoidal)
// ============================================================================


// Power law: y = a * x^b  →  log(y) = log(a) + b*log(x)
inline FitResult fit_power_law(const std::vector<FitSample>& samples,
        const std::string& var = "x",
        const std::map<std::string, double>& extra_constants = {}) {
    FitResult result;
    result.degree = -1;

    std::vector<FitSample> log_samples;
    log_samples.reserve(samples.size());
    for (const auto& s : samples)
        if (s.x > 0 && s.y > 0)
            log_samples.push_back({std::log(s.x), std::log(s.y)});
    if (log_samples.size() < 3) return result;

    auto A = vandermonde(log_samples, 1);
    std::vector<double> b;
    b.reserve(log_samples.size());
    for (const auto& s : log_samples) b.push_back(s.y);
    auto coeffs = least_squares_solve(A, b);
    double a = std::exp(coeffs[0]), power = coeffs[1];

    result.coefficients = {a, power};
    compute_fit_stats(result, samples, [a, power](double x) {
        return a * std::pow(x, power);
    });

    ExprPtr a_expr = coeff_to_expr(snap_coeff(a), extra_constants);
    ExprPtr pow_expr = Expr::BinOpExpr(BinOp::POW, Expr::Var(var),
        coeff_to_expr(snap_coeff(power), extra_constants));
    result.expr = simplify(Expr::BinOpExpr(BinOp::MUL, a_expr, pow_expr));
    return result;
}

// Exponential: y = a * e^(b*x)  →  log(y) = log(a) + b*x
// Also tries quadratic exponent (Gaussian): y = a * e^(b*x² + c*x)
inline FitResult fit_exponential(const std::vector<FitSample>& samples,
        const std::string& var = "x",
        const std::map<std::string, double>& extra_constants = {}) {
    std::vector<FitSample> log_samples;
    for (const auto& s : samples)
        if (s.y > 0) log_samples.push_back({s.x, std::log(s.y)});
    if (log_samples.size() < 3) { FitResult r; r.degree = -1; return r; }

    FitResult best;
    best.degree = -1;
    best.r_squared = -1;

    // Try linear exponent: y = a * e^(b*x)
    {
        auto A = vandermonde(log_samples, 1);
        std::vector<double> b;
        b.reserve(log_samples.size());
        for (const auto& s : log_samples) b.push_back(s.y);
        auto coeffs = least_squares_solve(A, b);
        double a = std::exp(coeffs[0]), rate = coeffs[1];

        FitResult result;
        result.degree = -1;
        result.coefficients = {a, rate};
        compute_fit_stats(result, samples, [a, rate](double x) {
            return a * std::exp(rate * x);
        });

        if (result.r_squared > best.r_squared) {
            ExprPtr a_expr = coeff_to_expr(snap_coeff(a), extra_constants);
            ExprPtr bx = Expr::BinOpExpr(BinOp::MUL,
                coeff_to_expr(snap_coeff(rate), extra_constants), Expr::Var(var));
            result.expr = Expr::BinOpExpr(BinOp::MUL, a_expr,
                Expr::BinOpExpr(BinOp::POW, Expr::Var("e"), bx));
            best = result;
        }
    }

    // Try quadratic exponent (Gaussian): y = a * e^(b*x² + c*x)
    if (log_samples.size() >= 4) {
        auto A = vandermonde(log_samples, 2);
        std::vector<double> b;
        b.reserve(log_samples.size());
        for (const auto& s : log_samples) b.push_back(s.y);
        auto coeffs = least_squares_solve(A, b);
        double c0 = coeffs[0], c1 = coeffs[1], c2 = coeffs[2];
        double a = std::exp(c0);

        FitResult result;
        result.degree = -1;
        result.coefficients = {a, c2, c1};
        compute_fit_stats(result, samples, [a, c1, c2](double x) {
            return a * std::exp(c2 * x * x + c1 * x);
        });

        if (result.r_squared > best.r_squared) {
            ExprPtr a_expr = coeff_to_expr(snap_coeff(a), extra_constants);
            // Build exponent: c2*x² + c1*x
            ExprPtr exponent = nullptr;
            if (std::abs(c2) > EPSILON_ZERO) {
                ExprPtr x2 = Expr::BinOpExpr(BinOp::POW, Expr::Var(var), Expr::Num(2));
                exponent = Expr::BinOpExpr(BinOp::MUL,
                    coeff_to_expr(snap_coeff(c2), extra_constants), x2);
            }
            if (std::abs(c1) > EPSILON_ZERO) {
                ExprPtr cx = Expr::BinOpExpr(BinOp::MUL,
                    coeff_to_expr(snap_coeff(c1), extra_constants), Expr::Var(var));
                exponent = exponent
                    ? Expr::BinOpExpr(BinOp::ADD, exponent, cx) : cx;
            }
            if (!exponent) exponent = Expr::Num(0);
            result.expr = Expr::BinOpExpr(BinOp::MUL, a_expr,
                Expr::BinOpExpr(BinOp::POW, Expr::Var("e"), exponent));
            best = result;
        }
    }

    return best;
}

// Logarithmic: y = a * log(x) + b
inline FitResult fit_logarithmic(const std::vector<FitSample>& samples,
        const std::string& var = "x",
        const std::map<std::string, double>& extra_constants = {}) {
    FitResult result;
    result.degree = -1;

    std::vector<FitSample> log_samples;
    log_samples.reserve(samples.size());
    for (auto& s : samples)
        if (s.x > 0) log_samples.push_back({std::log(s.x), s.y});
    if (log_samples.size() < 3) return result;

    auto A = vandermonde(log_samples, 1);
    std::vector<double> b;
    b.reserve(log_samples.size());
    for (const auto& s : log_samples) b.push_back(s.y);
    auto coeffs = least_squares_solve(A, b);
    double intercept = coeffs[0], slope = coeffs[1];

    result.coefficients = {intercept, slope};
    compute_fit_stats(result, samples, [intercept, slope](double x) {
        return (x > 0) ? slope * std::log(x) + intercept : std::numeric_limits<double>::quiet_NaN();
    });

    ExprPtr log_part = Expr::Call("log", {Expr::Var(var)});
    ExprPtr a_expr = coeff_to_expr(snap_coeff(slope), extra_constants);
    ExprPtr term = simplify(Expr::BinOpExpr(BinOp::MUL, a_expr, log_part));
    if (std::abs(intercept) > EPSILON_ZERO) {
        ExprPtr b_expr = coeff_to_expr(snap_coeff(intercept), extra_constants);
        result.expr = simplify(Expr::BinOpExpr(BinOp::ADD, term, b_expr));
    } else {
        result.expr = term;
    }
    return result;
}

// Sinusoidal: y = a * sin(b*x + c) + d
inline FitResult fit_sinusoidal(const std::vector<FitSample>& samples,
        const std::string& var = "x",
        const std::map<std::string, double>& extra_constants = {}) {
    FitResult result;
    result.degree = -1;
    if (samples.size() < 10) return result;

    // Estimate frequency via zero-crossing count
    double y_mean = 0;
    for (const auto& s : samples) y_mean += s.y;
    y_mean /= static_cast<double>(samples.size());
    int zero_crossings = 0;
    for (size_t i = 1; i < samples.size(); i++)
        if ((samples[i-1].y - y_mean) * (samples[i].y - y_mean) < 0)
            zero_crossings++;

    double x_range = samples.back().x - samples[0].x;
    if (x_range < 1e-15 || zero_crossings < 2) return result;
    double freq = M_PI * zero_crossings / x_range;

    // Linear regression: y = A*sin(freq*x) + B*cos(freq*x) + D
    FitMatrix A(samples.size(), std::vector<double>(3));
    std::vector<double> b;
    for (size_t i = 0; i < samples.size(); i++) {
        double wx = freq * samples[i].x;
        A[i][0] = std::sin(wx);
        A[i][1] = std::cos(wx);
        A[i][2] = 1.0;
        b.push_back(samples[i].y);
    }
    auto coeffs = least_squares_solve(A, b);
    double amplitude = std::sqrt(coeffs[0]*coeffs[0] + coeffs[1]*coeffs[1]);
    double phase = std::atan2(coeffs[1], coeffs[0]);
    double offset = coeffs[2];

    result.coefficients = {amplitude, freq, phase, offset};
    compute_fit_stats(result, samples, [amplitude, freq, phase, offset](double x) {
        return amplitude * std::sin(freq * x + phase) + offset;
    });

    // Build expression
    ExprPtr freq_expr = coeff_to_expr(snap_coeff(freq), extra_constants);
    ExprPtr inner = simplify(Expr::BinOpExpr(BinOp::MUL, freq_expr, Expr::Var(var)));
    if (std::abs(phase) > EPSILON_ZERO)
        inner = simplify(Expr::BinOpExpr(BinOp::ADD, inner,
            coeff_to_expr(snap_coeff(phase), extra_constants)));
    ExprPtr sin_part = Expr::Call("sin", {inner});
    ExprPtr amp_expr = coeff_to_expr(snap_coeff(amplitude), extra_constants);
    ExprPtr term = simplify(Expr::BinOpExpr(BinOp::MUL, amp_expr, sin_part));
    if (std::abs(offset) > EPSILON_ZERO)
        result.expr = simplify(Expr::BinOpExpr(BinOp::ADD, term,
            coeff_to_expr(snap_coeff(offset), extra_constants)));
    else
        result.expr = term;
    return result;
}

// Reciprocal: y = a / (x + b) + c  →  1/(y-c) = (x+b)/a  →  linear in x
// Requires y values that aren't all the same sign
inline FitResult fit_reciprocal(const std::vector<FitSample>& samples,
        const std::string& var = "x",
        const std::map<std::string, double>& extra_constants = {}) {
    FitResult result;
    result.degree = -1;
    if (samples.size() < 5) return result;

    // Try different offsets c by estimating from the asymptotic value
    // As x → ∞, y → c. Use the last few samples to estimate c.
    double c_est = 0;
    int tail = std::min(5, static_cast<int>(samples.size()));
    for (int i = static_cast<int>(samples.size()) - tail; i < static_cast<int>(samples.size()); i++)
        c_est += samples[i].y;
    c_est /= tail;

    // Transform: 1/(y - c) should be linear in x
    // Try c = c_est and c = 0
    double best_r2 = -1;
    double best_a = 0, best_b = 0, best_c = 0;

    for (double c_try : {c_est, 0.0}) {
        std::vector<FitSample> inv_samples;
        inv_samples.reserve(samples.size());
        for (auto& s : samples) {
            double ym = s.y - c_try;
            if (std::abs(ym) < 1e-15) continue;
            inv_samples.push_back({s.x, 1.0 / ym});
        }
        if (inv_samples.size() < 3) continue;

        auto A = vandermonde(inv_samples, 1);
        std::vector<double> bv;
        bv.reserve(inv_samples.size());
        for (const auto& s : inv_samples) bv.push_back(s.y);
        auto coeffs = least_squares_solve(A, bv);
        // 1/(y-c) = coeffs[0] + coeffs[1]*x = (x + coeffs[0]/coeffs[1]) / (1/coeffs[1])
        // So a = 1/coeffs[1], b = coeffs[0]/coeffs[1]
        if (std::abs(coeffs[1]) < 1e-15) continue;
        double a = 1.0 / coeffs[1];
        double b = coeffs[0] / coeffs[1];

        // Compute R²
        double y_mean = 0;
        for (const auto& s : samples) y_mean += s.y;
        y_mean /= static_cast<double>(samples.size());
        double ss_res = 0, ss_tot = 0, max_err = 0;
        for (const auto& s : samples) {
            double predicted = a / (s.x + b) + c_try;
            if (!std::isfinite(predicted)) { ss_res = 1e30; break; }
            double residual = s.y - predicted;
            ss_res += residual * residual;
            ss_tot += (s.y - y_mean) * (s.y - y_mean);
            max_err = std::max(max_err, std::abs(residual));
        }
        double r2 = (ss_tot < 1e-30) ? 1.0 : (1.0 - ss_res / ss_tot);
        if (r2 > best_r2) {
            best_r2 = r2;
            best_a = a; best_b = b; best_c = c_try;
            result.max_error = max_err;
        }
    }

    if (best_r2 < 0) return result;
    result.r_squared = best_r2;
    result.exact = result.max_error < FIT_PERFECT_THRESHOLD;
    result.coefficients = {best_a, best_b, best_c};

    // Build expression: a / (x + b) + c
    ExprPtr a_expr = coeff_to_expr(snap_coeff(best_a), extra_constants);
    ExprPtr denom = (std::abs(best_b) > EPSILON_ZERO)
        ? Expr::BinOpExpr(BinOp::ADD, Expr::Var(var),
            coeff_to_expr(snap_coeff(best_b), extra_constants))
        : static_cast<ExprPtr>(Expr::Var(var));
    ExprPtr frac = Expr::BinOpExpr(BinOp::DIV, a_expr, denom);
    if (std::abs(best_c) > EPSILON_ZERO)
        result.expr = Expr::BinOpExpr(BinOp::ADD, frac,
            coeff_to_expr(snap_coeff(best_c), extra_constants));
    else
        result.expr = frac;
    return result;
}

constexpr int FIT_DEFAULT_DEPTH = 5;

// Sort and deduplicate fit results
inline std::vector<FitResult> sort_and_dedup(std::vector<FitResult>& fits) {
    std::sort(fits.begin(), fits.end(), [](const FitResult& a, const FitResult& b) {
        if (std::abs(a.r_squared - b.r_squared) > 1e-6)
            return a.r_squared > b.r_squared;
        return a.coefficients.size() < b.coefficients.size();
    });
    std::vector<FitResult> unique;
    std::set<std::string> seen;
    for (const auto& f : fits) {
        if (!f.expr) continue;
        std::string s = expr_to_string(f.expr);
        if (!seen.insert(s).second) continue;

        // Skip redundant abs() wrapping: unwrap all abs() layers and check
        ExprPtr unwrapped = f.expr;
        while (unwrapped->type == ExprType::FUNC_CALL && unwrapped->name == "abs"
               && !unwrapped->args.empty())
            unwrapped = unwrapped->args[0];
        if (unwrapped != f.expr && seen.count(expr_to_string(unwrapped)))
            continue;
        unique.push_back(f);
    }
    return unique;
}

// Try all base template forms + polynomial at depth 1.
inline std::vector<FitResult> fit_base(const std::vector<FitSample>& samples,
        const std::string& var,
        const std::map<std::string, double>& extra_constants,
        double min_r2) {
    std::vector<FitResult> fits;

    auto poly = fit_polynomial_auto(samples);
    if (poly.r_squared > min_r2) {
        poly.expr = poly_to_expr(poly.coefficients, var, extra_constants);
        fits.push_back(poly);
    }

    auto pw = fit_power_law(samples, var, extra_constants);
    if (pw.r_squared > min_r2) fits.push_back(pw);

    auto ex = fit_exponential(samples, var, extra_constants);
    if (ex.r_squared > min_r2) fits.push_back(ex);

    auto lg = fit_logarithmic(samples, var, extra_constants);
    if (lg.r_squared > min_r2) fits.push_back(lg);

    auto sn = fit_sinusoidal(samples, var, extra_constants);
    if (sn.r_squared > min_r2) fits.push_back(sn);

    auto rc = fit_reciprocal(samples, var, extra_constants);
    if (rc.r_squared > min_r2) fits.push_back(rc);

    return fits;
}

// Max inners to carry forward per composition level (prevents combinatorial explosion)
constexpr int FIT_MAX_INNERS_PER_LEVEL = 10;

// One level of composition: wrap each inner in outer templates + builtins
inline std::vector<FitResult> compose_level(
        const std::vector<FitSample>& samples,
        const std::vector<FitResult>& inners,
        const std::string& var,
        const std::map<std::string, double>& extra_constants,
        double min_r2, double best_so_far) {
    std::vector<FitResult> new_fits;
    std::set<std::string> seen;

    // Try wrapping each inner in template-based outers
    for (auto& inner : inners) {
        if (!inner.expr) continue;
        auto eval_inner = [&inner, &var](double x) -> double {
            return evaluate(*substitute(inner.expr, var, Expr::Num(x))).value_or_nan();
        };

        std::vector<FitSample> transformed;
        for (auto& s : samples) {
            double ix = eval_inner(s.x);
            if (std::isfinite(ix)) transformed.push_back({ix, s.y});
        }
        if (transformed.size() < 5) continue;

        auto outer_fits = fit_base(transformed, "__inner__", extra_constants, min_r2);
        for (auto& of : outer_fits) {
            if (of.r_squared <= best_so_far + 1e-6) continue;
            if (!of.expr) continue;
            ExprPtr composed = substitute(of.expr, "__inner__", inner.expr);
            composed = simplify(composed);

            std::string cstr = expr_to_string(composed);
            if (!seen.insert(cstr).second) continue;

            FitResult cr;
            cr.degree = -1;
            cr.expr = composed;
            compute_fit_stats(cr, samples, [&eval_inner, &of](double x) {
                double ix = eval_inner(x);
                if (!std::isfinite(ix)) return std::numeric_limits<double>::quiet_NaN();
                return evaluate(*substitute(of.expr, "__inner__", Expr::Num(ix))).value_or_nan();
            });
            if (cr.r_squared > min_r2) new_fits.push_back(cr);
        }

        // Also try builtins as outer wrappers: builtin(inner(x))
        struct OuterBuiltin { std::string name; std::function<double(double)> fn; };
        static const std::vector<OuterBuiltin> outer_builtins = {
            {"sin",  [](double v) { return std::sin(v); }},
            {"cos",  [](double v) { return std::cos(v); }},
            {"sqrt", [](double v) { return v > 0 ? std::sqrt(v) : std::numeric_limits<double>::quiet_NaN(); }},
            {"log",  [](double v) { return v > 0 ? std::log(v) : std::numeric_limits<double>::quiet_NaN(); }},
            {"exp",  [](double v) { double r = std::exp(v); return std::isfinite(r) ? r : std::numeric_limits<double>::quiet_NaN(); }},
        };
        for (auto& ob : outer_builtins) {
            ExprPtr composed;
            if (ob.name == "exp")
                composed = Expr::BinOpExpr(BinOp::POW, Expr::Var("e"), inner.expr);
            else
                composed = Expr::Call(ob.name, {inner.expr});
            std::string cstr = expr_to_string(composed);
            if (!seen.insert(cstr).second) continue;

            FitResult cr;
            cr.degree = -1;
            cr.expr = composed;
            compute_fit_stats(cr, samples, [&eval_inner, &ob](double x) {
                double ix = eval_inner(x);
                if (!std::isfinite(ix)) return std::numeric_limits<double>::quiet_NaN();
                return ob.fn(ix);
            });
            if (cr.r_squared > min_r2) new_fits.push_back(cr);
        }
    }
    return new_fits;
}

// Recursive composition: at each depth level, compose previous fits as inners
inline std::vector<FitResult> fit_all(const std::vector<FitSample>& samples,
        const std::string& var = "x",
        const std::map<std::string, double>& extra_constants = {},
        double min_r2 = 0.9,
        int depth = FIT_DEFAULT_DEPTH) {
    // Level 1: base templates on raw data
    auto fits = fit_base(samples, var, extra_constants, min_r2);

    if (depth <= 1) return sort_and_dedup(fits);

    // Seed inners: builtin functions as simple wrappers
    struct BuiltinInner { std::string name; std::function<double(double)> fn; };
    std::vector<BuiltinInner> builtins = {
        {"sin",  [](double x) { return std::sin(x); }},
        {"cos",  [](double x) { return std::cos(x); }},
        {"sqrt", [](double x) { return x > 0 ? std::sqrt(x) : std::numeric_limits<double>::quiet_NaN(); }},
        {"log",  [](double x) { return x > 0 ? std::log(x) : std::numeric_limits<double>::quiet_NaN(); }},
    };

    // Build initial inners from builtins + products + level-1 fits
    std::vector<FitResult> level_inners;
    for (auto& bi : builtins) {
        // bare builtin: f(x)
        FitResult br;
        br.degree = -1;
        br.expr = Expr::Call(bi.name, {Expr::Var(var)});
        compute_fit_stats(br, samples, bi.fn);
        level_inners.push_back(br);

        // product: x * f(x)
        FitResult pr;
        pr.degree = -1;
        pr.expr = Expr::BinOpExpr(BinOp::MUL, Expr::Var(var),
            Expr::Call(bi.name, {Expr::Var(var)}));
        compute_fit_stats(pr, samples, [&bi](double x) {
            return x * bi.fn(x);
        });
        level_inners.push_back(pr);
    }
    for (const auto& f : fits)
        if (f.r_squared > 0.5 && f.expr) level_inners.push_back(f);

    // Iterate composition levels
    for (int lvl = 2; lvl <= depth; lvl++) {
        double best_so_far = 0;
        for (const auto& f : fits) best_so_far = std::max(best_so_far, f.r_squared);

        auto new_fits = compose_level(samples, level_inners, var,
            extra_constants, min_r2, best_so_far);
        fits.insert(fits.end(), new_fits.begin(), new_fits.end());

        // Prune: keep top N from new fits as inners for next level
        auto pruned = sort_and_dedup(new_fits);
        level_inners.clear();
        for (size_t i = 0; i < pruned.size() && static_cast<int>(i) < FIT_MAX_INNERS_PER_LEVEL; i++)
            if (pruned[i].expr) level_inners.push_back(pruned[i]);

        if (level_inners.empty()) break; // no new compositions found
    }

    return sort_and_dedup(fits);
}
