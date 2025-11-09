#ifndef LEXER_H
#define LEXER_H

#include <string>

enum Token {
    tok_eof = -1,
    tok_identifier = -2,
    tok_number = -3,
    tok_if = -4,
    tok_else = -5,
    tok_vec2 = -10,
    tok_vec3 = -11,
    tok_vec4 = -12,
    tok_double = -13,
    tok_float = -14,
    tok_int = -15,
    tok_uint = -16,
    tok_bool = -17,
    tok_plus = -50,
    tok_assign = -51,
    tok_lparen = -52,
    tok_rparen = -53,
    tok_comma = -54,
    tok_greater = -55,
    tok_semicolon = -56,
    tok_lbrace = -57,
    tok_rbrace = -58,
    tok_dot = -59,
    tok_invalid_number = -100,
};

extern std::string IdentifierStr;
extern double NumVal;

int gettok();

#endif
