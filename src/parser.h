#pragma once
#include "lexer.h"
#include "expr.h"
#include <iostream>

// Thrown by the parser on a structurally-invalid vec/mat literal (e.g. a
// ragged matrix `[[1, 2], [3]]`). Intentionally NOT derived from
// std::runtime_error so the per-line warning catch in `load_lines`
// doesn't swallow it — ragged-literal errors must propagate to the
// top-level caller so the user sees the diagnostic instead of "Cannot
// solve for X". Same precedent as SolveBudgetExceededError and
// CrossFileResolutionCycleError in system.h.
struct RaggedMatrixError : std::exception {
    std::string msg;
    explicit RaggedMatrixError(std::string m) : msg(std::move(m)) {}
    [[nodiscard]] const char* what() const noexcept override { return msg.c_str(); }
};

// Grammar lock-in violation in `var:(atom1, atom2, ...) = expr` intersection
// annotations: only IDENT/COMMA permitted inside parens (see gen-3 cycle 2
// design D10). A sibling exception (not std::runtime_error) so it propagates
// through `load_lines`' per-line resilience to the load_string caller — same
// pattern as `RaggedMatrixError` (#13) and `CrossFileResolutionCycleError` (#69).
struct BindingAnnotationError : std::exception {
    std::string msg;
    explicit BindingAnnotationError(std::string m) : msg(std::move(m)) {}
    [[nodiscard]] const char* what() const noexcept override { return msg.c_str(); }
};

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tok_(tokens), pos_(0) {}

    ExprPtr parse_expr() { return additive(); }
    [[nodiscard]] bool at_end() const { return tok_[pos_].type == TokenType::END; }

private:
    std::vector<Token> tok_;
    size_t pos_;

    [[nodiscard]] const Token& peek() const                { return tok_[pos_]; }
    const Token& advance()                   { return tok_[pos_++]; }
    [[nodiscard]] bool is(TokenType t) const               { return peek().type == t; }
    [[nodiscard]] bool is(TokenType a, TokenType b) const  { return is(a) || is(b); }

    void expect(TokenType t, const char* msg) {
        if (!is(t)) throw std::runtime_error(std::string(msg) + " (got '" + peek().text + "')");
        advance();
    }

    // Grammar: additive → multiplicative ((+|-) multiplicative)*
    ExprPtr additive() {
        auto node = multiplicative();
        while (is(TokenType::PLUS, TokenType::MINUS)) {
            auto op = (advance().type == TokenType::PLUS) ? BinOp::ADD : BinOp::SUB;
            node = Expr::BinOpExpr(op, node, multiplicative());
        }
        return node;
    }

    // multiplicative → unary ((*|/) unary)*
    ExprPtr multiplicative() {
        auto node = unary();
        while (is(TokenType::STAR, TokenType::SLASH)) {
            auto op = (advance().type == TokenType::STAR) ? BinOp::MUL : BinOp::DIV;
            node = Expr::BinOpExpr(op, node, unary());
        }
        return node;
    }

    // unary → -unary | power
    ExprPtr unary() {
        if (is(TokenType::MINUS)) { advance(); return Expr::Neg(unary()); }
        return power();
    }

    // power → primary (^ unary)?     — right-associative via unary on RHS
    ExprPtr power() {
        auto node = primary();
        if (is(TokenType::CARET)) {
            advance();
            node = Expr::BinOpExpr(BinOp::POW, node, unary());
        }
        return node;
    }

    // primary → NUMBER | IDENT (LPAREN args RPAREN)? | LPAREN expr RPAREN | LBRACKET vec/mat RBRACKET
    ExprPtr primary() {
        if (is(TokenType::NUMBER)) {
            const double v = peek().numval;
            const std::string num_text = peek().text;
            advance();
            // Units cycle 1 (Future #7): NUMBER+IDENT adjacency -> `MUL(Num, Var)`,
            // or `MUL(Num, FUNC_CALL)` when the IDENT is immediately followed by
            // `(`. Identifier IS a regular Var — stdlib `.fw` files supply
            // semantics via plain bindings like `kg = 1`. Design proposal §"Final
            // Design — Units cycle 1" Shape A. Scientific notation (`100e3`)
            // is already consumed by `read_number` so we never see NUMBER+IDENT("e3")
            // here.
            if (is(TokenType::IDENT)) {
                const std::string ident_name = peek().text;
                // Reserved-word denylist for the NUMBER-IDENT desugar (Future
                // #76): language keywords (`if`, `iff`) and the Euler constant
                // `e` cannot serve as unit suffixes. Without this guard,
                // `y = 2if` would desugar to `MUL(2, Var("if"))` — absorbing
                // the keyword into arithmetic — and `2e` would silently mean
                // `2 * Euler` even when the user typed an incomplete scientific
                // literal (`2e0`, `2e+1`). LEAVE the IDENT in the token stream
                // (do NOT advance) so the caller / parse_line keyword scanner
                // or the outer expression can pick it up appropriately. Builtin
                // `i` is intentionally NOT denylisted: `2i` is the canonical
                // complex-literal pattern and the simplifier's `i*i = -1` rule
                // composes through MUL correctly. `pi` and `phi` are also kept
                // free for `2pi` / `3phi` shorthand.
                if (ident_name == "if" || ident_name == "iff"
                    || ident_name == "e") {
                    return Expr::Num(v);
                }
                advance();
                ExprPtr rhs;
                if (is(TokenType::LPAREN)) {
                    advance();
                    std::vector<ExprPtr> args;
                    if (!is(TokenType::RPAREN)) {
                        args.push_back(parse_expr());
                        while (is(TokenType::COMMA)) { advance(); args.push_back(parse_expr()); }
                    }
                    expect(TokenType::RPAREN, "Expected ')'");
                    rhs = Expr::Call(ident_name, args);
                } else {
                    // Parse-time warning: `100m^2` adjacency is the precedence
                    // trap (Future #74) — desugar wraps the NUMBER+IDENT pair,
                    // so POW applies to the MUL node and the user gets
                    // `(100 * m)^2`, not `100 * m^2`. Warn so the user notices.
                    // Discriminator: warning fires only when the IDENT is NOT
                    // followed by `(` (function-call branch above), since
                    // `100sin(x)^2` is mathematically `100 * sin(x)^2` which
                    // IS what the user wants.
                    if (is(TokenType::CARET)) {
                        std::cerr << "warning: '" << num_text << ident_name
                                  << "^...' parses as (" << num_text << " * "
                                  << ident_name << ")^... — write '"
                                  << num_text << " * " << ident_name
                                  << "^...' for the mathematical "
                                  << num_text << " * " << ident_name << "^N\n";
                    }
                    rhs = Expr::Var(ident_name);
                }
                return Expr::BinOpExpr(BinOp::MUL, Expr::Num(v), rhs);
            }
            return Expr::Num(v);
        }
        if (is(TokenType::IDENT)) {
            const std::string name = peek().text;
            advance();
            if (is(TokenType::LPAREN)) {
                advance();
                std::vector<ExprPtr> args;
                if (!is(TokenType::RPAREN)) {
                    args.push_back(parse_expr());
                    while (is(TokenType::COMMA)) { advance(); args.push_back(parse_expr()); }
                }
                expect(TokenType::RPAREN, "Expected ')'");
                return Expr::Call(name, args);
            }
            return Expr::Var(name);
        }
        if (is(TokenType::LPAREN)) {
            advance();
            auto e = parse_expr();
            expect(TokenType::RPAREN, "Expected ')'");
            return e;
        }
        // Vec/Mat literal: `[a, b, c]` → vec(a, b, c). If every element is itself
        // a `vec(...)` call, rewrap as `mat(...)`. Empty `[]` → vec() (0-elem).
        // No new ExprType; pure FUNC_CALL sugar (design-proposal §M3).
        if (is(TokenType::LBRACKET)) {
            advance();
            std::vector<ExprPtr> elems;
            if (!is(TokenType::RBRACKET)) {
                elems.push_back(parse_expr());
                while (is(TokenType::COMMA)) { advance(); elems.push_back(parse_expr()); }
            }
            expect(TokenType::RBRACKET, "Expected ']'");
            // Promote to mat if every element is a vec literal AND we have at
            // least one element. Empty [] stays as vec().
            const bool all_vec = !elems.empty() && std::all_of(elems.begin(), elems.end(),
                [](const Expr* el) { return el->type == ExprType::FUNC_CALL && el->name == "vec"; });
            // Ragged-matrix parse-time check: when every element is a vec,
            // every row must share the same column count. Report the FIRST
            // row that diverges from row 0 so the user has a concrete pair.
            if (all_vec) {
                const size_t cols0 = elems[0]->args.size();
                for (size_t r = 1; r < elems.size(); ++r) {
                    const size_t cols_r = elems[r]->args.size();
                    if (cols_r != cols0) {
                        throw RaggedMatrixError(
                            "Ragged matrix literal: row 0 has "
                            + std::to_string(cols0)
                            + (cols0 == 1 ? " column" : " columns")
                            + ", row " + std::to_string(r) + " has "
                            + std::to_string(cols_r)
                            + (cols_r == 1 ? " column" : " columns"));
                    }
                }
            }
            return Expr::Call(all_vec ? "mat" : "vec", elems);
        }
        throw std::runtime_error("Unexpected token: '" + peek().text + "'");
    }
};
