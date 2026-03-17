#include "parser.h"
#include "../error_utils.h"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fmt/core.h>

// Forward declarations for accessing codegen state
extern std::unordered_set<std::string> StructTypes;
extern std::unordered_map<std::string, std::vector<std::string>> StructDependencies;

class Parser {
public:
    // take next token from lexer
    int next() {
        Cur = gettok();
        return peek();
    }

    // peek at Parser::peek() current token
    int peek() const { return Cur; }

    std::vector<std::unique_ptr<ExprAST> > ParseProgram();

    std::unique_ptr<ExprAST> parseStruct();

    std::unique_ptr<ExprAST> parseFunction();

    std::optional<FunctionAttrs> parseFunctionAttrs();
    
    static std::optional<ShaderStage> stageFromTok(int tok);

    std::unique_ptr<ExprAST> parseStatement();

    std::unique_ptr<ExprAST> parseBlockExpr();

    std::unique_ptr<ExprAST> parseIfExpr();

    std::unique_ptr<ExprAST> parseWhile();

    std::unique_ptr<ExprAST> parseFor();

    std::unique_ptr<ExprAST> parseReturn();

    std::unique_ptr<ExprAST> parseBreak();
    std::unique_ptr<ExprAST> parseStageVarDecl(bool isInput, int binding = -1);
    std::unique_ptr<ExprAST> parseContinue();
    std::unique_ptr<ExprAST> parseDiscard();
    std::unique_ptr<ExprAST> parseUniform(int binding = -1);
    
    std::unique_ptr<ExprAST> parseStorageBuffer(int binding = -1);

    std::unique_ptr<ExprAST> parseVarDecl();

    std::unique_ptr<ExprAST> parseAssignmentOrExprStatement();

    std::unique_ptr<ExprAST> parseExpression();

    std::unique_ptr<ExprAST> parseUnary();

    std::unique_ptr<ExprAST> parsePrimary();

    std::unique_ptr<ExprAST> parseNumberExpr();

    std::unique_ptr<ExprAST> parseIdentifierOrCtorExpr();

    std::unique_ptr<ExprAST> parsePostfixAfterIdent(std::unique_ptr<ExprAST> base);

    std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrecedence, std::unique_ptr<ExprAST> lhs);

private:
    // Parser::peek()rent token the parser is looking at
    int Cur = 0;

    // check if token is a type token (e.g., int, float, vec3, etc.)
    static bool isTypeTok(const int tok) {
        switch (tok) {
            case tok_vec2: case tok_vec3: case tok_vec4:
            case tok_double: case tok_float: case tok_int: case tok_uint: case tok_bool:
            case tok_mat2: case tok_mat3: case tok_mat4:
            case tok_mat2x3: case tok_mat2x4: case tok_mat3x2:
            case tok_mat3x4: case tok_mat4x2: case tok_mat4x3:
            case tok_uvec2: case tok_uvec3: case tok_uvec4:
            case tok_ivec2: case tok_ivec3: case tok_ivec4:
            case tok_sampler2D: case tok_sampler3D: case tok_samplerCube:
            case tok_image2D:
                return true;
            default: return false;
        }
    }

    // get precedence of binary operators
    static int precedence(const int tok) {
        switch (tok) {
            case tok_or:
                return 5;
            case tok_and:
                return 6;
            case tok_less:
            case tok_less_equal:
            case tok_greater:
            case tok_greater_equal:
            case tok_equal:
            case tok_not_equal:
                return 10;
            case tok_plus:
            case tok_minus:
            case '+':
            case '-':
                return 20;
            case tok_multiply:
            case tok_divide:
            case '*':
            case '/':
            case '%':
                return 40;
            default:
                return -1;
        }
    }

    // map token to type name string
    static std::string_view tokToTypeName(const int tok) {
        switch (tok) {
            case tok_vec2: return "vec2";
            case tok_vec3: return "vec3";
            case tok_vec4: return "vec4";
            case tok_double: return "double";
            case tok_float: return "float";
            case tok_int: return "int";
            case tok_uint: return "uint";
            case tok_bool: return "bool";
            case tok_mat2: return "mat2";
            case tok_mat3: return "mat3";
            case tok_mat4: return "mat4";
            case tok_mat2x3: return "mat2x3";
            case tok_mat2x4: return "mat2x4";
            case tok_mat3x2: return "mat3x2";
            case tok_mat3x4: return "mat3x4";
            case tok_mat4x2: return "mat4x2";
            case tok_mat4x3: return "mat4x3";
            case tok_uvec2: return "uvec2";
            case tok_uvec3: return "uvec3";
            case tok_uvec4: return "uvec4";
            case tok_ivec2: return "ivec2";
            case tok_ivec3: return "ivec3";
            case tok_ivec4: return "ivec4";
            case tok_sampler2D:   return "sampler2D";
            case tok_sampler3D:   return "sampler3D";
            case tok_samplerCube: return "samplerCube";
            case tok_image2D:     return "image2D";
            case tok_void: return "void";
            default: return "";
        }
    }

    // map token to its name string
    static std::string getTokName(const int tok) {
        switch (tok) {
            case tok_eof: return "EOF";
            case tok_identifier: return "IDENTIFIER";
            case tok_number: return "NUMBER";
            case tok_if: return "IF";
            case tok_else: return "ELSE";
            case tok_while: return "WHILE";
            case tok_for: return "FOR";
            case tok_break: return "BREAK";
            case tok_vec2: return "VEC2";
            case tok_vec3: return "VEC3";
            case tok_vec4: return "VEC4";
            case tok_double: return "DOUBLE";
            case tok_float: return "FLOAT";
            case tok_int: return "INT";
            case tok_uint: return "UINT";
            case tok_bool: return "BOOL";
            case tok_true: return "TRUE";
            case tok_false: return "FALSE";
            case tok_mat2: return "MAT2";
            case tok_mat3: return "MAT3";
            case tok_mat4: return "MAT4";
            case tok_mat2x3: return "MAT2X3";
            case tok_mat2x4: return "MAT2X4";
            case tok_mat3x2: return "MAT3X2";
            case tok_mat3x4: return "MAT3X4";
            case tok_mat4x2: return "MAT4X2";
            case tok_mat4x3: return "MAT4X3";
            case tok_plus: return "PLUS";
            case tok_assign: return "ASSIGN";
            case tok_lparen: return "LPAREN";
            case tok_rparen: return "RPAREN";
            case tok_comma: return "COMMA";
            case tok_semicolon: return "SEMICOLON";
            case tok_lbrace: return "LBRACE";
            case tok_rbrace: return "RBRACE";
            case tok_dot: return "DOT";
            case tok_minus: return "MINUS";
            case tok_multiply: return "MULTIPLY";
            case tok_divide: return "DIVIDE";
            case tok_greater: return "GREATER";
            case tok_greater_equal: return "GREATER_EQUAL";
            case tok_less: return "LESS";
            case tok_less_equal: return "LESS_EQUAL";
            case tok_equal: return "EQUAL";
            case tok_not_equal: return "NOT_EQUAL";
            case tok_lbracket: return "LBRACKET";
            case tok_rbracket: return "RBRACKET";
            case tok_return: return "RETURN";
            case tok_fn: return "FN";
            case tok_and: return "AND";
            case tok_or: return "OR";
            case tok_struct: return "STRUCT";
            case tok_uniform: return "UNIFORM";
            case tok_void: return "VOID";
            case tok_invalid_number: return "INVALID_NUMBER";
            default:
                if (tok >= 0 && tok <= 127) {
                    return std::string(1, static_cast<char>(tok));
                }
                return "UNKNOWN(" + std::to_string(tok) + ")";
        }
    }

    // check if a name is a user-defined struct type
    bool isUserTypeName(const std::string &name) const {
        return StructTypes.find(name) != StructTypes.end();
    }

    // check for recursive struct definitions using DFS cycle detection
    bool hasRecursiveStructs(const std::string &structName, 
                            const std::vector<std::pair<std::string, std::string>>& fields) {
        // extract user-defined type dependencies from fields
        std::vector<std::string> dependencies;
        for (const auto& [fieldType, fieldName] : fields) {
            if (isUserTypeName(fieldType)) {
                dependencies.push_back(fieldType);
            }
        }

        // if no dependencies, no recursion possible
        if (dependencies.empty()) {
            return false;
        }

        // store dependencies for this struct
        StructDependencies[structName] = dependencies;

        // perform DFS cycle detection
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> recStack;
        
        return hasCycleDFS(structName, visited, recStack);
    }

    // DFS helper to detect cycles in struct dependency graph
    bool hasCycleDFS(const std::string& structName,
                    std::unordered_set<std::string>& visited,
                    std::unordered_set<std::string>& recStack) const {
        // mark current node as visited and add to recursion stack
        visited.insert(structName);
        recStack.insert(structName);

        // check all dependencies
        auto it = StructDependencies.find(structName);
        if (it != StructDependencies.end()) {
            for (const auto& dependency : it->second) {
                // if dependency not visited, recurse
                if (visited.find(dependency) == visited.end()) {
                    if (hasCycleDFS(dependency, visited, recStack)) {
                        return true;
                    }
                }
                // if dependency is in recursion stack, we found a cycle
                else if (recStack.find(dependency) != recStack.end()) {
                    return true;
                }
            }
        }

        // remove from recursion stack before returning
        recStack.erase(structName);
        return false;
    }

    // parse index expression inside brackets: '[' expression ']' - helper function
    std::unique_ptr<ExprAST> parseBracketIndex() {
        if (Parser::peek() != tok_lbracket) {
            logError("Expected '[' for index expression");
            return nullptr;
        }
        next();

        auto idx = parseExpression();
        if (!idx) return nullptr;

        if (Parser::peek() != tok_rbracket) {
            logError("Expected ']' after index expression");
            return nullptr;
        }

        next();
        return idx;
    }
};

// Parse program
std::vector<std::unique_ptr<ExprAST> > Parser::ParseProgram() {
    std::vector<std::unique_ptr<ExprAST> > program;
    std::unordered_map<ShaderStage, std::string> entryByStage;
    while (peek() != tok_eof) {
        std::unique_ptr<ExprAST> stmt = nullptr;
    if (peek() == tok_at) {
        auto attrsOpt = parseFunctionAttrs();
        if (!attrsOpt) {
            next(); // skip invalid attributes
            continue;
        }

        if (peek() != tok_fn) {
            logError("Attributes are only allowed before a function ('fn')");
            while (peek() != tok_fn && peek() != tok_eof) next();
            continue;
        }

        auto fn = parseFunction();
        if (auto *F = dynamic_cast<FunctionAST*>(fn.get())) {
            F->Attrs = *attrsOpt;
        } else {
            logError("Internal error: expected FunctionAST after parsing function");
        }

        stmt = std::move(fn);
        if (auto *F = dynamic_cast<FunctionAST*>(stmt.get())) {
            if (F->Attrs.isEntry) {
            ShaderStage st = *F->Attrs.stage;
            if (entryByStage.count(st)) {
                logError(fmt::format("Multiple entry points for stage. Already: {}, new: {}",
                                    entryByStage[st], F->Proto->Name));
                } else {
                    entryByStage[st] = F->Proto->Name;
                }
            }
        }
    }
    else if (peek() == tok_fn) {
        stmt = parseFunction();
    } else if (peek() == tok_in) {
            stmt = parseStageVarDecl(true);
        } else if (peek() == tok_out) {
            stmt = parseStageVarDecl(false);
        } else if (peek() == tok_identifier && IdentifierStr == "layout") {
            // layout(binding=N, ...) uniform/in/out ...
            int binding = -1;
            next(); // 'layout'
            if (peek() == tok_lparen) {
                next(); // '('
                while (peek() != tok_rparen && peek() != tok_eof) {
                    if (peek() == tok_identifier && IdentifierStr == "binding") {
                        next();
                        if (peek() == tok_assign) { next(); if (peek() == tok_number) { binding = (int)NumVal; next(); } }
                    } else if (peek() == tok_identifier) {
                        next(); // skip unknown key
                        if (peek() == tok_assign) { next(); if (peek() == tok_number) next(); }
                    } else if (peek() == tok_comma) { next(); }
                    else { next(); }
                }
                if (peek() == tok_rparen) next();
            }
            if (peek() == tok_uniform)      stmt = parseUniform(binding);
            else if (peek() == tok_in)      stmt = parseStageVarDecl(true, binding);
            else if (peek() == tok_out)     stmt = parseStageVarDecl(false, binding);
            else if (peek() == tok_readonly || peek() == tok_writeonly || peek() == tok_buffer)
                                            stmt = parseStorageBuffer(binding);
            else {
                logError("Expected 'uniform', 'in', 'out', or 'buffer' after layout(...)");
                while (peek() != tok_semicolon && peek() != tok_eof) next();
                if (peek() == tok_semicolon) next();
            }
        } else if (peek() == tok_uniform) {
            stmt = parseUniform();
        } else if (peek() == tok_struct) {
            stmt = parseStruct();
        } else {
            stmt = parseStatement();
        }
        if (stmt) {
            program.push_back(std::move(stmt));
        } else {
            logError(fmt::format("Unexpected token at top level: {}", getTokName(peek())));
            next();
        }
    }
    return program;
}

// Struct declaration
std::unique_ptr<ExprAST> Parser::parseStruct() {
    next(); // 'struct'
    if (peek() != tok_identifier) {
        logError("Expected struct name after 'struct'");
        return nullptr;
    }
    std::string structName = IdentifierStr;
    next(); // struct name

    if (peek() != tok_lbrace) {
        logError("Expected '{' after struct name");
        return nullptr;
    }
    next(); // '{'
    std::vector<std::pair<std::string, std::string> > fields;
    std::unordered_set<std::string> seenFieldNames;
    while (peek() != tok_rbrace) {
        if (peek() != tok_identifier && !isTypeTok(peek())) {
            logError("Expected field type in struct");
            return nullptr;
        }
        std::string fieldType;
        if (isTypeTok(peek())) {
            fieldType = std::string(tokToTypeName(peek()));
        } else {
            // check if it's a user-defined struct type
            if (!isUserTypeName(IdentifierStr)) {
                logError(fmt::format("Unknown type '{}' in struct field", IdentifierStr));
                return nullptr;
            }
            fieldType = IdentifierStr;
        }
        next(); // type
        if (peek() != tok_identifier) {
            logError("Expected field name in struct");
            return nullptr;
        }
        std::string fieldName = IdentifierStr;
        if (!seenFieldNames.insert(fieldName).second) {
            logError(fmt::format("Duplicate field name '{}' in struct", fieldName));
            return nullptr;
        }
        next(); // field name
        if (peek() != tok_semicolon) {
            logError("Expected ';' after struct field");
            return nullptr;
        }
        next(); // ';'
        fields.emplace_back(fieldType, fieldName);
    }
    if (peek() != tok_rbrace) {
        logError("Expected '}' after struct body");
        return nullptr;
    }
    next(); // '}'
    if (peek() != tok_semicolon) {
        logError("Expected ';' after struct declaration");
        return nullptr;
    }
    next(); // ';'

    // add to struct types before cycle check (needed for dependency lookup)
    StructTypes.insert(structName);

    // check for recursive struct definitions (both direct and indirect)
    if (hasRecursiveStructs(structName, fields)) {
        logError(fmt::format("Struct '{}' contains recursive definition", structName));
        StructTypes.erase(structName);
        StructDependencies.erase(structName);
        return nullptr;
    }
    
    return std::make_unique<StructDeclExprAST>(structName, std::move(fields));
}

// Uniform declaration
std::unique_ptr<ExprAST> Parser::parseUniform(int /*binding*/) {
    next(); // 'uniform'
    std::string type;

    // built-in type
    if (isTypeTok(peek())) {
        type = std::string(tokToTypeName(peek()));
        next(); // type
    }
    // user-defined struct type
    else if (peek() == tok_identifier && isUserTypeName(IdentifierStr)) {
        type = IdentifierStr;
        next(); // type
    } else {
        logError("Expected type after 'uniform'");
        return nullptr;
    }

    if (peek() != tok_identifier) {
        logError("Expected uniform name");
        return nullptr;
    }
    std::string name = IdentifierStr;
    next(); // name

    // Check for array syntax
    bool isArray = false;
    int arraySize = 0;

    if (peek() == tok_lbracket) {
        next(); // '['
        isArray = true;

        if (peek() != tok_number) {
            logError("Expected array size");
            return nullptr;
        }
        if (NumVal < 1.0 || NumVal > 65536.0) {
            logError(fmt::format("Array size must be between 1 and 65536, got {}", (int)NumVal));
            return nullptr;
        }
        arraySize = static_cast<int>(NumVal);
        next(); // number

        if (peek() != tok_rbracket) {
            logError("Expected ']' after array size");
            return nullptr;
        }
        next(); // ']'
    }

    if (peek() != tok_semicolon) {
        logError("Expected ';' after uniform declaration");
        return nullptr;
    }
    next(); // ';'

    if (isArray) {
        return std::make_unique<UniformArrayDeclExprAST>(type, name, arraySize);
    }
    return std::make_unique<UniformDeclExprAST>(type, name);
}

// Storage buffer declaration (compute shaders)
// Syntax: layout(std430, binding=N) readonly buffer ElemType name[];
std::unique_ptr<ExprAST> Parser::parseStorageBuffer(int binding) {
    bool isReadOnly = true;
    if (peek() == tok_readonly) {
        isReadOnly = true;
        next(); // 'readonly'
    } else if (peek() == tok_writeonly) {
        isReadOnly = false;
        next(); // 'writeonly'
    }

    if (peek() != tok_buffer) {
        logError("Expected 'buffer' in storage buffer declaration");
        return nullptr;
    }
    next(); // 'buffer'

    // Element type
    std::string elemType;
    if (isTypeTok(peek())) {
        elemType = std::string(tokToTypeName(peek()));
        next();
    } else if (peek() == tok_identifier) {
        elemType = IdentifierStr;
        next();
    } else {
        logError("Expected element type in buffer declaration");
        return nullptr;
    }

    // Variable name
    if (peek() != tok_identifier) {
        logError("Expected buffer variable name");
        return nullptr;
    }
    std::string name = IdentifierStr;
    next(); // name

    // Consume unsized [] array suffix
    if (peek() == tok_lbracket) {
        next(); // '['
        if (peek() == tok_rbracket) next(); // ']'
    }

    // Consume ';'
    if (peek() == tok_semicolon) next();

    return std::make_unique<StorageBufferDeclAST>(elemType, name, isReadOnly, binding);
}


// Function definition
std::unique_ptr<ExprAST> Parser::parseFunction() {
    if (peek() != tok_fn) {
        logError("Expected 'fn' at function start");
        return nullptr;
    }
    next(); // 'fn'
    std::string ret;
    if (isTypeTok(peek()) || peek() == tok_void) {
        ret = std::string(tokToTypeName(peek()));
        next(); // built-in/void
    } else if (peek() == tok_identifier && isUserTypeName(IdentifierStr)) {
        // e.g. Hit, Ray, Light...
        ret = IdentifierStr;
        next(); // user-defined type
    } else {
        logError("Function must start with return type");
        return nullptr;
    }
    if (peek() != tok_identifier) {
        logError("Expected function name");
        return nullptr;
    }
    std::string name = IdentifierStr;
    next(); // function name
    if (peek() != tok_lparen) {
        logError("Expected '(' after function name");
        return nullptr;
    }
    next(); // '('
    std::vector<std::pair<std::string, std::string>> params;
    if (peek() != tok_rparen) {
        while (true) {
            std::string pty;

            // === Optional qualifier: in, out, inout ===
            if (peek() == tok_in || peek() == tok_out || peek() == tok_inout)
                next(); // consume (semantics reserved for future inout pointer passing)

            // === PARAM TYPE: built-in or user-defined struct ===
            if (isTypeTok(peek())) {
                pty = std::string(tokToTypeName(peek()));
                next();          // e.g. vec3
            } else if (peek() == tok_identifier && isUserTypeName(IdentifierStr)) {
                pty = IdentifierStr;
                next(); // user-defined type
            } else {
                logError("Expected param type");
                return nullptr;
            }

            if (peek() != tok_identifier) {
                logError("Expected param name");
                return nullptr;
            }
            std::string pname = IdentifierStr;
            next(); // param name

            params.emplace_back(pty, pname);

            if (peek() == tok_rparen) break;
            if (peek() != tok_comma) {
                logError("Expected ',' in parameter list");
                return nullptr;
            }
            next(); // ','
        }
    }
    next(); // ')'
    if (peek() != tok_lbrace) {
        logError("Expected '{' to start function body");
        return nullptr;
    }
    auto body = parseBlockExpr();
    if (!body) return nullptr;
    
    auto proto = std::make_unique<PrototypeAST>(name, std::move(params), ret);
    return std::make_unique<FunctionAST>(std::move(proto), std::move(body));
}

std::optional<ShaderStage> Parser::stageFromTok(int tok) {
    switch (tok) {
        case tok_vertex:   return ShaderStage::Vertex;
        case tok_fragment: return ShaderStage::Fragment;
        case tok_compute:  return ShaderStage::Compute;
        default: return std::nullopt;
    }
}

std::optional<FunctionAttrs> Parser::parseFunctionAttrs() {
    FunctionAttrs attrs;

    while (peek() == tok_at) {
        next(); // '@'

        if (peek() == tok_entry) {
            next(); // 'entry'
            if (attrs.isEntry) {
                logError("Duplicate @entry");
                return std::nullopt;
            }
            attrs.isEntry = true;
            continue;
        }

        if (peek() == tok_stage) {
            next(); // 'stage'

            if (peek() != tok_lparen) {
                logError("Expected '(' after @stage");
                return std::nullopt;
            }
            next(); // '('

            auto st = stageFromTok(peek());
            if (!st.has_value()) {
                logError("Expected stage name: vertex|fragment|compute");
                return std::nullopt;
            }
            if (attrs.stage.has_value()) {
                logError("Duplicate @stage");
                return std::nullopt;
            }
            attrs.stage = *st;
            next(); // consume vertex/fragment/compute

            if (peek() != tok_rparen) {
                logError("Expected ')' after @stage(...)");
                return std::nullopt;
            }
            next(); // ')'
            continue;
        }

        if (peek() == tok_identifier && IdentifierStr == "workgroup_size") {
            next(); // 'workgroup_size'
            if (peek() != tok_lparen) { logError("Expected '(' after @workgroup_size"); return std::nullopt; }
            next(); // '('
            uint32_t x = 1, y = 1, z = 1;
            if (peek() == tok_number) { x = (uint32_t)NumVal; next(); }
            if (peek() == tok_comma)  { next(); if (peek() == tok_number) { y = (uint32_t)NumVal; next(); } }
            if (peek() == tok_comma)  { next(); if (peek() == tok_number) { z = (uint32_t)NumVal; next(); } }
            if (peek() != tok_rparen) { logError("Expected ')' after @workgroup_size(...)"); return std::nullopt; }
            next(); // ')'
            attrs.workgroupSize = std::array<uint32_t,3>{x, y, z};
            continue;
        }

        logError(fmt::format("Unknown attribute after '@': {}", getTokName(peek())));
        return std::nullopt;
    }

    if (attrs.isEntry && !attrs.stage.has_value()) {
        logError("@entry requires @stage(...)");
        return std::nullopt;
    }

    return attrs;
}

// Parse statement
std::unique_ptr<ExprAST> Parser::parseStatement() {
    switch (peek()) {
        case tok_struct:
            return parseStruct();
        case tok_uniform:
            return parseUniform();
        case tok_return:
            return parseReturn();
        case tok_while:
            return parseWhile();
        case tok_for:
            return parseFor();
        case tok_break:
            return parseBreak();
        case tok_continue:
            return parseContinue();
        case tok_discard:
            return parseDiscard();
        case tok_const:
            next(); // consume 'const'
            return parseVarDecl();
        case tok_if:
            return parseIfExpr();
        case tok_lbrace:
            return parseBlockExpr();
        case tok_increment:
        case tok_decrement: {
            // prefix ++/-- as a standalone statement (e.g. "++x;")
            auto expr = parseExpression();
            if (!expr) return nullptr;
            if (peek() == tok_semicolon) next();
            return expr;
        }
        case tok_identifier:
            // could be var decl for user-defined type
            if (isUserTypeName(IdentifierStr)) {
                return parseVarDecl();
            }
            // otherwise, treat as assignment or expression statement
            return parseAssignmentOrExprStatement();
        default:
            if (isTypeTok(peek())) {
                return parseVarDecl();
            }
            logError(fmt::format("Unknown token at start of statement {}", getTokName(peek())));
            return nullptr;
    }
}

// Block expression
std::unique_ptr<ExprAST> Parser::parseBlockExpr() {
    next(); // '{'
    std::vector<std::unique_ptr<ExprAST> > statements;
    while (peek() != tok_rbrace && peek() != tok_eof) {
        auto stmt = parseStatement();
        if (!stmt) return nullptr;
        statements.push_back(std::move(stmt));
    }
    if (peek() != tok_rbrace) {
        logError("Expected '}' at end of block");
        return nullptr;
    }
    next(); // '}'
    return std::make_unique<BlockExprAST>(std::move(statements));
}

// parse if expression
// example: if (cond) { ... } else { ... }
std::unique_ptr<ExprAST> Parser::parseIfExpr() {
    next(); // 'if'
    if (peek() != tok_lparen) {
        logError("Expected '(' after 'if'");
        return nullptr;
    }
    next(); // '('
    auto cond = parseExpression();
    if (!cond) return nullptr;
    if (Parser::peek() != tok_rparen) {
        logError("Expected ')' in if condition");
        return nullptr;
    }
    next(); // ')'
    // then block or statement
    std::unique_ptr<ExprAST> thenExpr;
    if (peek() == tok_lbrace) {
        thenExpr = parseBlockExpr();
    } else {
        thenExpr = parseStatement();
    }
    if (!thenExpr) return nullptr;
    std::unique_ptr<ExprAST> elseExpr;
    if (peek() == tok_else) {
        next(); // 'else'
        if (peek() == tok_lbrace) {
            elseExpr = parseBlockExpr();
        } else {
            elseExpr = parseStatement();
        }
        if (!elseExpr) return nullptr;
    }
    return std::make_unique<IfExprAST>(std::move(cond), std::move(thenExpr), std::move(elseExpr));
}

// parse while loop
// example: while (cond) { ... }
std::unique_ptr<ExprAST> Parser::parseWhile() {
    next(); // 'while'
    if (peek() != tok_lparen) {
        logError("Expected '(' after 'while'");
        return nullptr;
    }
    next(); // '('
    auto cond = parseExpression();
    if (!cond) return nullptr;
    if (peek() != tok_rparen) {
        logError("Expected ')' in while condition");
        return nullptr;
    }
    next(); // ')'
    std::unique_ptr<ExprAST> body;
    if (peek() == tok_lbrace) {
        body = parseBlockExpr();
    } else {
        body = parseStatement();
    }
    if (!body) return nullptr;
    return std::make_unique<WhileExprAST>(std::move(cond), std::move(body));
}

// For loop
std::unique_ptr<ExprAST> Parser::parseFor() {
    next(); // 'for'
    if (peek() != tok_lparen) {
        logError("Expected '(' after 'for'");
        return nullptr;
    }
    next(); // '('
    // Initialization
    std::unique_ptr<ExprAST> init;
    if (peek() != tok_semicolon) {
        init = parseStatement();
    } else {
        next(); // ';'
    }
    // Condition
    std::unique_ptr<ExprAST> cond;
    if (peek() != tok_semicolon) {
        cond = parseExpression();
        if (!cond) return nullptr;
    }
    if (peek() != tok_semicolon) {
        logError("Expected ';' after for condition");
        return nullptr;
    }
    next(); // ';'
    // Increment
    std::unique_ptr<ExprAST> inc;
    if (peek() != tok_rparen) {
        if (peek() == tok_identifier) {
            std::string name = IdentifierStr;
            next(); // identifier
            if (peek() == tok_assign) {
                // i = i + 1;
                next(); // '='
                auto rhs = parseExpression();
                if (!rhs) return nullptr;
                inc = std::make_unique<AssignmentExprAST>("", name, std::move(rhs));
            } else {
                // example : i++ or i--
                auto lhs = std::make_unique<VariableExprAST>(name);
                inc = parseBinOpRHS(0, std::move(lhs));
            }
        } else {
            inc = parseExpression();
        }
    }
    if (peek() != tok_rparen) {
        logError("Expected ')' after for increment");
        return nullptr;
    }
    next(); // ')'
    std::unique_ptr<ExprAST> body;
    if (peek() == tok_lbrace) {
        body = parseBlockExpr();
    } else {
        body = parseStatement();
    }
    if (!body) return nullptr;
    return std::make_unique<ForExprAST>(std::move(init), std::move(cond), std::move(inc), std::move(body));
}

// parse return statement
// example: return expr;
std::unique_ptr<ExprAST> Parser::parseReturn() {
    next(); // 'return'
    // for void return
    if (peek() == tok_semicolon) {
        next(); // ';'
        return std::make_unique<ReturnStmtAST>(nullptr);
    }
    auto expr_ast = parseExpression();
    if (!expr_ast) return nullptr;
    if (peek() != tok_semicolon) {
        logError("Expected ';' after return expression");
        return nullptr;
    }
    next(); // ';'
    return std::make_unique<ReturnStmtAST>(std::move(expr_ast));
}

// Break statement
// Stage variable declaration: in/out type name;
std::unique_ptr<ExprAST> Parser::parseStageVarDecl(bool isInput, int binding) {
    next(); // consume 'in' or 'out'
    std::string type;
    if (isTypeTok(peek())) {
        type = std::string(tokToTypeName(peek()));
        next();
    } else if (peek() == tok_identifier && isUserTypeName(IdentifierStr)) {
        type = IdentifierStr;
        next();
    } else {
        logError("Expected type in in/out declaration");
        return nullptr;
    }
    if (peek() != tok_identifier) { logError("Expected name in in/out declaration"); return nullptr; }
    std::string name = IdentifierStr;
    next();
    if (peek() != tok_semicolon) { logError("Expected ';' after in/out declaration"); return nullptr; }
    next();
    return std::make_unique<StageVarDeclAST>(isInput, type, name, binding);
}

// Continue statement
std::unique_ptr<ExprAST> Parser::parseContinue() {
    next(); // 'continue'
    if (peek() != tok_semicolon) { logError("Expected ';' after continue"); return nullptr; }
    next();
    return std::make_unique<ContinueStmtAST>();
}

// Discard statement
std::unique_ptr<ExprAST> Parser::parseDiscard() {
    next(); // 'discard'
    if (peek() != tok_semicolon) { logError("Expected ';' after discard"); return nullptr; }
    next();
    return std::make_unique<DiscardStmtAST>();
}

std::unique_ptr<ExprAST> Parser::parseBreak() {
    if (peek() != tok_break) {
        logError("Expected 'break' keyword");
        return nullptr;
    }
    next(); // 'break'
    if (peek() != tok_semicolon) {
        logError("Expected ';' after break");
        return nullptr;
    }
    next(); // ';'
    return std::make_unique<BreakStmtAST>();
}

// Variable declaration
std::unique_ptr<ExprAST> Parser::parseVarDecl() {
    // Parse type (builtin or user-defined struct)
    std::string type;
    if (isTypeTok(peek())) {
        type = std::string(tokToTypeName(peek()));
        next();
    } else if (peek() == tok_identifier && isUserTypeName(IdentifierStr)) {
        type = IdentifierStr;
        next();
    } else {
        logError("Expected type at start of variable declaration");
        return nullptr;
    }

    // Variable name
    if (peek() != tok_identifier) {
        logError("Expected variable name after type");
        return nullptr;
    }
    std::string name = IdentifierStr;
    next(); // consume name

    // Optional local array: [size]
    bool isArray   = false;
    int  arraySize = 0;

    if (peek() == tok_lbracket) {
        next(); // '['

        if (peek() != tok_number) {
            logError("Expected array size inside []");
            return nullptr;
        }
        if (NumVal < 1.0 || NumVal > 65536.0) {
            logError(fmt::format("Array size must be between 1 and 65536, got {}", (int)NumVal));
            return nullptr;
        }
        arraySize = static_cast<int>(NumVal);
        next(); // number

        if (peek() != tok_rbracket) {
            logError("Expected ']' in array declaration");
            return nullptr;
        }
        next(); // ']'

        isArray = true;
    }

    // No initializer: "type name;" or "type name[size];"
    if (peek() == tok_semicolon) {
        next(); // ';'

        if (isArray) {
            // local array without initialization
            return std::make_unique<ArrayDeclExprAST>(type, name, arraySize, nullptr);
        }

        // scalars/vectors/matrices get default value
        std::unique_ptr<ExprAST> defaultValue;

        if (type == "bool") {
            defaultValue = std::make_unique<BooleanExprAST>(false);
        } else if (type == "int" || type == "uint") {
            defaultValue = std::make_unique<NumberExprAST>(0.0);
        } else if (type == "float" || type == "double") {
            defaultValue = std::make_unique<NumberExprAST>(0.0);
        } else if (type == "vec2" || type == "vec3" || type == "vec4") {
            std::vector<std::unique_ptr<ExprAST>> args;
            args.push_back(std::make_unique<NumberExprAST>(0.0));
            defaultValue = std::make_unique<CallExprAST>(type, std::move(args));
        } else if (type == "mat2" || type == "mat3" || type == "mat4" ||
                   type == "mat2x2" || type == "mat3x3" || type == "mat4x4" ||
                   type == "mat2x3" || type == "mat2x4" ||
                   type == "mat3x2" || type == "mat3x4" ||
                   type == "mat4x2" || type == "mat4x3") {
            // empty constructor for matrices
            std::vector<std::unique_ptr<ExprAST>> args;
            defaultValue = std::make_unique<CallExprAST>(type, std::move(args));
        } else if (isUserTypeName(type)) {
            // user-defined struct type: use empty constructor
            std::vector<std::unique_ptr<ExprAST>> args;
            defaultValue = std::make_unique<CallExprAST>(type, std::move(args));
        } else {
            logError("Unknown type in variable declaration");
            return nullptr;
        }

        return std::make_unique<AssignmentExprAST>(type, name, std::move(defaultValue));
    }

    // With initialization: '='
    if (peek() == tok_assign) {
        next(); // '='

        // array with initializer: type name[size] = { ... };
        if (isArray) {
            if (peek() != tok_lbrace) {
                logError("Expected '{' for array initializer");
                return nullptr;
            }
            next(); // '{'

            std::vector<std::unique_ptr<ExprAST>> elements;
            if (peek() != tok_rbrace) {
                while (true) {
                    auto elem = parseExpression();
                    if (!elem) return nullptr;
                    elements.push_back(std::move(elem));

                    if (peek() == tok_rbrace) break;
                    if (peek() != tok_comma) {
                        logError("Expected ',' or '}' in array initializer");
                        return nullptr;
                    }
                    next(); // ','
                }
            }

            if (peek() != tok_rbrace) {
                logError("Expected '}' at end of array initializer");
                return nullptr;
            }
            next(); // '}'

            if (static_cast<int>(elements.size()) != arraySize) {
                logError("Array initializer size mismatch");
                return nullptr;
            }

            if (peek() != tok_semicolon) {
                logError("Expected ';' after array declaration");
                return nullptr;
            }
            next(); // ';'

            auto init = std::make_unique<ArrayInitExprAST>(std::move(elements));
            return std::make_unique<ArrayDeclExprAST>(type, name, arraySize, std::move(init));
        }

        // 5b) regular variable: type name = expr;
        auto value = parseExpression();
        if (!value) return nullptr;

        if (peek() != tok_semicolon) {
            logError("Expected ';' at end of declaration");
            return nullptr;
        }
        next(); // ';'

        return std::make_unique<AssignmentExprAST>(type, name, std::move(value));
    }

    logError("Expected '=' or ';' after variable name in declaration");
    return nullptr;
}

std::unique_ptr<ExprAST> Parser::parseAssignmentOrExprStatement() {
    if (peek() != tok_identifier) {
        logError("Expected identifier at start of statement");
        return nullptr;
    }
    // parse left-hand side using primary expression
    auto lhs = parsePrimary();
    if (!lhs) return nullptr;
    // Compound assignment: a += rhs, a -= rhs, a *= rhs, a /= rhs
    if (peek() == tok_plus_assign || peek() == tok_minus_assign ||
        peek() == tok_mul_assign  || peek() == tok_div_assign) {
        int op = peek();
        next();
        auto rhs = parseExpression();
        if (!rhs) return nullptr;
        if (peek() != tok_semicolon) { logError("Expected ';' after compound assignment"); return nullptr; }
        next();
        int binOp;
        switch (op) {
            case tok_plus_assign:  binOp = tok_plus;     break;
            case tok_minus_assign: binOp = tok_minus;    break;
            case tok_mul_assign:   binOp = tok_multiply; break;
            default:               binOp = tok_divide;   break;
        }
        if (auto* var = dynamic_cast<VariableExprAST*>(lhs.get())) {
            auto lhsCopy = std::make_unique<VariableExprAST>(var->Name);
            auto binExpr = std::make_unique<BinaryExprAST>(binOp, std::move(lhsCopy), std::move(rhs));
            return std::make_unique<AssignmentExprAST>("", var->Name, std::move(binExpr));
        }
        logError("Compound assignment only supported for simple variables");
        return nullptr;
    }

    // assignment statement - example : a = 5;
    // ReSharper disable once CppDFAConstantConditions
    if (peek() == tok_assign) {
        next(); // '='
        auto rhs = parseExpression();
        if (!rhs) return nullptr;

        if (peek() != tok_semicolon) {
            logError("Expected ';' after assignment");
            return nullptr;
        }
        next(); // ';'

        // matrix assignment: a[i] = col_vec  or  a[i][j] = scalar
        if (auto *maccRaw = dynamic_cast<MatrixAccessExprAST*>(lhs.get())) {
            std::unique_ptr<MatrixAccessExprAST> macc(maccRaw);
            lhs.release();
            return std::make_unique<MatrixAssignmentExprAST>(
                std::move(macc->Object),
                std::move(macc->Index),
                std::move(macc->Index2),
                std::move(rhs));
        }

        // member assignment: v.xyz = ..., obj.field = ...
        if (auto *memRaw = dynamic_cast<MemberAccessExprAST*>(lhs.get())) {
            std::unique_ptr<MemberAccessExprAST> mem(memRaw);
            lhs.release();
            auto obj    = std::move(mem->Object);
            std::string member = mem->Member;
            return std::make_unique<MemberAssignmentExprAST>(
                std::move(obj), member, std::move(rhs));
        }

        // simple variable: a = ...
        if (auto *var = dynamic_cast<VariableExprAST*>(lhs.get())) {
            return std::make_unique<AssignmentExprAST>("", var->Name, std::move(rhs));
        }

        logError("Unsupported lvalue in assignment");
        return nullptr;
    }
    // expression statement - example : a + 1;
    auto expr = parseBinOpRHS(0, std::move(lhs));
    if (!expr) return nullptr;
    if (peek() != tok_semicolon) {
        logError("Expected ';' after expression");
        return nullptr;
    }
    next(); // ';'
    return expr;
}

// parse expression, including optional ternary
std::unique_ptr<ExprAST> Parser::parseExpression() {
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    auto expr = parseBinOpRHS(0, std::move(lhs));
    if (!expr) return nullptr;

    // Ternary: expr ? thenExpr : elseExpr
    if (peek() == tok_question) {
        next(); // '?'
        auto thenExpr = parseExpression();
        if (!thenExpr) return nullptr;
        if (peek() != tok_colon) { logError("Expected ':' in ternary operator"); return nullptr; }
        next(); // ':'
        auto elseExpr = parseExpression();
        if (!elseExpr) return nullptr;
        return std::make_unique<TernaryExprAST>(std::move(expr), std::move(thenExpr), std::move(elseExpr));
    }
    return expr;
}

// for unary operators
// example : -x, ++x, --x
std::unique_ptr<ExprAST> Parser::parseUnary() {
    // prefix ++ / --
    if (peek() == tok_increment || peek() == tok_decrement) {
        bool isInc = (peek() == tok_increment);
        next();
        auto operand = parseUnary();
        if (!operand) return nullptr;
        if (auto* var = dynamic_cast<VariableExprAST*>(operand.get())) {
            auto one     = std::make_unique<NumberExprAST>(1.0);
            auto varCopy = std::make_unique<VariableExprAST>(var->Name);
            int binOp    = isInc ? tok_plus : tok_minus;
            auto binExpr = std::make_unique<BinaryExprAST>(binOp, std::move(varCopy), std::move(one));
            return std::make_unique<AssignmentExprAST>("", var->Name, std::move(binExpr));
        }
        logError("Prefix ++/-- only supported on simple variables");
        return nullptr;
    }

    if (peek() == tok_minus || peek() == '-' ||
        peek() == tok_plus  || peek() == '+' ||
        peek() == tok_not   || peek() == '!') {
        int Op = peek();
        next();
        auto operand = parseUnary();
        if (!operand) return nullptr;
        return std::make_unique<UnaryExprAST>(Op, std::move(operand));
    }
    return parsePrimary();
}

// parse primary expressions
// example: identifier, number, (expression), constructor call, member access, swizzle,
std::unique_ptr<ExprAST> Parser::parsePrimary() {
    std::unique_ptr<ExprAST> expr;
    switch (peek()) {
        case tok_identifier:
        case tok_vec2:
        case tok_vec3:
        case tok_vec4:
        case tok_double:
        case tok_float:
        case tok_int:
        case tok_uint:
        case tok_bool:
        case tok_mat2:
        case tok_mat3:
        case tok_mat4:
        case tok_mat2x3:
        case tok_mat2x4:
        case tok_mat3x2:
        case tok_mat3x4:
        case tok_mat4x2:
        case tok_mat4x3:
        case tok_uvec2: case tok_uvec3: case tok_uvec4:
        case tok_ivec2: case tok_ivec3: case tok_ivec4:
        case tok_sampler2D: case tok_sampler3D: case tok_samplerCube:
        case tok_image2D:
            expr = parseIdentifierOrCtorExpr();
            break;
        case tok_number:
            expr = parseNumberExpr();
            break;
        case tok_lparen: {
            next(); // '('
            expr = parseExpression();
            if (!expr) return nullptr;
            if (peek() != tok_rparen) {
                logError("Expected ')'");
                return nullptr;
            }
            next(); // ')'
            break;
        }
        case tok_true:
            next(); // 'true'
            expr = std::make_unique<BooleanExprAST>(true);
            break;
        case tok_false:
            next(); // 'false'
            expr = std::make_unique<BooleanExprAST>(false);
            break;
        default:
            logError("Unknown token when expecting an expression");
            return nullptr;
    }
    if (!expr) return nullptr;
    // Handle postfix operators (like member access/swizzle)
    expr = parsePostfixAfterIdent(std::move(expr));
    return expr;
}

// number literal
// example: 42, 3.14
std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
    auto result = std::make_unique<NumberExprAST>(NumVal, IsIntLiteral);
    next();
    return result;
}

// identifier or constructor call
// example: myVar, vec3(1.0, 0.0, 0.0)
std::unique_ptr<ExprAST> Parser::parseIdentifierOrCtorExpr() {
    std::string name;
    if (isTypeTok(peek())) {
        // Constructor type call e.g., vec3(1.0, 0.0, 0.0)
        name = std::string(tokToTypeName(peek()));
        next(); // type token
        if (peek() != tok_lparen) {
            logError("Type name in expression must be followed by '(' (constructor)");
            return nullptr;
        }
    }
    // Function call, variable, or user-defined type constructor
    else if (peek() == tok_identifier) {
        name = IdentifierStr;
        next(); // identifier
        // simple variable if not followed by '('
        if (peek() != tok_lparen) {
            return std::make_unique<VariableExprAST>(name);
        }
    } else {
        logError("Expected identifier or type constructor");
        return nullptr;
    }
    // function or constructor call
    next(); // '('
    std::vector<std::unique_ptr<ExprAST> > args;
    if (peek() != tok_rparen) {
        while (true) {
            auto arg = parseExpression();
            if (!arg) return nullptr;
            args.push_back(std::move(arg));
            if (peek() == tok_rparen) break;
            if (peek() != tok_comma) {
                logError("Expected ',' in argument list");
                return nullptr;
            }
            next(); // ','
        }
    }
    next(); // ')'
    // CallExprAST - function call or user-defined type constructor
    return std::make_unique<CallExprAST>(name, std::move(args));
}

std::unique_ptr<ExprAST> Parser::parsePostfixAfterIdent(std::unique_ptr<ExprAST> base) {
    while (true) {
        // member access or swizzle - someVec.x or someVec.xyz
        if (peek() == tok_dot) {
            next();
            if (peek() != tok_identifier) {
                logError("Expected member/swizzle name after '.'");
                return nullptr;
            }
            // check if single component or swizzle
            std::string member = IdentifierStr;
            next();
            base = std::make_unique<MemberAccessExprAST>(std::move(base), member);
            continue;
        }
        // matrix access - someMat[i] or someMat[i][j]
        if (peek() == tok_lbracket) {
            auto idx1 = parseBracketIndex();
            if (!idx1) return nullptr;

            if (peek() == tok_lbracket) {
                auto idx2 = parseBracketIndex();
                if (!idx2) return nullptr;
                base = std::make_unique<MatrixAccessExprAST>(
                    std::move(base),
                    std::move(idx1),
                    std::move(idx2)
                );
            } else {
                base = std::make_unique<MatrixAccessExprAST>(
                    std::move(base),
                    std::move(idx1)
                );
            }
            continue;
        }
        // postfix ++ / --
        if (peek() == tok_increment || peek() == tok_decrement) {
            bool isInc = (peek() == tok_increment);
            next();
            if (auto* var = dynamic_cast<VariableExprAST*>(base.get())) {
                auto one     = std::make_unique<NumberExprAST>(1.0);
                auto varCopy = std::make_unique<VariableExprAST>(var->Name);
                int binOp    = isInc ? tok_plus : tok_minus;
                auto binExpr = std::make_unique<BinaryExprAST>(binOp, std::move(varCopy), std::move(one));
                base = std::make_unique<AssignmentExprAST>("", var->Name, std::move(binExpr));
            }
            continue;
        }
        break;
    }
    return base;
}

// binary operators - right-hand side
// exprPrecedence - minimal precedence for this level, lhs - already parsed left-hand side
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int exprPrecedence, std::unique_ptr<ExprAST> lhs) {
    while (true) {
        // get precedence of current
        int tokPrecedence = precedence(peek());
        // if this operator is lower precedence than what we are allowed, return lhs
        if (tokPrecedence < exprPrecedence) return lhs;
        // this is a binary operator
        int op = peek();
        next(); // operator
        // parse unary expression after the binary operator
        auto rhs = parseUnary();
        if (!rhs) return nullptr;
        int nextPrecedence = precedence(peek());
        if (tokPrecedence < nextPrecedence) {
            rhs = parseBinOpRHS(tokPrecedence + 1, std::move(rhs));
            if (!rhs) return nullptr;
        }
        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
}

static Parser GParser;

int getNextToken() { return GParser.next(); }
std::vector<std::unique_ptr<ExprAST> > ParseProgram() { return GParser.ParseProgram(); }
std::unique_ptr<ExprAST> ParsePrimary() { return GParser.parsePrimary(); }
std::unique_ptr<ExprAST> ParseUnary() { return GParser.parseUnary(); }
std::unique_ptr<ExprAST> ParseExpression() { return GParser.parseExpression(); }
std::unique_ptr<ExprAST> ParseStatement() { return GParser.parseStatement(); }
std::unique_ptr<ExprAST> ParseFunction() { return GParser.parseFunction(); }
std::unique_ptr<ExprAST> ParseStructDecl() { return GParser.parseStruct(); }
std::unique_ptr<ExprAST> ParseUniformDecl() { return GParser.parseUniform(); }
