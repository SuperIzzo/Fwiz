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

static_assert(FIT_DEFAULT_SAMPLES >= 50);
static_assert(FIT_MAX_DEGREE >= 1 && FIT_MAX_DEGREE <= 20);
static_assert(FIT_R2_THRESHOLD > 0.99 && FIT_R2_THRESHOLD <= 1.0);

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

// Compute R² and max error for a polynomial fit
inline void compute_fit_stats(FitResult& result, const std::vector<FitSample>& samples) {
    double y_mean = 0;
    for (auto& s : samples) y_mean += s.y;
    y_mean /= static_cast<double>(samples.size());

    double ss_res = 0, ss_tot = 0;
    result.max_error = 0;
    for (auto& s : samples) {
        double predicted = poly_eval(result.coefficients, s.x);
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
    for (auto& s : samples) b.push_back(s.y);

    result.coefficients = least_squares_solve(A, b);

    // Snap coefficients to integers
    for (auto& c : result.coefficients) c = snap_coeff(c);

    compute_fit_stats(result, samples);
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
//  Expression tree construction
// ============================================================================

// Build ExprPtr from polynomial coefficients: c[0] + c[1]*x + c[2]*x² + ...
// Requires active ExprArena::Scope. Drops near-zero coefficients.
inline ExprPtr poly_to_expr(const std::vector<double>& coeffs, const std::string& var) {
    ExprPtr result = nullptr;

    for (size_t i = 0; i < coeffs.size(); i++) {
        if (std::abs(coeffs[i]) < EPSILON_ZERO) continue;

        ExprPtr term;
        if (i == 0) {
            term = Expr::Num(coeffs[i]);
        } else {
            ExprPtr x_part;
            if (i == 1) {
                x_part = Expr::Var(var);
            } else {
                x_part = Expr::BinOpExpr(BinOp::POW, Expr::Var(var), Expr::Num(static_cast<double>(i)));
            }

            if (coeffs[i] == 1.0) {
                term = x_part;
            } else if (coeffs[i] == -1.0) {
                term = Expr::Neg(x_part);
            } else {
                term = Expr::BinOpExpr(BinOp::MUL, Expr::Num(coeffs[i]), x_part);
            }
        }

        if (!result) {
            result = term;
        } else {
            result = Expr::BinOpExpr(BinOp::ADD, result, term);
        }
    }

    if (!result) result = Expr::Num(0);
    return simplify(result);
}
