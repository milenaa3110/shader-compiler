#include "lexer.h"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>
#include <unordered_map>

// Static lookup table that maps keyword spellings to their TokenKind
static const std::unordered_map<std::string_view, TokenKind>& keywords() {
    static const std::unordered_map<std::string_view, TokenKind> table = {
#define KEYWORD(Kind, Spelling) { Spelling, TokenKind::Kind },
#include "tokens.def"
    };
    return table;
}

// Lexer

// Avoid using locale-dependent functions (like std::isdigit) to ensure determinism.
static bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool isAlnum(char c) {
    return isAlpha(c) || isDigit(c);
}
static bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

Token Lexer::next() {
    // Skip whitespace and `#`-to-end-of-line comments.
    // A plain loop — no recursion — so an all-comments file cannot overflow the stack.
    for (;;) {
        if (Cur >= End) break;
        char c = *Cur;
        if (isSpace(c)) {
            advance();
            continue;
        }
        if (c == '#') {
            while (Cur < End && *Cur != '\n' && *Cur != '\r')
                advance();
            continue;
        }
        break;
    }

    // Snapshot the position of the token's first character.
    const char* start = Cur;
    const SourceLocation loc = locAt(start);

    auto makeToken = [&](TokenKind k) {
        return Token{ k,     std::string_view(start, static_cast<size_t>(Cur - start)),
                      0.0,   false,
                      false, loc };
    };

    if (Cur >= End) return makeToken(TokenKind::Eof);

    const char c = *Cur;

    // Identifier / keyword
    if (isAlpha(c) || c == '_') {
        advance();
        while (Cur < End && (isAlnum(*Cur) || *Cur == '_'))
            advance();
        std::string_view text(start, static_cast<size_t>(Cur - start));
        auto it = keywords().find(text);
        TokenKind kind = (it != keywords().end()) ? it->second : TokenKind::Identifier;
        return makeToken(kind);
    }

    // Number, or a bare '.'
    // Consumes a numeric literal.
    // Malformed numbers (e.g., '1.2.3.4' or '1e5e6') are folded into a single TokenKind::InvalidNumber
    if (isDigit(c) || c == '.') {
        // A '.' not followed by a digit is the member-access operator.
        if (c == '.' && !isDigit(peekCh(1))) {
            advance();
            return makeToken(TokenKind::Dot);
        }

        bool hasDot = false, hasExp = false, malformed = false, isUnsigned = false;

        while (Cur < End && isDigit(*Cur))
            advance();                   // integer part
        if (Cur < End && *Cur == '.') {  // fractional part
            hasDot = true;
            advance();
            while (Cur < End && isDigit(*Cur))
                advance();
        }
        if (Cur < End && *Cur == '.') {  // second '.' → bad
            malformed = true;
            advance();
            while (Cur < End && (isDigit(*Cur) || *Cur == '.'))
                advance();
        }
        if (!malformed && Cur < End && (*Cur == 'e' || *Cur == 'E')) {  // exponent
            hasExp = true;
            advance();
            if (Cur < End && (*Cur == '+' || *Cur == '-')) advance();
            if (Cur >= End || !isDigit(*Cur)) {
                malformed = true;  // 'e' with no digits
            } else {
                while (Cur < End && isDigit(*Cur))
                    advance();
            }
            if (Cur < End && (*Cur == 'e' || *Cur == 'E')) {  // second 'e' → bad
                malformed = true;
                advance();
                if (Cur < End && (*Cur == '+' || *Cur == '-')) advance();
                while (Cur < End && isDigit(*Cur))
                    advance();
            }
        }
        if (!malformed && Cur < End && (*Cur == 'u' || *Cur == 'U')) {  // u suffix
            // The 'u' (unsigned) suffix is only meaningful on integer literals; the
            // language has no "unsigned float". `1.5u` / `1e3u` are typos, not values.
            if (hasDot || hasExp)
                malformed = true;
            else
                isUnsigned = true;  // a valid uint literal (e.g. 42u)
            advance();              // consume it either way so it's part of the (invalid) token
        }

        if (malformed) return makeToken(TokenKind::InvalidNumber);

        std::string_view text(start, static_cast<size_t>(Cur - start));
        double v = 0.0;
        // Fast, locale-independent parsing via C++17 std::from_chars if supported
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
        auto res = std::from_chars(text.data(), text.data() + text.size(), v);
        if (res.ec == std::errc::result_out_of_range) return makeToken(TokenKind::InvalidNumber);
#else
        // Fallback for older stdlibs; requires a copy to guarantee null-termination
        std::string buf(text);
        errno = 0;
        v = std::strtod(buf.c_str(), nullptr);
        if (errno == ERANGE) return makeToken(TokenKind::InvalidNumber);
#endif
        return Token{ TokenKind::Number, text, v, !hasDot && !hasExp, isUnsigned, loc };
    }

    // Operators and punctuation
    switch (c) {
        case '+':
            advance();
            if (peekCh() == '+') {
                advance();
                return makeToken(TokenKind::Increment);
            }
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::PlusAssign);
            }
            return makeToken(TokenKind::Plus);
        case '-':
            advance();
            if (peekCh() == '-') {
                advance();
                return makeToken(TokenKind::Decrement);
            }
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::MinusAssign);
            }
            return makeToken(TokenKind::Minus);
        case '*':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::MulAssign);
            }
            return makeToken(TokenKind::Multiply);
        case '/':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::DivAssign);
            }
            return makeToken(TokenKind::Divide);
        case '%':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::ModAssign);
            }
            return makeToken(TokenKind::Percent);
        case '=':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::Equal);
            }
            return makeToken(TokenKind::Assign);
        case '<':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::LessEqual);
            }
            if (peekCh() == '<') {
                advance();
                return makeToken(TokenKind::ShiftLeft);
            }
            return makeToken(TokenKind::Less);
        case '>':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::GreaterEqual);
            }
            if (peekCh() == '>') {
                advance();
                return makeToken(TokenKind::ShiftRight);
            }
            return makeToken(TokenKind::Greater);
        case '!':
            advance();
            if (peekCh() == '=') {
                advance();
                return makeToken(TokenKind::NotEqual);
            }
            return makeToken(TokenKind::Not);
        case '&':
            advance();
            if (peekCh() == '&') {
                advance();
                return makeToken(TokenKind::And);
            }
            return makeToken(TokenKind::BitwiseAnd);
        case '|':
            advance();
            if (peekCh() == '|') {
                advance();
                return makeToken(TokenKind::Or);
            }
            return makeToken(TokenKind::BitwiseOr);
        case '^':
            advance();
            return makeToken(TokenKind::BitwiseXor);
        case '~':
            advance();
            return makeToken(TokenKind::BitwiseNot);
        case '?':
            advance();
            return makeToken(TokenKind::Question);
        case ':':
            advance();
            return makeToken(TokenKind::Colon);
        case '(':
            advance();
            return makeToken(TokenKind::Lparen);
        case ')':
            advance();
            return makeToken(TokenKind::Rparen);
        case '[':
            advance();
            return makeToken(TokenKind::Lbracket);
        case ']':
            advance();
            return makeToken(TokenKind::Rbracket);
        case ',':
            advance();
            return makeToken(TokenKind::Comma);
        case ';':
            advance();
            return makeToken(TokenKind::Semicolon);
        case '{':
            advance();
            return makeToken(TokenKind::Lbrace);
        case '}':
            advance();
            return makeToken(TokenKind::Rbrace);
        case '@':
            advance();
            return makeToken(TokenKind::At);
        default:
            // Skip the entire malformed or unhandled UTF-8 sequence to emit a single Unknown token
            advance();
            while (Cur < End && (static_cast<unsigned char>(*Cur) & 0xC0) == 0x80)
                advance();
            return makeToken(TokenKind::Unknown);
    }
}

// Diagnostics — human-readable spelling of a token kind
const char* tokenKindName(TokenKind kind) {
    switch (kind) {
#define TOK(Kind, Name)   \
    case TokenKind::Kind: \
        return Name;
#include "tokens.def"
    }
    return "<token>";
}