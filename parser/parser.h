#ifndef PARSER_H
#define PARSER_H

#include "../ast/ast.h"
#include <memory>

extern int CurTok;
int getNextToken();

std::unique_ptr<ExprAST> ParseExpression();
std::unique_ptr<ExprAST> ParsePrimary();
std::unique_ptr<ExprAST> ParseUnary();
std::unique_ptr<ExprAST> ParseStatement();
std::unique_ptr<ExprAST> parseFunction();
std::unique_ptr<ExprAST> parseReturn();
std::vector<std::unique_ptr<ExprAST>> ParseProgram();

#endif // PARSER_H
