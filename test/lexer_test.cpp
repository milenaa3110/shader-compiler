// lexer_test.cpp — standalone unit tests for the buffer-backed Lexer.
// No framework: a table of cases, asserted in main(). Links only lexer.cpp.
//
//   cmake --build build --target lexer_test && ./build/lexer_test
//   (or `ctest` from the build dir)

#include "lexer/lexer.h"

#include <cstdio>
#include <string_view>
#include <vector>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  FAIL: %s\n", (msg));                               \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

// Lex `src` fully and collect every token's kind (excluding the final Eof).
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

static void expectKinds(const char* name, std::string_view src,
                        std::vector<TokenKind> expected) {
    auto got = kinds(src);
    bool ok = (got.size() == expected.size());
    for (size_t i = 0; ok && i < got.size(); ++i)
        ok = (got[i] == expected[i]);
    if (!ok) {
        std::printf("  FAIL: %s — got %zu tokens, expected %zu\n", name,
                    got.size(), expected.size());
        ++failures;
    }
}

int main() {
    std::printf("Running lexer tests...\n");

    // ── Operators: one- and two-char forms ───────────────────────────────────
    expectKinds("two-char operators",
                "+= -= *= /= %= == != <= >= && || << >> ++ --",
                {TokenKind::PlusAssign, TokenKind::MinusAssign,
                 TokenKind::MulAssign, TokenKind::DivAssign,
                 TokenKind::ModAssign, TokenKind::Equal,
                 TokenKind::NotEqual, TokenKind::LessEqual,
                 TokenKind::GreaterEqual, TokenKind::And, TokenKind::Or,
                 TokenKind::ShiftLeft, TokenKind::ShiftRight,
                 TokenKind::Increment, TokenKind::Decrement});

    expectKinds("single-char operators",
                "+ - * / % & | ^ ~ ! < > = . , ; ( ) { } [ ] @ ? :",
                {TokenKind::Plus, TokenKind::Minus, TokenKind::Multiply,
                 TokenKind::Divide, TokenKind::Percent, TokenKind::BitwiseAnd,
                 TokenKind::BitwiseOr, TokenKind::BitwiseXor,
                 TokenKind::BitwiseNot, TokenKind::Not, TokenKind::Less,
                 TokenKind::Greater, TokenKind::Assign, TokenKind::Dot,
                 TokenKind::Comma, TokenKind::Semicolon, TokenKind::Lparen,
                 TokenKind::Rparen, TokenKind::Lbrace, TokenKind::Rbrace,
                 TokenKind::Lbracket, TokenKind::Rbracket, TokenKind::At,
                 TokenKind::Question, TokenKind::Colon});

    // ── Keyword vs identifier ────────────────────────────────────────────────
    expectKinds("keyword vs identifier", "float floaty fragment fragments",
                {TokenKind::Float, TokenKind::Identifier, TokenKind::Fragment,
                 TokenKind::Identifier});

    // ── Numbers ──────────────────────────────────────────────────────────────
    {
        Lexer lex("42 3.14 1.5e3 2E-4 0u");
        Token t;
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && t.isInt && t.num == 42.0,
              "integer literal 42");
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && !t.isInt && t.num > 3.13 &&
                  t.num < 3.15,
              "float literal 3.14");
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && !t.isInt && t.num == 1500.0,
              "scientific 1.5e3");
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && !t.isInt && t.num > 1.9e-4 &&
                  t.num < 2.1e-4,
              "scientific 2E-4");
        t = lex.next();
        CHECK(t.kind == TokenKind::Number && t.isInt && t.num == 0.0,
              "unsigned suffix 0u");
    }

    // ── Malformed numbers → InvalidNumber ────────────────────────────────────
    expectKinds("malformed 1e5e6", "1e5e6", {TokenKind::InvalidNumber});
    expectKinds("malformed 1.2.3", "1.2.3", {TokenKind::InvalidNumber});

    // ── Unknown character ────────────────────────────────────────────────────
    expectKinds("stray $", "a $ b",
                {TokenKind::Identifier, TokenKind::Unknown,
                 TokenKind::Identifier});

    // ── Comments are skipped ─────────────────────────────────────────────────
    expectKinds("# comment skipping", "a # this is a comment\nb",
                {TokenKind::Identifier, TokenKind::Identifier});

    // ── A bare '.' is the Dot operator, '.5' is a Number ─────────────────────
    expectKinds("bare dot vs leading-dot number", "a . .5",
                {TokenKind::Identifier, TokenKind::Dot, TokenKind::Number});

    // ── Position tracking, including \r\n line endings ───────────────────────
    {
        Lexer lex("a\n  b\r\nc");
        Token a = lex.next();
        CHECK(a.line == 1 && a.col == 1, "token 'a' at (1,1)");
        Token b = lex.next();
        CHECK(b.line == 2 && b.col == 3, "token 'b' at (2,3) after indent");
        Token c = lex.next();
        CHECK(c.line == 3 && c.col == 1, "token 'c' at (3,1) after CRLF");
    }

    // ── Token text spans the lexeme ──────────────────────────────────────────
    {
        Lexer lex("vec3 myVar");
        Token kw = lex.next();
        CHECK(kw.kind == TokenKind::Vec3 && kw.text == "vec3", "keyword text");
        Token id = lex.next();
        CHECK(id.kind == TokenKind::Identifier && id.text == "myVar",
              "identifier text");
    }

    if (failures == 0) {
        std::printf("All lexer tests passed.\n");
        return 0;
    }
    std::printf("%d lexer test(s) FAILED.\n", failures);
    return 1;
}
