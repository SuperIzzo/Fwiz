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

// Negation helpers
inline bool is_neg_num(const ExprPtr& e) { return e->type == ExprType::NUM && e->num < 0; }
inline bool is_neg(const ExprPtr& e)     { return e->type == ExprType::UNARY_NEG; }
inline bool is_negative(const ExprPtr& e){ return is_neg(e) || is_neg_num(e); }

inline ExprPtr strip_neg(const ExprPtr& e) {
    if (e->type == ExprType::UNARY_NEG) return e->child;
    if (e->type == ExprType::NUM && e->num < 0) return Expr::Num(-e->num);
    return nullptr;
}

// For MUL and DIV: factor out negation from operands
inline ExprPtr simplify_neg_pair(BinOp op, const ExprPtr& l, const ExprPtr& r) {
    if (is_negative(l) && is_negative(r))
        return Expr::BinOpExpr(op, strip_neg(l), strip_neg(r));
    if (is_neg(l) && !is_negative(r))
        return Expr::Neg(Expr::BinOpExpr(op, l->child, r));
    if (is_neg(r) && !is_negative(l))
        return Expr::Neg(Expr::BinOpExpr(op, l, r->child));
    return nullptr;
}

inline ExprPtr simplify_once(const ExprPtr& e) {
    if (!e) return e;
    switch (e->type) {
        case ExprType::NUM:
        case ExprType::VAR:
            return e;

        case ExprType::UNARY_NEG: {
            auto c = simplify_once(e->child);
            if (c->type == ExprType::NUM)                             return Expr::Num(-c->num);
            if (c->type == ExprType::UNARY_NEG)                       return c->child;
            if (c->type == ExprType::BINOP && c->op == BinOp::SUB)    return Expr::BinOpExpr(BinOp::SUB, c->right, c->left);
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
            auto L = [&](){ return l->type == ExprType::NUM; };
            auto R = [&](){ return r->type == ExprType::NUM; };

            // Constant fold
            if (L() && R()) return Expr::Num(evaluate(Expr::BinOpExpr(e->op, l, r)));

            // Constant reassociation: (a ⊕ K1) ⊗ K2 → a ⊕ combine(K1, K2)
            if (R() && l->type == ExprType::BINOP && l->right->type == ExprType::NUM) {
                double k1 = l->right->num, k2 = r->num;
                auto a = l->left;
                // ADD+ADD, SUB+ADD, ADD+SUB, SUB+SUB, MUL+MUL
                if (e->op == BinOp::ADD && l->op == BinOp::ADD) return Expr::BinOpExpr(BinOp::ADD, a, Expr::Num(k1 + k2));
                if (e->op == BinOp::ADD && l->op == BinOp::SUB) return Expr::BinOpExpr(BinOp::ADD, a, Expr::Num(k2 - k1));
                if (e->op == BinOp::SUB && l->op == BinOp::ADD) return Expr::BinOpExpr(BinOp::ADD, a, Expr::Num(k1 - k2));
                if (e->op == BinOp::SUB && l->op == BinOp::SUB) return Expr::BinOpExpr(BinOp::SUB, a, Expr::Num(k1 + k2));
                if (e->op == BinOp::MUL && l->op == BinOp::MUL) return Expr::BinOpExpr(BinOp::MUL, a, Expr::Num(k1 * k2));
            }

            switch (e->op) {
                case BinOp::ADD:
                    if (L() && l->num == 0) return r;
                    if (R() && r->num == 0) return l;
                    if (is_neg(r))     return Expr::BinOpExpr(BinOp::SUB, l, r->child);
                    if (is_neg_num(r)) return Expr::BinOpExpr(BinOp::SUB, l, Expr::Num(-r->num));
                    if (is_neg(l))     return Expr::BinOpExpr(BinOp::SUB, r, l->child);
                    if (is_neg_num(l)) return Expr::BinOpExpr(BinOp::SUB, r, Expr::Num(-l->num));
                    break;

                case BinOp::SUB:
                    if (R() && r->num == 0) return l;
                    if (L() && l->num == 0) return Expr::Neg(r);
                    if (is_neg(r))     return Expr::BinOpExpr(BinOp::ADD, l, r->child);
                    if (is_neg_num(r)) return Expr::BinOpExpr(BinOp::ADD, l, Expr::Num(-r->num));
                    break;

                case BinOp::MUL:
                    if (L() && l->num == 0) return Expr::Num(0);
                    if (R() && r->num == 0) return Expr::Num(0);
                    if (L() && l->num == 1) return r;
                    if (R() && r->num == 1) return l;
                    if (L() && l->num == -1) return Expr::Neg(r);
                    if (R() && r->num == -1) return Expr::Neg(l);
                    if (auto s = simplify_neg_pair(BinOp::MUL, l, r)) return s;
                    break;

                case BinOp::DIV:
                    if (L() && l->num == 0) return Expr::Num(0);
                    if (R() && r->num == 1) return l;
                    if (R() && r->num == -1) return Expr::Neg(l);
                    if (auto s = simplify_neg_pair(BinOp::DIV, l, r)) return s;
                    break;

                case BinOp::POW:
                    if (R() && r->num == 0) return Expr::Num(1);
                    if (R() && r->num == 1) return l;
                    break;
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
