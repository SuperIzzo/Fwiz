#pragma once
#include <string>
#include <memory>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <set>
#include <map>
#include <vector>
#include <optional>
#include <stdexcept>
#include <functional>

// Thresholds used throughout the solver
constexpr double EPSILON_ZERO = 1e-12;   // treat |x| < this as zero (coefficient guard, like-term combining)
constexpr double EPSILON_REL  = 1e-9;    // relative tolerance for verify mode (approx_equal)
constexpr int    SIMPLIFY_MAX_ITER = 20; // fixpoint loop limit for simplify()

// ============================================================================
//  Expression tree
// ============================================================================

enum class ExprType { NUM, VAR, BINOP, UNARY_NEG, FUNC_CALL };
enum class BinOp   { ADD, SUB, MUL, DIV, POW };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
    ExprType type;
    double num = 0;
    std::string name;
    BinOp op{};
    ExprPtr left, right, child;
    std::vector<ExprPtr> args;

    static ExprPtr Num(double v)                                   { auto e = std::make_shared<Expr>(); e->type = ExprType::NUM;       e->num = v;                              return e; }
    static ExprPtr Var(const std::string& n)                       { auto e = std::make_shared<Expr>(); e->type = ExprType::VAR;       e->name = n;                             return e; }
    static ExprPtr BinOpExpr(BinOp o, ExprPtr l, ExprPtr r)        { auto e = std::make_shared<Expr>(); e->type = ExprType::BINOP;     e->op = o; e->left = l; e->right = r;   return e; }
    static ExprPtr Neg(ExprPtr c)                                  { auto e = std::make_shared<Expr>(); e->type = ExprType::UNARY_NEG; e->child = c;                            return e; }
    static ExprPtr Call(const std::string& n, std::vector<ExprPtr> a) { auto e = std::make_shared<Expr>(); e->type = ExprType::FUNC_CALL; e->name = n; e->args = std::move(a); return e; }
};

// ============================================================================
//  Type predicates
// ============================================================================

inline bool is_num(const ExprPtr& e)    { return e && e->type == ExprType::NUM; }
inline bool is_var(const ExprPtr& e)    { return e && e->type == ExprType::VAR; }
inline bool is_atomic(const ExprPtr& e) { return is_num(e) || is_var(e); }
inline bool is_zero(const ExprPtr& e)   { return is_num(e) && e->num == 0; }
inline bool is_one(const ExprPtr& e)    { return is_num(e) && e->num == 1; }
inline bool is_neg_one(const ExprPtr& e){ return is_num(e) && e->num == -1; }
inline bool is_neg(const ExprPtr& e)    { return e && e->type == ExprType::UNARY_NEG; }
inline bool is_neg_num(const ExprPtr& e){ return is_num(e) && e->num < 0; }

inline bool is_additive(BinOp op)       { return op == BinOp::ADD || op == BinOp::SUB; }
inline bool is_multiplicative(BinOp op) { return op == BinOp::MUL || op == BinOp::DIV; }

// ============================================================================
//  BinOp metadata
// ============================================================================

struct BinOpInfo {
    const char* symbol;
    int precedence;
    std::function<double(double, double)> eval;
};

inline const BinOpInfo& binop_info(BinOp op) {
    static const BinOpInfo table[] = {
        {" + ", 1, [](double l, double r) { return l + r; }},
        {" - ", 1, [](double l, double r) { return l - r; }},
        {" * ", 2, [](double l, double r) { return l * r; }},
        {" / ", 2, [](double l, double r) { if (r == 0) throw std::runtime_error("Division by zero"); return l / r; }},
        {"^",   4, [](double l, double r) { return std::pow(l, r); }},
    };
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
//  Tree queries
// ============================================================================

inline void collect_vars(const ExprPtr& e, std::set<std::string>& out) {
    if (!e) return;
    switch (e->type) {
        case ExprType::NUM:       break;
        case ExprType::VAR:       out.insert(e->name); break;
        case ExprType::BINOP:     collect_vars(e->left, out); collect_vars(e->right, out); break;
        case ExprType::UNARY_NEG: collect_vars(e->child, out); break;
        case ExprType::FUNC_CALL: for (auto& a : e->args) collect_vars(a, out); break;
    }
}

// Direct search — no allocation, returns at first hit
inline bool contains_var(const ExprPtr& e, const std::string& v) {
    if (!e) return false;
    switch (e->type) {
        case ExprType::NUM:       return false;
        case ExprType::VAR:       return e->name == v;
        case ExprType::BINOP:     return contains_var(e->left, v) || contains_var(e->right, v);
        case ExprType::UNARY_NEG: return contains_var(e->child, v);
        case ExprType::FUNC_CALL: for (auto& a : e->args) if (contains_var(a, v)) return true;
                                  return false;
    }
    return false;
}

// Structural equality — no allocation, used for simplifier fixpoint
inline bool expr_equal(const ExprPtr& a, const ExprPtr& b) {
    if (a.get() == b.get()) return true;    // pointer shortcut
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    switch (a->type) {
        case ExprType::NUM:       return a->num == b->num;
        case ExprType::VAR:       return a->name == b->name;
        case ExprType::UNARY_NEG: return expr_equal(a->child, b->child);
        case ExprType::BINOP:     return a->op == b->op
                                      && expr_equal(a->left, b->left)
                                      && expr_equal(a->right, b->right);
        case ExprType::FUNC_CALL:
            if (a->name != b->name || a->args.size() != b->args.size()) return false;
            for (size_t i = 0; i < a->args.size(); i++)
                if (!expr_equal(a->args[i], b->args[i])) return false;
            return true;
    }
    return false;
}

// ============================================================================
//  Formatting
// ============================================================================

inline std::string fmt_num(double v) {
    if (std::abs(v) < 1e12 && v == static_cast<long long>(v))
        return std::to_string(static_cast<long long>(v));
    std::ostringstream os;
    os << std::setprecision(10) << v;
    return os.str();
}

inline int precedence(const ExprPtr& e) {
    if (!e) return 0;
    if (e->type == ExprType::BINOP) return binop_info(e->op).precedence;
    if (e->type == ExprType::UNARY_NEG) return 3;
    return 5; // atom
}

inline std::string expr_to_string(const ExprPtr& e) {
    if (!e) return "?";
    switch (e->type) {
        case ExprType::NUM:
            return (e->num < 0) ? "(" + fmt_num(e->num) + ")" : fmt_num(e->num);

        case ExprType::VAR:
            return e->name;

        case ExprType::UNARY_NEG: {
            return is_atomic(e->child)
                ? "-" + expr_to_string(e->child)
                : "-(" + expr_to_string(e->child) + ")";
        }

        case ExprType::BINOP: {
            auto& info = binop_info(e->op);
            int prec = info.precedence;

            auto wrap = [&](const ExprPtr& child, bool rhs) {
                int cp = precedence(child);
                bool need = (cp < prec) ||
                    (cp == prec && rhs && (e->op == BinOp::SUB || e->op == BinOp::DIV));
                auto s = expr_to_string(child);
                return need ? "(" + s + ")" : s;
            };
            return wrap(e->left, false) + info.symbol + wrap(e->right, true);
        }

        case ExprType::FUNC_CALL: {
            std::string s = e->name + "(";
            for (size_t i = 0; i < e->args.size(); i++)
                s += (i ? ", " : "") + expr_to_string(e->args[i]);
            return s + ")";
        }
    }
    return "?";
}

// ============================================================================
//  Substitute
// ============================================================================

inline ExprPtr substitute(const ExprPtr& e, const std::string& var, const ExprPtr& val) {
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
            for (auto& arg : e->args) a.push_back(substitute(arg, var, val));
            return Expr::Call(e->name, a);
        }
    }
    return e;
}

// ============================================================================
//  Evaluate
// ============================================================================

inline double evaluate(const ExprPtr& e) {
    if (!e) throw std::runtime_error("Cannot evaluate null expression");
    switch (e->type) {
        case ExprType::NUM: return e->num;
        case ExprType::VAR:
            throw std::runtime_error("Cannot evaluate: unresolved variable '" + e->name + "'");
        case ExprType::UNARY_NEG:
            return -evaluate(e->child);
        case ExprType::BINOP:
            return binop_info(e->op).eval(evaluate(e->left), evaluate(e->right));
        case ExprType::FUNC_CALL: {
            if (e->args.size() != 1)
                throw std::runtime_error("Unknown function: " + e->name);
            auto& registry = builtin_functions();
            auto it = registry.find(e->name);
            if (it == registry.end())
                throw std::runtime_error("Unknown function: " + e->name);
            return it->second(evaluate(e->args[0]));
        }
    }
    throw std::runtime_error("Cannot evaluate: unknown expression type");
}

// ============================================================================
//  Simplify
// ============================================================================

// ---- Flattening helpers ----

// Decompose expr into (base, exponent) — e.g. x^3 → (x, 3), x → (x, 1)
inline std::pair<ExprPtr, double> split_pow(const ExprPtr& e) {
    if (e->type == ExprType::BINOP && e->op == BinOp::POW
        && e->right->type == ExprType::NUM)
        return {e->left, e->right->num};
    return {e, 1.0};
}

// Flatten an additive chain (ADD/SUB) into (coefficient, base) terms.
// Each term represents coeff * base. Bare constants have base=nullptr.
inline void flatten_additive(const ExprPtr& e, double sign,
                             std::vector<std::pair<double, ExprPtr>>& terms) {
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
inline void flatten_multiplicative(const ExprPtr& e,
                                   double& coeff,
                                   std::vector<std::pair<ExprPtr, double>>& factors) {
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
inline void group_additive(std::vector<std::pair<double, ExprPtr>>& terms) {
    for (size_t i = 0; i < terms.size(); i++) {
        if (!terms[i].second) continue;
        for (size_t j = i + 1; j < terms.size(); j++) {
            if (!terms[j].second) continue;
            if (expr_equal(terms[i].second, terms[j].second)) {
                terms[i].first += terms[j].first;
                terms[j].first = 0;
                terms[j].second = nullptr;
            }
        }
    }
}

// Group multiplicative factors by base, combining exponents
inline void group_multiplicative(std::vector<std::pair<ExprPtr, double>>& factors) {
    for (size_t i = 0; i < factors.size(); i++) {
        if (!factors[i].first) continue;
        for (size_t j = i + 1; j < factors.size(); j++) {
            if (!factors[j].first) continue;
            if (expr_equal(factors[i].first, factors[j].first)) {
                factors[i].second += factors[j].second;
                factors[j].second = 0;
                factors[j].first = nullptr;
            }
        }
    }
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
            if (expr_equal(lb, rb)) { le -= re; re = 0; rb = nullptr; changed = true; }
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
                    return Expr::BinOpExpr(BinOp::POW, l, r);
            }
        }
    }
    return e;
}

inline ExprPtr simplify(const ExprPtr& e) {
    ExprPtr cur = e;
    for (int i = 0; i < SIMPLIFY_MAX_ITER; i++) {
        auto next = simplify_once(cur);
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
            }
            break;

        case ExprType::FUNC_CALL:
            return contains_var(e, t) ? fail() : ok(Expr::Num(0), e);
    }
    return fail();
}

inline ExprPtr solve_for(const ExprPtr& lhs, const ExprPtr& rhs, const std::string& target) {
    auto combined = simplify(Expr::BinOpExpr(BinOp::SUB, lhs, rhs));
    auto decomp = decompose_linear(combined, target);
    if (!decomp) return nullptr;

    auto sc = simplify(decomp->coeff);
    if (is_num(sc) && std::abs(sc->num) < EPSILON_ZERO) return nullptr;

    auto sr = simplify(decomp->rest);
    // If rest is zero and coeff is symbolic, the equation is coeff * target = 0.
    // Reject to avoid spurious zeros from underdetermined systems.
    if (is_num(sr) && std::abs(sr->num) < EPSILON_ZERO && !is_num(sc))
        return nullptr;

    // coeff · target + rest = 0  →  target = −rest / coeff
    return simplify(Expr::BinOpExpr(BinOp::DIV, simplify(Expr::Neg(sr)), sc));
}
