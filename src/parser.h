#pragma once
#include "lexer.h"
#include "expr.h"

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tok_(tokens), pos_(0) {}

    ExprPtr parse_expr() { return additive(); }
    bool at_end() const { return tok_[pos_].type == TokenType::END; }

private:
    std::vector<Token> tok_;
    size_t pos_;

    const Token& peek() const                { return tok_[pos_]; }
    const Token& advance()                   { return tok_[pos_++]; }
    bool is(TokenType t) const               { return peek().type == t; }
    bool is(TokenType a, TokenType b) const  { return is(a) || is(b); }

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

    // primary → NUMBER | IDENT (LPAREN args RPAREN)? | LPAREN expr RPAREN
    ExprPtr primary() {
        if (is(TokenType::NUMBER)) {
            double v = peek().numval;
            advance();
            return Expr::Num(v);
        }
        if (is(TokenType::IDENT)) {
            std::string name = peek().text;
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
        throw std::runtime_error("Unexpected token: '" + peek().text + "'");
    }
};
