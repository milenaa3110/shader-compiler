#include "parser.h"
#include <iostream>

int CurTok;
int getNextToken() { return CurTok = gettok(); }

std::unique_ptr<ExprAST> ParseNumberExpr() {
    auto Result = std::make_unique<NumberExprAST>(NumVal);
    getNextToken();
    return std::move(Result);
}

std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;
    getNextToken();

    if (CurTok != tok_lparen)
        return std::make_unique<VariableExprAST>(IdName);

    getNextToken();
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != tok_rparen) {
        while (true) {
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;

            if (CurTok == tok_rparen)
                break;

            if (CurTok != tok_comma) {
                std::cerr << "Expected ',' in argument list\n";
                return nullptr;
            }
            getNextToken(); // consume comma
        }
    }

    getNextToken(); // consume ')'
    return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

/// Parse expressions inside parentheses or literals
std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok) {
        case tok_identifier: 
        case tok_vec2:
        case tok_vec3:
        case tok_vec4:
        case tok_double:
        case tok_float:
        case tok_int:
        case tok_uint:
        case tok_bool:
            return ParseIdentifierExpr();
        case tok_number: return ParseNumberExpr();
        case tok_lparen: {
            getNextToken(); // consume '('
            auto E = ParseExpression();
            if (CurTok != tok_rparen) {
                std::cerr << "Expected ')'\n";
                return nullptr;
            }
            getNextToken();
            return E;
        }
        default:
            std::cerr << "Unknown token when expecting an expression\n";
            return nullptr;
    }
}

/// Operator precedence (simple version)
int GetTokPrecedence() {
    if (CurTok == tok_plus) return 20;
    return -1;
}

/// Parse binary operator RHS
std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
    while (true) {
        int TokPrec = GetTokPrecedence();
        if (TokPrec < ExprPrec)
            return LHS;

        int BinOp = CurTok;
        getNextToken(); // consume operator

        auto RHS = ParsePrimary();
        if (!RHS) return nullptr;

        int NextPrec = GetTokPrecedence();
        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
            if (!RHS) return nullptr;
        }

        LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
    }
}

/// Parse full expression
std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary();
    if (!LHS) return nullptr;

    return ParseBinOpRHS(0, std::move(LHS));
}

/// Parse assignment: vec3 a = expr;
std::unique_ptr<ExprAST> ParseAssignment() {
    std::string type;

    if (CurTok == tok_vec2 || CurTok == tok_vec3 || CurTok == tok_vec4 || CurTok == tok_double || CurTok == tok_float || CurTok == tok_int || CurTok == tok_uint || CurTok == tok_bool) {
        if (CurTok == tok_vec2) type = "vec2";
        if (CurTok == tok_vec3) type = "vec3";
        if (CurTok == tok_vec4) type = "vec4";
        if (CurTok == tok_double) type = "double";
        if (CurTok == tok_float) type = "float";
        if (CurTok == tok_int) type = "int";
        if (CurTok == tok_uint) type = "uint";
        if (CurTok == tok_bool) type = "bool";
    } else {
        std::cerr << "Expected type not valid";
        return nullptr;
    }

    getNextToken(); // consume type

    if (CurTok != tok_identifier) {
        std::cerr << "Expected variable name\n";
        return nullptr;
    }

    std::string name = IdentifierStr;
    getNextToken(); // consume variable name

    if (CurTok != tok_assign) {
        std::cerr << "Expected '=' after variable name\n";
        return nullptr;
    }

    getNextToken(); // consume '='

    auto value = ParseExpression();
    if (!value) return nullptr;

    if (CurTok != tok_semicolon) {
        std::cerr << "Expected ';' at end of assignment\n";
        return nullptr;
    }

    getNextToken(); // consume ';'

    return std::make_unique<AssignmentExprAST>(type, name, std::move(value));
}
