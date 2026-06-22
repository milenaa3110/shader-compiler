#ifndef LEXER_H
#define LEXER_H

#include <cstdint>
#include <string>
#include <string_view>

#include "../../common/source_manager.h"

// Token kinds — generated from tokens.def
enum class TokenKind {
#define TOK(Kind, Name) Kind,
#include "tokens.def"
};

// A single lexical token
// text: A view into the original source (the source must outlive this token)
// num/isInt/isUnsigned: Only populated for numeric literals
// loc: Byte offset of the token's first character;
struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string_view text;
    double num = 0.0;
    bool isInt = false;
    bool isUnsigned = false;
    SourceLocation loc;
};

// Buffer-backed, instantiable, reentrant scanner
class Lexer {
 public:
    explicit Lexer(std::string_view src)
        : Beg(src.data()), Cur(src.data()), End(src.data() + src.size()) {}

    // String literals / const char* bind here
    explicit Lexer(const char* src) : Lexer(std::string_view(src)) {}

    // Reject temporary strings
    Lexer(std::string&&) = delete;

    // Scan and return the next token. Returns TokenKind::Eof past end.
    Token next();

 private:
    const char* Beg;  // start of the buffer, for computing token offsets
    const char* Cur;
    const char* End;

    // Lookahead without consuming. Returns '\0' past End.
    char peekCh(uint32_t n = 0) const {
        return (static_cast<uint32_t>(End - Cur) > n) ? Cur[n] : '\0';
    }
    // Consume one char
    void advance() {
        if (Cur < End) ++Cur;
    }
    // Offset of pointer `p` within the buffer, as a SourceLocation.
    SourceLocation locAt(const char* p) const {
        return SourceLocation::fromOffset(static_cast<uint32_t>(p - Beg));
    }
};

// Human-readable name for a token kind — used in diagnostics.
const char* tokenKindName(TokenKind kind);

#endif  // LEXER_H