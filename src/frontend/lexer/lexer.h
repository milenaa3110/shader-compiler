#ifndef LEXER_H
#define LEXER_H

#include <string_view>

// Token kinds
enum class TokenKind {
  // Structural
  Eof,
  Identifier,
  Number,
  InvalidNumber,  // malformed numeric literal (e.g. 1e5e6, 1.2.3)
  Unknown,        // a character the lexer doesn't recognise

  // Keywords — control flow
  If,
  Else,
  While,
  For,
  Break,
  Continue,
  Return,
  Discard,

  // Keywords — scalar / vector / matrix types
  Vec2,
  Vec3,
  Vec4,
  Double,
  Float,
  Int,
  Uint,
  Bool,
  True,
  False,
  Mat2,
  Mat3,
  Mat4,
  Mat2x3,
  Mat2x4,
  Mat3x2,
  Mat3x4,
  Mat4x2,
  Mat4x3,
  Uvec2,
  Uvec3,
  Uvec4,
  Ivec2,
  Ivec3,
  Ivec4,
  Sampler2D,
  Sampler3D,
  SamplerCube,
  Image2D,
  Void,

  // Keywords — declarations / qualifiers
  Fn,
  Struct,
  Uniform,
  Const,
  Buffer,
  Readonly,
  Writeonly,
  In,
  Out,
  Inout,
  Stage,
  Entry,
  Vertex,
  Fragment,
  Compute,

  // Operators — arithmetic
  Plus,
  Minus,
  Multiply,
  Divide,
  Percent,
  Assign,
  PlusAssign,
  MinusAssign,
  MulAssign,
  DivAssign,
  ModAssign,
  Increment,
  Decrement,

  // Operators — comparison / logical
  Greater,
  GreaterEqual,
  Less,
  LessEqual,
  Equal,
  NotEqual,
  And,
  Or,
  Not,

  // Operators — bitwise (integer scalars/vectors)
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  BitwiseNot,
  ShiftLeft,
  ShiftRight,

  // Punctuation
  Lparen,
  Rparen,
  Lbrace,
  Rbrace,
  Lbracket,
  Rbracket,
  Comma,
  Semicolon,
  Dot,
  At,
  Question,
  Colon,
};

// A single lexical token
// text: A view into the original source (the source must outlive this token)
// num/isInt: Only populated for numeric literals
// line/col: Source location used for compiler diagnostics and error messages
struct Token {
  TokenKind kind = TokenKind::Eof;
  std::string_view text;
  double num = 0.0;
  bool isInt = false;
  int line = 1;
  int col = 1;
};

// Buffer-backed, instantiable, reentrant scanner
class Lexer {
 public:
  explicit Lexer(std::string_view src)
      : Cur(src.data()), End(src.data() + src.size()) {}

  // Scan and return the next token. Returns TokenKind::Eof past end.
  Token next();

 private:
  const char* Cur;
  const char* End;
  int Line = 1;
  int Col = 1;

  // Lookahead without consuming. Returns '\0' past End.
  char peekCh(int n = 0) const { return (Cur + n < End) ? Cur[n] : '\0'; }
  // Consume one char, maintaining Line/Col (handles \n, \r, \r\n).
  void advance();
};

// Human-readable name for a token kind — used in diagnostics.
const char* tokenKindName(TokenKind kind);

#endif  // LEXER_H