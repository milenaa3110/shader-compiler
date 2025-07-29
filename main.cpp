#include <iostream>
#include "lexer.cpp"  // ili "lexer.h" ako razdvojiš

using namespace std;

int main() {
    int tok;
    while ((tok = gettok()) != tok_eof) {
        switch (tok) {
            case tok_identifier:
                cout << "Identifier: " << IdentifierStr << endl;
                break;
            case tok_number:
                cout << "Number: " << NumVal << endl;
                break;
            case tok_if:
                cout << "Keyword: if\n"; break;
            case tok_else:
                cout << "Keyword: else\n"; break;
            default:
                cout << "Token: '" << (char)tok << "'\n"; break;
        }
    }

    return 0;
}
