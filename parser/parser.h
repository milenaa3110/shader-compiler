#ifndef PARSER_H
#define PARSER_H

#include "../ast/ast.h"
#include "../ast/ast_context.h"

#include <string_view>
#include <vector>

// Parses a complete shader from an in-memory source buffer
// All AST nodes are allocated within 'ctx'. The 'ctx' object must outlive the returned vector and any subsequent use of the nodes.
// The 'source' buffer must outlive the returned AST, as node identifiers may reference it directly via string_view (zero-allocation parsing).
// Failed nodes are reported via logErrorAt and omitted from the result
std::vector<ExprAST*> ParseProgram(ASTContext& ctx, std::string_view source);

#endif  // PARSER_H
