#include <iostream>
#include <string>
#include <cctype>

using namespace std;

enum Token {
    tok_eof = -1,
    tok_identifier = -2,
    tok_number = -3,
    tok_if = -4,
    tok_else = -5,
};

string IdentifierStr;
double NumVal;

int gettok() {
    static int LastChar = ' ';
    
    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar)) {
        IdentifierStr = LastChar;
        while (isalnum(LastChar = getchar()))
            IdentifierStr += LastChar;

        if (IdentifierStr == "if") return tok_if;
        if (IdentifierStr == "else") return tok_else;
        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.') {
        string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while (isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }

    if (LastChar == '#') {
        do LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
        return gettok();
    }

    if (LastChar == EOF) return tok_eof;

    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}
