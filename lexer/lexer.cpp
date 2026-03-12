#include "lexer.h"
#include <cctype>
#include <unordered_map>

std::string IdentifierStr;
double NumVal;

static std::unordered_map<std::string, int> keywords = {
    {"if", tok_if},
    {"else", tok_else},
    {"vec2", tok_vec2},
    {"vec3", tok_vec3},
    {"vec4", tok_vec4},
    {"mat2", tok_mat2},
    {"mat3", tok_mat3},
    {"mat4", tok_mat4},
    {"mat2x3", tok_mat2x3},
    {"mat2x4", tok_mat2x4},
    {"mat3x2", tok_mat3x2},
    {"mat3x4", tok_mat3x4},
    {"mat4x2", tok_mat4x2},
    {"mat4x3", tok_mat4x3},
    {"double", tok_double},
    {"float", tok_float},
    {"int", tok_int},
    {"uint", tok_uint},
    {"bool", tok_bool},
    {"true", tok_true},
    {"false", tok_false},
    {"return", tok_return},
    {"fn", tok_fn},
    {"for", tok_for},
    {"while", tok_while},
    {"break", tok_break},
    {"struct", tok_struct},
    {"uniform", tok_uniform},
    {"void", tok_void},
    {"stage", tok_stage},
    {"entry", tok_entry},
    {"vertex", tok_vertex},
    {"fragment", tok_fragment},
    {"compute", tok_compute},
    // Stage interface qualifiers
    {"in",    tok_in},
    {"out",   tok_out},
    {"inout", tok_inout},
    // Integer vector types
    {"uvec2", tok_uvec2},
    {"uvec3", tok_uvec3},
    {"uvec4", tok_uvec4},
    {"ivec2", tok_ivec2},
    {"ivec3", tok_ivec3},
    {"ivec4", tok_ivec4},
    // Sampler / image types
    {"sampler2D",   tok_sampler2D},
    {"sampler3D",   tok_sampler3D},
    {"samplerCube", tok_samplerCube},
    {"image2D",     tok_image2D},
    // Additional keywords
    {"continue", tok_continue},
    {"discard",  tok_discard},
    {"const",    tok_const},
};

int gettok() {
    static int LastChar = ' ';
    // skip whitespace
    while (isspace(LastChar))
        LastChar = getchar();
    // identifiers and keywords
    if (isalpha(LastChar) || LastChar == '_') {
        IdentifierStr = LastChar;
        while (isalnum(LastChar = getchar()) || LastChar == '_')
            IdentifierStr += LastChar;

        auto it = keywords.find(IdentifierStr);
        return (it != keywords.end()) ? it->second : tok_identifier;
    }

    // Number (integer or decimal)
    if (isdigit(LastChar) || LastChar == '.') {
        std::string NumStr;

        if (LastChar == '.') {
            int C = getchar();
            if (!isdigit(C)) {
                LastChar = C;
                return tok_dot;
            }
            NumStr += '.';
            NumStr += static_cast<char>(C);
            LastChar = getchar();
        } else {
            do {
                NumStr += LastChar;
                LastChar = getchar();
            } while (isdigit(LastChar));
            if (LastChar == '.') {
                NumStr += LastChar;
                LastChar = getchar();
            }
        }
        while (isdigit(LastChar)) {
            NumStr += LastChar;
            LastChar = getchar();
        }
        // Scientific notation: e.g. 1.5e3 or 2E-4
        if (LastChar == 'e' || LastChar == 'E') {
            NumStr += LastChar;
            LastChar = getchar();
            if (LastChar == '+' || LastChar == '-') {
                NumStr += LastChar;
                LastChar = getchar();
            }
            while (isdigit(LastChar)) {
                NumStr += LastChar;
                LastChar = getchar();
            }
        }
        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }

    // Comments
    if (LastChar == '#') {
        do LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
        return gettok();
    }

    switch (LastChar) {
        case '+':
            LastChar = getchar();
            if (LastChar == '+') { LastChar = getchar(); return tok_increment; }
            if (LastChar == '=') { LastChar = getchar(); return tok_plus_assign; }
            return tok_plus;
        case '-':
            LastChar = getchar();
            if (LastChar == '-') { LastChar = getchar(); return tok_decrement; }
            if (LastChar == '=') { LastChar = getchar(); return tok_minus_assign; }
            return tok_minus;
        case '*':
            LastChar = getchar();
            if (LastChar == '=') { LastChar = getchar(); return tok_mul_assign; }
            return tok_multiply;
        case '/':
            LastChar = getchar();
            if (LastChar == '=') { LastChar = getchar(); return tok_div_assign; }
            return tok_divide;
        case '?': LastChar = getchar(); return tok_question;
        case ':': LastChar = getchar(); return tok_colon;
        case '(': LastChar = getchar(); return tok_lparen;
        case ')': LastChar = getchar(); return tok_rparen;
        case '[': LastChar = getchar(); return tok_lbracket;
        case ']': LastChar = getchar(); return tok_rbracket;
        case ',': LastChar = getchar(); return tok_comma;
        case ';': LastChar = getchar(); return tok_semicolon;
        case '{': LastChar = getchar(); return tok_lbrace;
        case '}': LastChar = getchar(); return tok_rbrace;
        case '@': LastChar = getchar(); return tok_at;

        case '=':
            if ((LastChar = getchar()) == '=') {
                LastChar = getchar();
                return tok_equal;
            }
            return tok_assign;

        case '<':
            if ((LastChar = getchar()) == '=') {
                LastChar = getchar();
                return tok_less_equal;
            }
            return tok_less;

        case '>':
            if ((LastChar = getchar()) == '=') {
                LastChar = getchar();
                return tok_greater_equal;
            }
            return tok_greater;

        case '!':
            if ((LastChar = getchar()) == '=') {
                LastChar = getchar();
                return tok_not_equal;
            }
            return tok_not;

        case '&':
            if ((LastChar = getchar()) == '&') {
                LastChar = getchar();
                return tok_and;
            }
            return tok_bitwise_and;  // bare '&' — not a supported operator

        case '|':
            if ((LastChar = getchar()) == '|') {
                LastChar = getchar();
                return tok_or;
            }
            return tok_bitwise_or;   // bare '|' — not a supported operator

        case '.':
            LastChar = getchar();
            return tok_dot;

        case EOF:
            return tok_eof;

        default:
            int ThisChar = LastChar;
            LastChar = getchar();
            return ThisChar;
    }
}


