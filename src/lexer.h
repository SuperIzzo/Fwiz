#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <cctype>
#include <optional>

enum class TokenType : uint8_t {
    NUMBER, IDENT,
    PLUS, MINUS, STAR, SLASH, CARET,
    LPAREN, RPAREN, EQUALS, QUESTION, COMMA,
    END
};

struct Token {
    TokenType type;
    std::string text;
    double numval = 0;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src), pos_(0) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (pos_ < src_.size()) {
            skip_ws();
            if (pos_ >= src_.size()) break;
            char c = src_[pos_];

            if (is_digit(c) || (c == '.' && pos_ + 1 < src_.size() && is_digit(src_[pos_ + 1])))
                tokens.push_back(read_number());
            else if (is_alpha(c) || c == '_')
                tokens.push_back(read_ident());
            else if (auto t = single_char(c))
                tokens.push_back({*t, {c}, 0}), pos_++;
            else
                throw std::runtime_error(std::string("Unexpected character: ") + c);
        }
        tokens.push_back({TokenType::END, ""});
        return tokens;
    }

private:
    std::string src_;
    size_t pos_;

    static bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }
    static bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)); }
    static bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)); }

    void skip_ws() {
        while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t'))
            pos_++;
    }

    static std::optional<TokenType> single_char(char c) {
        switch (c) {
            case '+': return TokenType::PLUS;   case '-': return TokenType::MINUS;
            case '*': return TokenType::STAR;   case '/': return TokenType::SLASH;
            case '^': return TokenType::CARET;  case '(': return TokenType::LPAREN;
            case ')': return TokenType::RPAREN; case '=': return TokenType::EQUALS;
            case '?': return TokenType::QUESTION; case ',': return TokenType::COMMA;
            default:  return std::nullopt;
        }
    }

    Token read_number() {
        size_t start = pos_;
        while (pos_ < src_.size() && (is_digit(src_[pos_]) || src_[pos_] == '.'))
            pos_++;
        std::string text = src_.substr(start, pos_ - start);
        return {TokenType::NUMBER, text, std::stod(text)};
    }

    Token read_ident() {
        size_t start = pos_;
        while (pos_ < src_.size() && (is_alnum(src_[pos_]) || src_[pos_] == '_'))
            pos_++;
        return {TokenType::IDENT, src_.substr(start, pos_ - start)};
    }
};
