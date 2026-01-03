#ifndef LEXER_H
#define LEXER_H

#include <string>

enum Token {
    tok_eof = -1,
    tok_identifier = -2,
    tok_number = -3,
    tok_if = -4,
    tok_else = -5,
	tok_while = -6,
	tok_for = -7,
    tok_break = -8,
    tok_vec2 = -10,
    tok_vec3 = -11,
    tok_vec4 = -12,
    tok_double = -13,
    tok_float = -14,
    tok_int = -15,
    tok_uint = -16,
    tok_bool = -17,
    tok_true = -18,
    tok_false = -19,
    tok_mat2 = -20,
    tok_mat3 = -21,
    tok_mat4 = -22,
    tok_mat2x3 = -23,
    tok_mat2x4 = -24,
    tok_mat3x2 = -25,
    tok_mat3x4 = -26,
    tok_mat4x2 = -27,
    tok_mat4x3 = -28,
    tok_plus = -50,
    tok_assign = -51,
    tok_lparen = -52,
    tok_rparen = -53,
    tok_comma = -54,
    tok_semicolon = -55,
    tok_lbrace = -56,
    tok_rbrace = -57,
    tok_dot = -58,
    tok_minus = -59,
    tok_multiply = -60,
    tok_divide = -61,
    tok_greater = -62,
    tok_greater_equal = -63,
    tok_less = -64,
    tok_less_equal = -65,
    tok_equal = -66,
    tok_not_equal = -67,
    tok_lbracket = -68,
    tok_rbracket = -69,
    tok_return = -70,
    tok_fn = -71,
    tok_and = -72,
    tok_or = -73,
	tok_struct = -74,
	tok_uniform = -75,
	tok_void = -76,
	tok_not = -77,
    tok_invalid_number = -100,
};

extern std::string IdentifierStr;
extern double NumVal;

int gettok();

#endif
