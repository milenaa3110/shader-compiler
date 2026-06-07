#include "lexer.h"

#include <cctype>
#include <charconv>
#include <cstdlib>      // strtod (libc++ float-from_chars fallback)
#include <cstring>      // memcpy (same)
#include <unordered_map>

// Static lookup table that maps string literals to their corresponding TokenKind.
// Using string_view as the key ensures that keyword checks are extremely fast
// and perform zero memory allocations.
static const std::unordered_map<std::string_view, TokenKind>& keywords() {
  static const std::unordered_map<std::string_view, TokenKind> table = {
      {"if", TokenKind::If},
      {"else", TokenKind::Else},
      {"while", TokenKind::While},
      {"for", TokenKind::For},
      {"break", TokenKind::Break},
      {"continue", TokenKind::Continue},
      {"return", TokenKind::Return},
      {"discard", TokenKind::Discard},
      {"vec2", TokenKind::Vec2},
      {"vec3", TokenKind::Vec3},
      {"vec4", TokenKind::Vec4},
      {"double", TokenKind::Double},
      {"float", TokenKind::Float},
      {"int", TokenKind::Int},
      {"uint", TokenKind::Uint},
      {"bool", TokenKind::Bool},
      {"true", TokenKind::True},
      {"false", TokenKind::False},
      {"mat2", TokenKind::Mat2},
      {"mat3", TokenKind::Mat3},
      {"mat4", TokenKind::Mat4},
      {"mat2x3", TokenKind::Mat2x3},
      {"mat2x4", TokenKind::Mat2x4},
      {"mat3x2", TokenKind::Mat3x2},
      {"mat3x4", TokenKind::Mat3x4},
      {"mat4x2", TokenKind::Mat4x2},
      {"mat4x3", TokenKind::Mat4x3},
      {"uvec2", TokenKind::Uvec2},
      {"uvec3", TokenKind::Uvec3},
      {"uvec4", TokenKind::Uvec4},
      {"ivec2", TokenKind::Ivec2},
      {"ivec3", TokenKind::Ivec3},
      {"ivec4", TokenKind::Ivec4},
      {"sampler2D", TokenKind::Sampler2D},
      {"sampler3D", TokenKind::Sampler3D},
      {"samplerCube", TokenKind::SamplerCube},
      {"image2D", TokenKind::Image2D},
      {"void", TokenKind::Void},
      {"fn", TokenKind::Fn},
      {"struct", TokenKind::Struct},
      {"uniform", TokenKind::Uniform},
      {"const", TokenKind::Const},
      {"buffer", TokenKind::Buffer},
      {"readonly", TokenKind::Readonly},
      {"writeonly", TokenKind::Writeonly},
      {"in", TokenKind::In},
      {"out", TokenKind::Out},
      {"inout", TokenKind::Inout},
      {"stage", TokenKind::Stage},
      {"entry", TokenKind::Entry},
      {"vertex", TokenKind::Vertex},
      {"fragment", TokenKind::Fragment},
      {"compute", TokenKind::Compute},
  };
  return table;
}

// Lexer

// True for an ASCII decimal digit. 
// Pulled out because `std::isdigit` is locale-dependent.
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

void Lexer::advance() {
  if (Cur >= End) return;
  char c = *Cur;
  if (c == '\n') {
    ++Line;
    Col = 1;
    ++Cur;
  } else if (c == '\r') {
    ++Line;
    Col = 1;
    ++Cur;
    // Treat \r\n as a single newline — consume the \n without re-counting.
    if (Cur < End && *Cur == '\n') ++Cur;
  } else {
    ++Col;
    ++Cur;
  }
}

Token Lexer::next() {
  // Skip whitespace and `#`-to-end-of-line comments. 
  // A plain loop — no recursion — so an all-comments file cannot overflow the stack.
  for (;;) {
    if (Cur >= End) break;
    char c = *Cur;
    if (std::isspace(static_cast<unsigned char>(c))) {
      advance();
      continue;
    }
    if (c == '#') {
      while (Cur < End && *Cur != '\n' && *Cur != '\r') advance();
      continue;
    }
    break;
  }

  // Snapshot the position of the token's first character.
  const int sl = Line;
  const int sc = Col;
  const char* start = Cur;

  auto makeToken = [&](TokenKind k) {
    return Token{k,   std::string_view(start, static_cast<size_t>(Cur - start)),
                 0.0, false,
                 sl,  sc};
  };

  if (Cur >= End) return makeToken(TokenKind::Eof);

  const char c = *Cur;

  // Identifier / keyword
  // Goes through makeToken() like the operator branch does 
  // the Number branch is the one exception because it also carries `num` / `isInt` fields.
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    advance();
    while (Cur < End &&
           (std::isalnum(static_cast<unsigned char>(*Cur)) || *Cur == '_'))
      advance();
    std::string_view text(start, static_cast<size_t>(Cur - start));
    auto it = keywords().find(text);
    TokenKind kind =
        (it != keywords().end()) ? it->second : TokenKind::Identifier;
    return makeToken(kind);
  }

// Number, or a bare '.'
// Consumes a numeric literal. 
// Malformed numbers (e.g., '1.2.3.4' or '1e5e6') are folded into a single TokenKind::InvalidNumber.
// This prevents a single typo from triggering a cascade of confusing parser errors.
  if (isDigit(c) || c == '.') {
    // A '.' not followed by a digit is the member-access operator.
    if (c == '.' && !isDigit(peekCh(1))) {
      advance();
      return makeToken(TokenKind::Dot);
    }

    bool hasDot = false, hasExp = false, malformed = false;

    while (Cur < End && isDigit(*Cur)) advance();  // integer part
    if (Cur < End && *Cur == '.') {                // fractional part
      hasDot = true;
      advance();
      while (Cur < End && isDigit(*Cur)) advance();
    }
    if (Cur < End && *Cur == '.') {  // second '.' → bad
      malformed = true;
      advance();
      while (Cur < End && (isDigit(*Cur) || *Cur == '.')) advance();
    }
    if (!malformed && Cur < End && (*Cur == 'e' || *Cur == 'E')) {  // exponent
      hasExp = true;
      advance();
      if (Cur < End && (*Cur == '+' || *Cur == '-')) advance();
      if (Cur >= End || !isDigit(*Cur)) {
        malformed = true;  // 'e' with no digits
      } else {
        while (Cur < End && isDigit(*Cur)) advance();
      }
      if (Cur < End && (*Cur == 'e' || *Cur == 'E')) {  // second 'e' → bad
        malformed = true;
        advance();
        if (Cur < End && (*Cur == '+' || *Cur == '-')) advance();
        while (Cur < End && isDigit(*Cur)) advance();
      }
    }
    if (!malformed && Cur < End && (*Cur == 'u' || *Cur == 'U'))  // u suffix
      advance();

    std::string_view text(start, static_cast<size_t>(Cur - start));
    if (malformed)
      return Token{TokenKind::InvalidNumber, text, 0.0, false, sl, sc};
    // Numeric conversion: prefer std::from_chars (locale-independent, no
    // allocation, stops at first non-numeric byte → trailing 'u'/'U' is
    // ignored), but fall back to strtod when the standard library doesn't
    // support floating-point from_chars yet — libc++ <19 lacks it, which
    // matters for the libFuzzer build that has to use clang+libc++.
    double v = 0.0;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && \
    !defined(_LIBCPP_VERSION)
    // Fast, locale-independent conversion (C++17 std::from_chars)
    std::from_chars(text.data(), text.data() + text.size(), v);
#else
    // Fallback for standard libraries that lack floating-point std::from_chars.
    // std::strtod requires a null-terminated string, so we copy text into a safe buffer.
    char buf[64];
    size_t n = text.size() < sizeof(buf) - 1 ? text.size() : sizeof(buf) - 1;
    std::memcpy(buf, text.data(), n);
    buf[n] = '\0';
    v = std::strtod(buf, nullptr);
#endif
    return Token{TokenKind::Number, text, v, !hasDot && !hasExp, sl, sc};
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
      advance();
      return makeToken(TokenKind::Unknown);
  }
}

// Diagnostics
const char* tokenKindName(TokenKind kind) {
  switch (kind) {
    case TokenKind::Eof:
      return "end of input";
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::Number:
      return "number";
    case TokenKind::InvalidNumber:
      return "malformed number";
    case TokenKind::Unknown:
      return "unknown character";
    case TokenKind::If:
      return "'if'";
    case TokenKind::Else:
      return "'else'";
    case TokenKind::While:
      return "'while'";
    case TokenKind::For:
      return "'for'";
    case TokenKind::Break:
      return "'break'";
    case TokenKind::Continue:
      return "'continue'";
    case TokenKind::Return:
      return "'return'";
    case TokenKind::Discard:
      return "'discard'";
    case TokenKind::Vec2:
      return "'vec2'";
    case TokenKind::Vec3:
      return "'vec3'";
    case TokenKind::Vec4:
      return "'vec4'";
    case TokenKind::Double:
      return "'double'";
    case TokenKind::Float:
      return "'float'";
    case TokenKind::Int:
      return "'int'";
    case TokenKind::Uint:
      return "'uint'";
    case TokenKind::Bool:
      return "'bool'";
    case TokenKind::True:
      return "'true'";
    case TokenKind::False:
      return "'false'";
    case TokenKind::Mat2:
      return "'mat2'";
    case TokenKind::Mat3:
      return "'mat3'";
    case TokenKind::Mat4:
      return "'mat4'";
    case TokenKind::Mat2x3:
      return "'mat2x3'";
    case TokenKind::Mat2x4:
      return "'mat2x4'";
    case TokenKind::Mat3x2:
      return "'mat3x2'";
    case TokenKind::Mat3x4:
      return "'mat3x4'";
    case TokenKind::Mat4x2:
      return "'mat4x2'";
    case TokenKind::Mat4x3:
      return "'mat4x3'";
    case TokenKind::Uvec2:
      return "'uvec2'";
    case TokenKind::Uvec3:
      return "'uvec3'";
    case TokenKind::Uvec4:
      return "'uvec4'";
    case TokenKind::Ivec2:
      return "'ivec2'";
    case TokenKind::Ivec3:
      return "'ivec3'";
    case TokenKind::Ivec4:
      return "'ivec4'";
    case TokenKind::Sampler2D:
      return "'sampler2D'";
    case TokenKind::Sampler3D:
      return "'sampler3D'";
    case TokenKind::SamplerCube:
      return "'samplerCube'";
    case TokenKind::Image2D:
      return "'image2D'";
    case TokenKind::Void:
      return "'void'";
    case TokenKind::Fn:
      return "'fn'";
    case TokenKind::Struct:
      return "'struct'";
    case TokenKind::Uniform:
      return "'uniform'";
    case TokenKind::Const:
      return "'const'";
    case TokenKind::Buffer:
      return "'buffer'";
    case TokenKind::Readonly:
      return "'readonly'";
    case TokenKind::Writeonly:
      return "'writeonly'";
    case TokenKind::In:
      return "'in'";
    case TokenKind::Out:
      return "'out'";
    case TokenKind::Inout:
      return "'inout'";
    case TokenKind::Stage:
      return "'stage'";
    case TokenKind::Entry:
      return "'entry'";
    case TokenKind::Vertex:
      return "'vertex'";
    case TokenKind::Fragment:
      return "'fragment'";
    case TokenKind::Compute:
      return "'compute'";
    case TokenKind::Plus:
      return "'+'";
    case TokenKind::Minus:
      return "'-'";
    case TokenKind::Multiply:
      return "'*'";
    case TokenKind::Divide:
      return "'/'";
    case TokenKind::Percent:
      return "'%'";
    case TokenKind::Assign:
      return "'='";
    case TokenKind::PlusAssign:
      return "'+='";
    case TokenKind::MinusAssign:
      return "'-='";
    case TokenKind::MulAssign:
      return "'*='";
    case TokenKind::DivAssign:
      return "'/='";
    case TokenKind::ModAssign:
      return "'%='";
    case TokenKind::Increment:
      return "'++'";
    case TokenKind::Decrement:
      return "'--'";
    case TokenKind::Greater:
      return "'>'";
    case TokenKind::GreaterEqual:
      return "'>='";
    case TokenKind::Less:
      return "'<'";
    case TokenKind::LessEqual:
      return "'<='";
    case TokenKind::Equal:
      return "'=='";
    case TokenKind::NotEqual:
      return "'!='";
    case TokenKind::And:
      return "'&&'";
    case TokenKind::Or:
      return "'||'";
    case TokenKind::Not:
      return "'!'";
    case TokenKind::BitwiseAnd:
      return "'&'";
    case TokenKind::BitwiseOr:
      return "'|'";
    case TokenKind::BitwiseXor:
      return "'^'";
    case TokenKind::BitwiseNot:
      return "'~'";
    case TokenKind::ShiftLeft:
      return "'<<'";
    case TokenKind::ShiftRight:
      return "'>>'";
    case TokenKind::Lparen:
      return "'('";
    case TokenKind::Rparen:
      return "')'";
    case TokenKind::Lbrace:
      return "'{'";
    case TokenKind::Rbrace:
      return "'}'";
    case TokenKind::Lbracket:
      return "'['";
    case TokenKind::Rbracket:
      return "']'";
    case TokenKind::Comma:
      return "','";
    case TokenKind::Semicolon:
      return "';'";
    case TokenKind::Dot:
      return "'.'";
    case TokenKind::At:
      return "'@'";
    case TokenKind::Question:
      return "'?'";
    case TokenKind::Colon:
      return "':'";
  }
  return "<token>";
}
