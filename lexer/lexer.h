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
    tok_at = -78,
    tok_entry = -79,
    tok_stage = -80,
    tok_vertex = -81,
    tok_fragment = -82,
    tok_compute = -83,
    tok_bitwise_and = -84,   // bare '&' — bitwise AND not supported; triggers parse error
    tok_bitwise_or  = -85,   // bare '|' — bitwise OR  not supported; triggers parse error

    // Stage interface qualifiers
    tok_in    = -86,
    tok_out   = -87,
    tok_inout = -88,

    // Unsigned/signed int vector types
    tok_uvec2 = -89,
    tok_uvec3 = -90,
    tok_uvec4 = -91,
    tok_ivec2 = -92,
    tok_ivec3 = -93,
    tok_ivec4 = -94,

    // Sampler / image opaque types
    tok_sampler2D   = -95,
    tok_sampler3D   = -96,
    tok_samplerCube = -97,
    tok_image2D     = -98,

    // Additional statement keywords
    tok_continue = -99,
    tok_discard  = -110,
    tok_const    = -111,

    // Compound assignment operators
    tok_plus_assign  = -112,  // +=
    tok_minus_assign = -113,  // -=
    tok_mul_assign   = -114,  // *=
    tok_div_assign   = -115,  // /=

    // Increment / decrement
    tok_increment = -116,  // ++
    tok_decrement = -117,  // --

    // Ternary operator
    tok_question = -118,  // ?
    tok_colon    = -119,  // :

    tok_invalid_number = -120,

    // Storage buffer qualifiers (compute shaders)
    tok_buffer    = -121,
    tok_readonly  = -122,
    tok_writeonly = -123,
};

// Set to true by gettok() when the last tok_number had no decimal point
extern bool IsIntLiteral;

extern std::string IdentifierStr;
extern double NumVal;

int gettok();

#endif
