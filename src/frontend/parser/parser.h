#ifndef PARSER_H
#define PARSER_H

#include "../ast/ast.h"
#include "../ast/ast_context.h"

#include <string_view>
#include <vector>

// Parses a shader into an AST allocated within 'ctx'.
// Both 'ctx' and the 'source' buffer must outlive the returned nodes (zero-allocation parsing).
// Malformed nodes are logged and omitted from the final vector.
std::vector<ExprAST*> ParseProgram(ASTContext& ctx, std::string_view source);

#endif  // PARSER_H
