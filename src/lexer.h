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
    LPAREN, RPAREN, LBRACKET, RBRACKET, EQUALS, QUESTION, COMMA,
    // COLON: reserved for binding-annotation grammar only (see Future #78 /
    // gen-3 cycle 1 design). Lock-in: `var:atom = expr` and
    // `var:(atom1, atom2, ...) = expr` are the ONLY shapes; map literals,
    // ternaries, range syntax etc. require a separate design cycle.
    COLON,
    // IN: reserved word for infix membership predicate (gen-5 cycle 3f).
    // `x in set_name` is syntax sugar for `is_in(x, set_name)`. Lexer-level
    // keyword (read_ident intercepts) — distinct from cycle 1.1 `{if, iff, e}`
    // NUMBER-IDENT denylist which is a parser-level desugar guard, not a token
    // upgrade. See parse_condition for the infix-to-FUNC_CALL synthesis.
    IN,
    // DOTDOT (`..`) and AT (`@`): range/step markers in range literals
    // `[lo..hi]` / `[lo..hi @ step]` (gen-6 cycle 1, Future #5b). DOTDOT is
    // lexed in tokenize() before the number-start dispatch; AT is a single_char.
    DOTDOT,
    AT,
    // LBRACE (`{`) / RBRACE (`}`): ordered-collection literal delimiters
    // `{1, 2, 3}` -> seq(...) and `{lo..hi}` -> range(...) (gen-6 arc cycle 1).
    // Single-char tokens; any new bracket pair the lexer emits must also be
    // tracked by the depth scanners in system.h (has_named_eq_in_range,
    // parse_call_args binding sub-loop) — see those sites.
    LBRACE,
    RBRACE,
    END,
    COUNT_
};
static_assert(static_cast<int>(TokenType::COUNT_) == 21,
    "TokenType count drift — update lexer dispatch table if adding/removing tokens.");

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

            // `..` range separator — two dots as one token (gen-6 cycle 1).
            // Must precede the number-start check: `..` starts with `.` which
            // would otherwise begin a number (then fail) or be unexpected.
            if (c == '.' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '.') {
                tokens.push_back({TokenType::DOTDOT, "..", 0}); pos_ += 2; continue;
            }

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
            case ')': return TokenType::RPAREN; case '[': return TokenType::LBRACKET;
            case ']': return TokenType::RBRACKET; case '=': return TokenType::EQUALS;
            case '?': return TokenType::QUESTION; case ',': return TokenType::COMMA;
            case ':': return TokenType::COLON;  case '@': return TokenType::AT;
            case '{': return TokenType::LBRACE; case '}': return TokenType::RBRACE;
            default:  return std::nullopt;
        }
    }

    Token read_number() {
        const size_t start = pos_;
        // Consume digits and a decimal point, but STOP at the first `.` of a
        // `..` range separator (gen-6 cycle 1) so `1..5` lexes as
        // NUMBER(1) DOTDOT NUMBER(5) rather than feeding "1..5" to std::stod.
        // `1.5` is unaffected: the dot is followed by a digit, not another dot.
        while (pos_ < src_.size() && (is_digit(src_[pos_]) ||
               (src_[pos_] == '.' &&
                (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '.'))))
            pos_++;
        // Scientific notation `[eE][+-]?[0-9]+` — required before the units
        // cycle 1 NUMBER-IDENT desugar so `100e3` keeps parsing as 100000
        // instead of becoming `100 * Var("e3")` (unbound-var solver error).
        // Three-char lookahead: 'e'/'E', optional sign, at least one digit.
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            size_t look = pos_ + 1;
            if (look < src_.size() && (src_[look] == '+' || src_[look] == '-')) look++;
            if (look < src_.size() && is_digit(src_[look])) {
                pos_ = look;
                while (pos_ < src_.size() && is_digit(src_[pos_])) pos_++;
            }
        }
        const std::string text = src_.substr(start, pos_ - start);
        return {TokenType::NUMBER, text, std::stod(text)};
    }

    Token read_ident() {
        const size_t start = pos_;
        while (pos_ < src_.size() && (is_alnum(src_[pos_]) || src_[pos_] == '_'))
            pos_++;
        // Include dots for dotted names (e.g., geometry.rectangle in formula calls)
        // Only if dot is followed by an alpha character (not a digit or end)
        while (pos_ < src_.size() && src_[pos_] == '.'
               && pos_ + 1 < src_.size() && (is_alpha(src_[pos_ + 1]) || src_[pos_ + 1] == '_')) {
            pos_++; // skip the dot
            while (pos_ < src_.size() && (is_alnum(src_[pos_]) || src_[pos_] == '_'))
                pos_++;
        }
        const std::string name = src_.substr(start, pos_ - start);
        // Reserved-word upgrade (gen-5 cycle 3f): `in` lexes as TokenType::IN
        // so `parse_condition` can string-scan ` in ` and synthesise
        // `is_in(lhs, rhs)`. `in` falls through to "Unexpected token" in
        // expression context — correct fail-safe per design D2.
        if (name == "in") return {TokenType::IN, name, 0};
        return {TokenType::IDENT, name};
    }
};
