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
        if (IdentifierStr == "double") return tok_double;
        if (IdentifierStr == "float") return tok_float;
        if (IdentifierStr == "int") return tok_int;
        if (IdentifierStr == "uint") return tok_uint;
        if (IdentifierStr == "bool") return tok_bool;
        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.') {
        std::string NumStr;
        int dotCount = 0;
        bool invalid = false;

        do {
            if (LastChar == '.') {
                dotCount++;
                if (dotCount > 1)
                    invalid = true;
            }
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        if (invalid) {
            std::cerr << "Warning: invalid number literal: " << NumStr << std::endl;
            return tok_invalid_number;
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

    if (LastChar == '=') {
        LastChar = getchar();
        return tok_assign;
    }

    if (LastChar == '(') {
        LastChar = getchar();
        return tok_lparen;
    }

    if (LastChar == ')') {
        LastChar = getchar();
        return tok_rparen;
    }

    if (LastChar == ',') {
        LastChar = getchar();
        return tok_comma;
    }

    if (LastChar == '>') {
        LastChar = getchar();
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
