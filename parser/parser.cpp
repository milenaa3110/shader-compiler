#include "../parser/parser.h"
#include "../error_utils.h"
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
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

    // peek at Parser::peek()rent token
    int peek() const { return Cur; }

    std::vector<std::unique_ptr<ExprAST> > ParseProgram();

    std::unique_ptr<ExprAST> parseStruct();

    std::unique_ptr<ExprAST> parseUniform();

    std::unique_ptr<ExprAST> parseFunction();

    std::unique_ptr<ExprAST> parseStatement();

    std::unique_ptr<ExprAST> parseBlockExpr();

    std::unique_ptr<ExprAST> parseIfExpr();

    std::unique_ptr<ExprAST> parseWhile();

    std::unique_ptr<ExprAST> parseFor();

    std::unique_ptr<ExprAST> parseReturn();

    std::unique_ptr<ExprAST> parseBreak();

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
    while (peek() != tok_eof) {
        std::unique_ptr<ExprAST> stmt = nullptr;
        if (peek() == tok_fn) {
            stmt = parseFunction();
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
        std::cerr << "Expected struct name after 'struct'\n";
        return nullptr;
    }
    std::string structName = IdentifierStr;
    next(); // struct name
    
    if (peek() != tok_lbrace) {
        std::cerr << "Expected '{' after struct name\n";
        return nullptr;
    }
    next(); // '{'
    std::vector<std::pair<std::string, std::string> > fields;
    while (peek() != tok_rbrace) {
        if (peek() != tok_identifier && !isTypeTok(peek())) {
            std::cerr << "Expected field type in struct\n";
            return nullptr;
        }
        std::string fieldType;
        if (isTypeTok(peek())) {
            fieldType = std::string(tokToTypeName(peek()));
        } else {
            // check if it's a user-defined struct type
            if (!isUserTypeName(IdentifierStr)) {
                std::cerr << "Unknown type '" << IdentifierStr << "' in struct field\n";
                return nullptr;
            }
            fieldType = IdentifierStr;
        }
        next(); // type
        if (peek() != tok_identifier) {
            std::cerr << "Expected field name in struct\n";
            return nullptr;
        }
        std::string fieldName = IdentifierStr;
        next(); // field name
        if (peek() != tok_semicolon) {
            std::cerr << "Expected ';' after struct field\n";
            return nullptr;
        }
        next(); // ';'
        fields.emplace_back(fieldType, fieldName);
    }
    if (peek() != tok_rbrace) {
        std::cerr << "Expected '}' after struct body\n";
        return nullptr;
    }
    next(); // '}'
    if (peek() != tok_semicolon) {
        std::cerr << "Expected ';' after struct declaration\n";
        return nullptr;
    }
    next(); // ';'
    
    // add to struct types before cycle check (needed for dependency lookup)
    StructTypes.insert(structName);
    
    // check for recursive struct definitions (both direct and indirect)
    if (hasRecursiveStructs(structName, fields)) {
        std::cerr << "Error: Struct '" << structName << "' contains recursive definition\n";
        StructTypes.erase(structName);
        StructDependencies.erase(structName);
        return nullptr;
    }
    
    return std::make_unique<StructDeclExprAST>(structName, std::move(fields));
}

// Uniform declaration
std::unique_ptr<ExprAST> Parser::parseUniform() {
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
        arraySize = static_cast<int>(NumVal);
        if (arraySize <= 0) {
            logError("Array size must be positive");
            return nullptr;
        }
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
        case tok_if:
            return parseIfExpr();
        case tok_lbrace:
            return parseBlockExpr();
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
        std::cerr << "Expected '}' at end of block\n";
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
        std::cerr << "Expected ')' in if condition\n";
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
        arraySize = static_cast<int>(NumVal);
        if (arraySize <= 0) {
            logError("Array size must be positive");
            return nullptr;
        }
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

        // matrix assignment: a[i] = ... or a[i][j] = ...
        if (auto *maccRaw = dynamic_cast<MatrixAccessExprAST*>(lhs.get())) {
            // For matrix element assignment, we need special handling
            // This will be handled in MatrixAccessExprAST or a dedicated assignment class
            logError("Direct matrix element assignment not yet fully supported");
            return nullptr;
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

// parse binary operator right-hand side
// example : lhs op rhs
std::unique_ptr<ExprAST> Parser::parseExpression() {
    // parse left-hand side (first operand)
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    return parseBinOpRHS(0, std::move(lhs));
}

// for unary operators
// example : -x
std::unique_ptr<ExprAST> Parser::parseUnary() {
    if (peek() == tok_minus || peek() == '-' || 
        peek() == tok_plus || peek() == '+' || 
        peek() == tok_not || peek() == '!') {
        int Op = peek();
        next(); // unary operator
        // for --x or ++x, it is handled here
        auto operand = parseUnary();
        if (!operand) return nullptr;
        return std::make_unique<UnaryExprAST>(Op, std::move(operand));
    }
    // if the current token is not unary operator, it must be a primary expr
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
    auto result = std::make_unique<NumberExprAST>(NumVal);
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
