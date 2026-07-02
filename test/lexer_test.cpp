// lexer_test.cpp - Unit tests for the buffer-backed Lexer

#include "lexer/lexer.h"

#include <cstdio>
#include <string_view>
#include <vector>

// Global accumulator for failed test verifications.
static int failures = 0;

// Standard test assertion macro. Logs failure and increments the global counter.
#define CHECK(cond, msg)                        \
    do {                                        \
        if (!(cond)) {                          \
            std::printf("  FAIL: %s\n", (msg)); \
            ++failures;                         \
        }                                       \
    } while (0)

// Lexes the entire source view and returns a vector of token kinds, excluding Eof.
static std::vector<TokenKind> kinds(std::string_view src) {
    Lexer lex(src);
    std::vector<TokenKind> out;
    for (;;) {
        Token t = lex.next();
        if (t.kind == TokenKind::Eof) break;
        out.push_back(t.kind);
    }
    return out;
}

// Asserts that the lexed token stream exactly matches the expected sequence.
static void expectKinds(const char* name, std::string_view src, std::vector<TokenKind> expected) {
    auto got = kinds(src);
    bool ok = (got.size() == expected.size());
    for (size_t i = 0; ok && i < got.size(); ++i)
        ok = (got[i] == expected[i]);
    if (!ok) {
        std::printf("  FAIL: %s — got %zu tokens, expected %zu\n", name, got.size(),
                    expected.size());
        ++failures;
    }
}

int main() {
    std::printf("Running lexer tests...\n");

    //  Operators: One- and two-character forms
    expectKinds(
        "two-char operators", "+= -= *= /= %= == != <= >= && || << >> ++ --",
        { TokenKind::PlusAssign, TokenKind::MinusAssign, TokenKind::MulAssign, TokenKind::DivAssign,
          TokenKind::ModAssign, TokenKind::Equal, TokenKind::NotEqual, TokenKind::LessEqual,
          TokenKind::GreaterEqual, TokenKind::And, TokenKind::Or, TokenKind::ShiftLeft,
          TokenKind::ShiftRight, TokenKind::Increment, TokenKind::Decrement });

    expectKinds(
        "single-char operators", "+ - * / % & | ^ ~ ! < > = . , ; ( ) { } [ ] @ ? :",
        { TokenKind::Plus,       TokenKind::Minus,      TokenKind::Multiply,  TokenKind::Divide,
          TokenKind::Percent,    TokenKind::BitwiseAnd, TokenKind::BitwiseOr, TokenKind::BitwiseXor,
          TokenKind::BitwiseNot, TokenKind::Not,        TokenKind::Less,      TokenKind::Greater,
          TokenKind::Assign,     TokenKind::Dot,        TokenKind::Comma,     TokenKind::Semicolon,
          TokenKind::Lparen,     TokenKind::Rparen,     TokenKind::Lbrace,    TokenKind::Rbrace,
          TokenKind::Lbracket,   TokenKind::Rbracket,   TokenKind::At,        TokenKind::Question,
          TokenKind::Colon });

    //  Keywords vs. Identifiers ─
    expectKinds(
        "keyword vs identifier", "float floaty fragment fragments",
        { TokenKind::Float, TokenKind::Identifier, TokenKind::Fragment, TokenKind::Identifier });

    //  Numeric Literals
    {
        Lexer lex("42 3.14 1.5e3 2E-4 0u");
        Token t;

        // Integer literal tracking.
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && t.isInt && t.num == 42.0, "integer literal 42");

        // Decimal floating-point literal tracking.
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && !t.isInt && t.num > 3.13 && t.num < 3.15,
              "float literal 3.14");

        // Scientific notation processing (lowercase 'e').
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && !t.isInt && t.num == 1500.0, "scientific 1.5e3");

        // Scientific notation processing (uppercase 'E' with negative exponent).
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && !t.isInt && t.num > 1.9e-4 && t.num < 2.1e-4,
              "scientific 2E-4");

        // Unsigned suffix processing.
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && t.isInt && t.num == 0.0, "unsigned suffix 0u");
    }

    //  Malformed Numbers → InvalidNumber
    expectKinds("malformed 1e5e6", "1e5e6", { TokenKind::InvalidNumber });
    expectKinds("malformed 1.2.3", "1.2.3", { TokenKind::InvalidNumber });

    // Suffixed floats are illegal (the 'u' modifier targets integers only).
    expectKinds("malformed 1.5u", "1.5u", { TokenKind::InvalidNumber });
    expectKinds("malformed 1e3u", "1e3u", { TokenKind::InvalidNumber });

    // Out-of-range floating point values must yield a malformed classification.
    expectKinds("out-of-range 1e400", "1e400", { TokenKind::InvalidNumber });

    //  Multi-byte UTF-8 Validation
    // A multi-byte symbol must map to a single Unknown token, preventing byte-splitting.
    expectKinds("utf-8 arrow is single Unknown",
                "a\xe2\x86\x92"
                "b",
                { TokenKind::Identifier, TokenKind::Unknown, TokenKind::Identifier });

    //  Stray/Unknown Characters ─
    expectKinds("stray $", "a $ b",
                { TokenKind::Identifier, TokenKind::Unknown, TokenKind::Identifier });

    //  Comments
    expectKinds("# comment skipping", "a # this is a comment\nb",
                { TokenKind::Identifier, TokenKind::Identifier });

    //  Disambiguation: Dot Operator vs Leading-Dot Number ─
    expectKinds("bare dot vs leading-dot number", "a . .5",
                { TokenKind::Identifier, TokenKind::Dot, TokenKind::Number });

    //  Source Position and Line Ending Tracking (\n & \r\n)
    // Coordinates are captured as raw byte-offsets; SourceManager expands to line/col.
    {
        std::string_view src = "a\n  b\r\nc";
        SourceManager sm(src, "<test>");
        Lexer lex(src);
        auto lc = [&](const Token& t) { return sm.getLineCol(t.loc); };

        Token a = lex.next();
        CHECK(lc(a).line == 1 && lc(a).col == 1, "token 'a' at (1,1)");

        Token b = lex.next();
        CHECK(lc(b).line == 2 && lc(b).col == 3, "token 'b' at (2,3) after indent");

        Token c = lex.next();
        CHECK(lc(c).line == 3 && lc(c).col == 1, "token 'c' at (3,1) after CRLF");
    }

    //  Token Text Spans
    {
        Lexer lex("vec3 myVar");
        Token kw = lex.next();
        CHECK(kw.kind == TokenKind::Vec3 && kw.text == "vec3", "keyword text");

        Token id = lex.next();
        CHECK(id.kind == TokenKind::Identifier && id.text == "myVar", "identifier text");
    }

    //  Execution Evaluation
    if (failures == 0) {
        std::printf("All lexer tests passed.\n");
        return 0;
    }
    std::printf("%d lexer test(s) FAILED.\n", failures);
    return 1;
}