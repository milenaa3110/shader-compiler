#include "lexer.h"
#include <cctype>
#include <iostream>

std::string IdentifierStr;
double NumVal;

int gettok() {
    static int LastChar = ' ';

    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar) || LastChar == '_') {
        IdentifierStr = LastChar;
        while (isalnum(LastChar = getchar()) || LastChar == '_')
            IdentifierStr += LastChar;

        if (IdentifierStr == "if") return tok_if;
        if (IdentifierStr == "else") return tok_else;
        if (IdentifierStr == "vec2") return tok_vec2;
        if (IdentifierStr == "vec3") return tok_vec3;
        if (IdentifierStr == "vec4") return tok_vec4;
        if (IdentifierStr == "mat2") return tok_mat2;
        if (IdentifierStr == "mat3") return tok_mat3;
        if (IdentifierStr == "mat4") return tok_mat4;
        if (IdentifierStr == "mat2x3") return tok_mat2x3;
        if (IdentifierStr == "mat2x4") return tok_mat2x4;
        if (IdentifierStr == "mat3x2") return tok_mat3x2;
        if (IdentifierStr == "mat3x4") return tok_mat3x4;
        if (IdentifierStr == "mat4x2") return tok_mat4x2;
        if (IdentifierStr == "mat4x3") return tok_mat4x3;
        if (IdentifierStr == "double") return tok_double;
        if (IdentifierStr == "float") return tok_float;
        if (IdentifierStr == "int") return tok_int;
        if (IdentifierStr == "uint") return tok_uint;
        if (IdentifierStr == "bool") return tok_bool;
        if (IdentifierStr == "true") return tok_true;
        if (IdentifierStr == "false") return tok_false;
        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.') {
        std::string NumStr;
        int dotCount = 0;

        if (LastChar == '.') {
            // lookahead: da li je posle '.' cifra?
            int C = getchar();
            if (!isdigit(C)) {
                // Nije decimalna tačka, već operator '.' (npr. a.x)
                int ThisChar = '.';
                LastChar = C;           // ne gubimo sledeći znak
                return tok_dot;
            }
            // Jeste decimalan broj, počinje sa ".<digit>"
            NumStr += '.';
            NumStr += static_cast<char>(C);
            dotCount = 1;
            LastChar = getchar();
        } else {
            // broj počinje cifrom
            do {
                NumStr += LastChar;
                LastChar = getchar();
            } while (isdigit(LastChar));
            if (LastChar == '.') {
                dotCount = 1;
                NumStr += LastChar;
                LastChar = getchar();
            }
        }

        // Nastavak: skupljaj ostatak decimalnog dela (samo još cifre / jedna tačka nije dozvoljena)
        while (isdigit(LastChar)) {
            NumStr += LastChar;
            LastChar = getchar();
        }
        if (LastChar == '.') {
            // druga tačka -> nevažan slučaj; vrati prvu kao broj, pusti parser da prijavi grešku kasnije
            // ili možeš ovde ispisati warning i tretirati '.' kao sledeći token
        }

        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }


    if (LastChar == '#') {
        do LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
        return gettok();
    }

    if (LastChar == '+') {
        LastChar = getchar();
        return tok_plus;
    }

    if (LastChar == '*') {
        LastChar = getchar();
        return tok_multiply;
    }

    if (LastChar == '/') {
        LastChar = getchar();
        return tok_divide;
    }

    if (LastChar == '-') {
        LastChar = getchar();
        return tok_minus;
    }

    if (LastChar == '=') {
        if ((LastChar = getchar()) == '=') {
            LastChar = getchar();
            return tok_equal;
        }
        return tok_assign;
    }

    if (LastChar == '!') {
        if ((LastChar = getchar()) == '=') {
            LastChar = getchar();
            return tok_not_equal;
        }
        return '!';
    }

    if (LastChar == '(') {
        LastChar = getchar();
        return tok_lparen;
    }

    if (LastChar == ')') {
        LastChar = getchar();
        return tok_rparen;
    }

    if (LastChar == '[') {
        LastChar = getchar();
        return tok_lbracket;
    }

    if (LastChar == ']') {
        LastChar = getchar();
        return tok_rbracket;
    }

    if (LastChar == ',') {
        LastChar = getchar();
        return tok_comma;
    }

    if (LastChar == '<') {
        if ((LastChar = getchar()) == '=') {
            LastChar = getchar();
            return tok_less_equal;
        }
        return tok_less;
    }

    if (LastChar == '>') {
        if ((LastChar = getchar()) == '=') {
            LastChar = getchar();
            return tok_greater_equal;
        }
        return tok_greater;
    }

    if (LastChar == ';') {
        LastChar = getchar();
        return tok_semicolon;
    }

    if (LastChar == '{') {
        LastChar = getchar();
        return tok_lbrace;
    }

    if (LastChar == '}') {
        LastChar = getchar();
        return tok_rbrace;
    }

    if (LastChar == '.') {
        LastChar = getchar();
        return tok_dot;
    }

    if (LastChar == EOF) return tok_eof;

    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}
