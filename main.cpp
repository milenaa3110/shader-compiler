#include "parser.h"
#include <iostream>

int main() {
    std::cout << "Shader Parser. Enter input:\n";
    getNextToken();

    while (CurTok != tok_eof) {
        if (auto stmt = ParseAssignment()) {
            std::cout << "AST:\n";
            stmt->print();
        } else {
            std::cerr << "Greška u parsiranju.\n";
            break;
        }
    }

    return 0;
}
