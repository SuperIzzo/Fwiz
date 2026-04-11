#pragma once
#include <string>
#include <memory>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <set>
#include <vector>
#include <stdexcept>

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
    switch (e->type) {
        case ExprType::BINOP:
            switch (e->op) {
                case BinOp::ADD: case BinOp::SUB: return 1;
                case BinOp::MUL: case BinOp::DIV: return 2;
                case BinOp::POW: return 4;
            }
            break;
        case ExprType::UNARY_NEG: return 3;
        default: break;
    }
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
            auto& c = e->child;
            bool simple = (c->type == ExprType::NUM || c->type == ExprType::VAR);
            return simple ? "-" + expr_to_string(c) : "-(" + expr_to_string(c) + ")";
        }

        case ExprType::BINOP: {
            static const char* ops[] = {" + ", " - ", " * ", " / ", "^"};
            int prec = precedence(e);

            auto wrap = [&](const ExprPtr& child, bool rhs) {
                int cp = precedence(child);
                bool need = (cp < prec) ||
                    (cp == prec && rhs && (e->op == BinOp::SUB || e->op == BinOp::DIV));
                auto s = expr_to_string(child);
                return need ? "(" + s + ")" : s;
            };
            return wrap(e->left, false) + ops[static_cast<int>(e->op)] + wrap(e->right, true);
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
        case ExprType::BINOP: {
            double l = evaluate(e->left), r = evaluate(e->right);
            switch (e->op) {
                case BinOp::ADD: return l + r;
                case BinOp::SUB: return l - r;
                case BinOp::MUL: return l * r;
                case BinOp::DIV: if (r == 0) throw std::runtime_error("Division by zero");
                                 return l / r;
                case BinOp::POW: return std::pow(l, r);
            }
            break;
        }
        case ExprType::FUNC_CALL: {
            using F = double(*)(double);
            static const struct { const char* name; F fn; } builtins[] = {
                {"sqrt", std::sqrt}, {"abs", std::fabs}, {"sin",  std::sin},
                {"cos",  std::cos},  {"tan", std::tan},  {"log",  std::log},
                {"asin", std::asin}, {"acos",std::acos}, {"atan", std::atan}
            };
            if (e->args.size() == 1) {
                double arg = evaluate(e->args[0]);
                for (auto& [name, fn] : builtins)
                    if (e->name == name) return fn(arg);
            }
            throw std::runtime_error("Unknown function: " + e->name);
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
        if (std::abs(coeff) < 1e-12) continue; // skip zero terms
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
        if (std::abs(exp) < 1e-12) continue; // base^0 = 1, skip
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

// ---- Simplify ----

inline ExprPtr simplify_once(const ExprPtr& e) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return e;

        case ExprType::UNARY_NEG: {
            auto c = simplify_once(e->child);
            if (c->type == ExprType::NUM) return Expr::Num(-c->num);
            if (c->type == ExprType::UNARY_NEG) return c->child;
            // Negate an additive expression: flatten with -1 sign
            if (c->type == ExprType::BINOP && (c->op == BinOp::ADD || c->op == BinOp::SUB)) {
                std::vector<std::pair<double, ExprPtr>> terms;
                flatten_additive(c, -1.0, terms);
                double constant = 0;
                std::vector<std::pair<double, ExprPtr>> symbolic;
                for (auto& [coeff, base] : terms) {
                    if (!base) constant += coeff;
                    else symbolic.push_back({coeff, base});
                }
                group_additive(symbolic);
                if (std::abs(constant) >= 1e-12)
                    symbolic.push_back({constant, nullptr});
                return rebuild_additive(symbolic);
            }
            // Negate a division: push negation through
            if (c->type == ExprType::BINOP && c->op == BinOp::DIV) {
                // -(a/b) → (-a)/b, let DIV rules handle double negation
                return Expr::BinOpExpr(BinOp::DIV, Expr::Neg(c->left), c->right);
            }
            return Expr::Neg(c);
        }

        case ExprType::FUNC_CALL: {
            std::vector<ExprPtr> sa;
            bool all_num = true;
            for (auto& a : e->args) {
                sa.push_back(simplify_once(a));
                if (sa.back()->type != ExprType::NUM) all_num = false;
            }
            auto s = Expr::Call(e->name, sa);
            return all_num ? Expr::Num(evaluate(s)) : s;
        }

        case ExprType::BINOP: {
            auto l = simplify_once(e->left);
            auto r = simplify_once(e->right);

            // Constant fold
            if (l->type == ExprType::NUM && r->type == ExprType::NUM)
                return Expr::Num(evaluate(Expr::BinOpExpr(e->op, l, r)));

            if (e->op == BinOp::ADD || e->op == BinOp::SUB) {
                // Flatten, group, rebuild
                auto combined = Expr::BinOpExpr(e->op, l, r);
                std::vector<std::pair<double, ExprPtr>> terms;
                flatten_additive(combined, 1.0, terms);

                // Collect bare constants
                double constant = 0;
                std::vector<std::pair<double, ExprPtr>> symbolic;
                for (auto& [c, b] : terms) {
                    if (!b) constant += c;
                    else    symbolic.push_back({c, b});
                }

                // Group like terms
                group_additive(symbolic);

                // Add constant back if nonzero
                if (std::abs(constant) >= 1e-12)
                    symbolic.push_back({constant, nullptr});

                return rebuild_additive(symbolic);
            }

            if (e->op == BinOp::MUL) {
                // Check for zero
                if ((l->type == ExprType::NUM && l->num == 0) ||
                    (r->type == ExprType::NUM && r->num == 0))
                    return Expr::Num(0);

                // Flatten MUL chain, group, rebuild
                auto combined = Expr::BinOpExpr(BinOp::MUL, l, r);
                double coeff = 1.0;
                std::vector<std::pair<ExprPtr, double>> factors;
                flatten_multiplicative(combined, coeff, factors);
                group_multiplicative(factors);
                return rebuild_multiplicative(coeff, factors);
            }

            if (e->op == BinOp::DIV) {
                // 0 / x → 0
                if (l->type == ExprType::NUM && l->num == 0) return Expr::Num(0);
                // x / 1 → x
                if (r->type == ExprType::NUM && r->num == 1) return l;
                // x / -1 → -x
                if (r->type == ExprType::NUM && r->num == -1) return Expr::Neg(l);
                // Negate denominator: x / (-K) → -x / K, then recurse
                if (r->type == ExprType::NUM && r->num < 0)
                    return Expr::BinOpExpr(BinOp::DIV, Expr::Neg(l), Expr::Num(-r->num));
                // Negate both: (-a) / (-b) → a / b
                if (l->type == ExprType::UNARY_NEG && r->type == ExprType::UNARY_NEG)
                    return Expr::BinOpExpr(BinOp::DIV, l->child, r->child);
                // Single neg: (-a) / b → -(a/b)
                if (l->type == ExprType::UNARY_NEG)
                    return Expr::Neg(Expr::BinOpExpr(BinOp::DIV, l->child, r));
                // Constant reassociation: (K1 * a) / K2 or (a * K1) / K2 → a * (K1/K2)
                if (r->type == ExprType::NUM && l->type == ExprType::BINOP && l->op == BinOp::MUL) {
                    if (l->right->type == ExprType::NUM)
                        return Expr::BinOpExpr(BinOp::MUL, l->left, Expr::Num(l->right->num / r->num));
                    if (l->left->type == ExprType::NUM)
                        return Expr::BinOpExpr(BinOp::MUL, Expr::Num(l->left->num / r->num), l->right);
                }
                // (a / K1) / K2 → a / (K1*K2)
                if (r->type == ExprType::NUM && l->type == ExprType::BINOP
                    && l->op == BinOp::DIV && l->right->type == ExprType::NUM)
                    return Expr::BinOpExpr(BinOp::DIV, l->left,
                        Expr::Num(l->right->num * r->num));
                // Self-division and power-aware: flatten both sides, cancel
                {
                    double lc = 1.0, rc = 1.0;
                    std::vector<std::pair<ExprPtr, double>> lf, rf;
                    flatten_multiplicative(l, lc, lf);
                    flatten_multiplicative(r, rc, rf);
                    // Cancel matching bases
                    bool changed = false;
                    for (auto& [lb, le] : lf) {
                        if (!lb) continue;
                        for (auto& [rb, re] : rf) {
                            if (!rb) continue;
                            if (expr_equal(lb, rb)) {
                                le -= re;
                                re = 0;
                                rb = nullptr;
                                changed = true;
                            }
                        }
                    }
                    if (changed) {
                        auto top = rebuild_multiplicative(lc, lf);
                        auto bot = rebuild_multiplicative(rc, rf);
                        // If denominator simplified to 1, just return numerator
                        if (bot->type == ExprType::NUM && bot->num == 1.0) return top;
                        if (bot->type == ExprType::NUM && bot->num == -1.0) return Expr::Neg(top);
                        return Expr::BinOpExpr(BinOp::DIV, top, bot);
                    }
                }
                return Expr::BinOpExpr(BinOp::DIV, l, r);
            }

            if (e->op == BinOp::POW) {
                if (r->type == ExprType::NUM && r->num == 0) return Expr::Num(1);
                if (r->type == ExprType::NUM && r->num == 1) return l;
            }

            return Expr::BinOpExpr(e->op, l, r);
        }
    }
    return e;
}

inline ExprPtr simplify(const ExprPtr& e) {
    ExprPtr cur = e;
    for (int i = 0; i < 20; i++) {
        auto next = simplify_once(cur);
        if (expr_equal(next, cur)) break;
        cur = next;
    }
    return cur;
}

// ============================================================================
//  Linear solver: decompose expr into coeff * target + rest
// ============================================================================

struct LinearForm { ExprPtr coeff, rest; bool ok; };

inline LinearForm decompose_linear(const ExprPtr& e, const std::string& t) {
    if (!e) return {Expr::Num(0), Expr::Num(0), true};

    auto ok = [](ExprPtr c, ExprPtr r) { return LinearForm{c, r, true}; };
    auto fail = []() { return LinearForm{nullptr, nullptr, false}; };
    auto S = simplify;

    switch (e->type) {
        case ExprType::NUM:
            return ok(Expr::Num(0), e);

        case ExprType::VAR:
            return (e->name == t) ? ok(Expr::Num(1), Expr::Num(0))
                                  : ok(Expr::Num(0), e);

        case ExprType::UNARY_NEG: {
            auto [c, r, v] = decompose_linear(e->child, t);
            return v ? ok(S(Expr::Neg(c)), S(Expr::Neg(r))) : fail();
        }

        case ExprType::BINOP:
            switch (e->op) {
                case BinOp::ADD: case BinOp::SUB: {
                    auto [lc, lr, lv] = decompose_linear(e->left, t);
                    auto [rc, rr, rv] = decompose_linear(e->right, t);
                    if (!lv || !rv) return fail();
                    return ok(S(Expr::BinOpExpr(e->op, lc, rc)),
                              S(Expr::BinOpExpr(e->op, lr, rr)));
                }
                case BinOp::MUL: {
                    bool lh = contains_var(e->left, t), rh = contains_var(e->right, t);
                    if (lh && rh) return fail();
                    if (!lh && !rh) return ok(Expr::Num(0), e);
                    auto [side, factor] = lh ? std::pair{e->left, e->right}
                                             : std::pair{e->right, e->left};
                    auto [c, r, v] = decompose_linear(side, t);
                    return v ? ok(S(Expr::BinOpExpr(BinOp::MUL, factor, c)),
                                  S(Expr::BinOpExpr(BinOp::MUL, factor, r))) : fail();
                }
                case BinOp::DIV: {
                    if (contains_var(e->right, t)) return fail();
                    auto [c, r, v] = decompose_linear(e->left, t);
                    return v ? ok(S(Expr::BinOpExpr(BinOp::DIV, c, e->right)),
                                  S(Expr::BinOpExpr(BinOp::DIV, r, e->right))) : fail();
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
    auto [coeff, rest, ok] = decompose_linear(combined, target);
    if (!ok) return nullptr;

    auto sc = simplify(coeff);
    if (sc->type == ExprType::NUM && std::abs(sc->num) < 1e-12) return nullptr;

    auto sr = simplify(rest);
    // If rest is zero and coeff is symbolic, the equation is coeff * target = 0.
    // This yields the trivial solution target = 0, but the symbolic coeff might
    // itself be zero (making 0 = 0, a tautology with infinite solutions).
    // Reject to avoid spurious zeros from underdetermined systems.
    if (sr->type == ExprType::NUM && std::abs(sr->num) < 1e-12 && sc->type != ExprType::NUM)
        return nullptr;

    // coeff · target + rest = 0  →  target = −rest / coeff
    return simplify(Expr::BinOpExpr(BinOp::DIV, simplify(Expr::Neg(sr)), sc));
}
