#include "parser.h"
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Lexer interfejs
extern int gettok();
extern std::string IdentifierStr;
extern double NumVal;

// Global zbog kompatibilnosti sa ostatkom koda
int CurTok = 0;

class Parser {
public:
    int Cur = 0;

    inline int next() {
        Cur = gettok();
        CurTok = Cur;       // drži global u sinhronu, ali se interno koristi Cur
        return Cur;
    }

    inline int peek() const { return Cur; }

    std::unique_ptr<ExprAST> parseExpression();
//    std::unique_ptr<ExprAST> parseAssignment();
    std::unique_ptr<ExprAST> parseStatement();
    std::unique_ptr<ExprAST> parsePrimary();
    std::unique_ptr<ExprAST> parseUnary();
    std::unique_ptr<ExprAST> parseNumberExpr();
    std::unique_ptr<ExprAST> parseIdentifierOrCtorExpr();
    std::unique_ptr<ExprAST> parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs);
    std::unique_ptr<ExprAST> parseIfExpr();
    std::unique_ptr<ExprAST> parseBlockExpr();

private:
    static inline int precedence(int tok) {
        switch (tok) {
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


    static inline bool isTypeTok(int t) {
        switch (t) {
            case tok_vec2: case tok_vec3: case tok_vec4:
            case tok_double: case tok_float:
            case tok_int: case tok_uint: case tok_bool: case tok_mat2:
            case tok_mat3: case tok_mat4: case tok_mat2x3:
            case tok_mat2x4: case tok_mat3x2:
            case tok_mat3x4: case tok_mat4x2: case tok_mat4x3:
                return true;
            default: return false;
        }
    }

    static inline std::string_view tokToTypeName(int t) {
        switch (t) {
            case tok_vec2:   return "vec2";
            case tok_vec3:   return "vec3";
            case tok_vec4:   return "vec4";
            case tok_double: return "double";
            case tok_float:  return "float";
            case tok_int:    return "int";
            case tok_uint:   return "uint";
            case tok_bool:   return "bool";
            case tok_mat2:   return "mat2";
            case tok_mat3:   return "mat3";
            case tok_mat4:   return "mat4";
            case tok_mat2x3: return "mat2x3";
            case tok_mat2x4: return "mat2x4";
            case tok_mat3x2: return "mat3x2";
            case tok_mat3x4: return "mat3x4";
            case tok_mat4x2: return "mat4x2";
            case tok_mat4x3: return "mat4x3";
            default:         return "";
        }
    }

    static std::unique_ptr<ExprAST> err(const char* msg) {
        std::cerr << msg << '\n';
        return nullptr;
    }

    std::unique_ptr<ExprAST> parsePostfixAfterIdent(std::unique_ptr<ExprAST> base) {
        while (true) {
            if (Cur == tok_dot) {
                next();
                if (Cur != tok_identifier) return err("Expected member/swizzle name after '.'");
                std::string member = IdentifierStr;
                next();
                if (member.size() == 1)
                    base = std::make_unique<MemberAccessExprAST>(std::move(base), member);
                else
                    base = std::make_unique<SwizzleExprAST>(std::move(base), member);
                continue;
            }

            if (Cur == tok_lbracket) {
                next(); // '['
                auto idx1 = parseExpression();
                if (!idx1) return nullptr;
                if (Cur != tok_rbracket) return err("Expected ']'");
                next(); // ']'

                if (Cur == tok_lbracket) {
                    next(); // '['
                    auto idx2 = parseExpression();
                    if (!idx2) return nullptr;
                    if (Cur != tok_rbracket) return err("Expected ']'");
                    next(); // ']'
                    base = std::make_unique<MatrixAccessExprAST>(std::move(base), std::move(idx1), std::move(idx2));
                } else {
                    base = std::make_unique<MatrixAccessExprAST>(std::move(base), std::move(idx1));
                }
                continue;
            }

            break;
        }

        return base;
    }
};

std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
    auto result = std::make_unique<NumberExprAST>(NumVal);
    next(); // potroši broj
    return result;
}

std::unique_ptr<ExprAST> Parser::parseIdentifierOrCtorExpr() {
    std::string name;

    if (isTypeTok(Cur)) {
        // Tip u primarnom izrazu mora da bude konstruktor: tip '(' ...
        name = std::string(tokToTypeName(Cur));
        next(); // tip
        if (Cur != tok_lparen)
            return err("Type name in expression must be followed by '(' (constructor)");
    } else if (Cur == tok_identifier) {
        name = IdentifierStr;
        next(); // ident
        if (Cur != tok_lparen) {
            // promenljiva
            return std::make_unique<VariableExprAST>(name);
        }
    } else {
        return err("Expected identifier or type constructor");
    }

    // Poziv: name '(' arglist ')'
    next(); // '('
    std::vector<std::unique_ptr<ExprAST>> args;
    if (Cur != tok_rparen) {
        while (true) {
            auto arg = parseExpression();
            if (!arg) return nullptr;
            args.push_back(std::move(arg));

            if (Cur == tok_rparen) break;
            if (Cur != tok_comma) return err("Expected ',' in argument list");
            next(); // ','
        }
    }
    next(); // ')'

    return std::make_unique<CallExprAST>(name, std::move(args));
}

std::unique_ptr<ExprAST> Parser::parsePrimary() {
    std::unique_ptr<ExprAST> expr;

    switch (Cur) {
        case tok_identifier:
        case tok_vec2: case tok_vec3: case tok_vec4:
        case tok_double: case tok_float: case tok_int: case tok_uint: case tok_bool:
            expr = parseIdentifierOrCtorExpr();
            break;

        case tok_number:
            expr = parseNumberExpr();
            break;

        case tok_lparen: {
            next(); // '('
            expr = parseExpression();
            if (!expr) return nullptr;
            if (Cur != tok_rparen) return err("Expected ')'");
            next(); // ')'
            break;
        }

        case tok_true:
            next();
            expr = std::make_unique<BooleanExprAST>(true);
            break;

        case tok_false:
            next();
            expr = std::make_unique<BooleanExprAST>(false);
            break;

        default:
            return err("Unknown token when expecting an expression");
    }

    if (!expr) return nullptr;

    // Handle postfix operators (like member access/swizzle)
    while (Cur == tok_dot) {
        next(); // consume '.'
        if (Cur != tok_identifier) {
            return err("Expected member name after '.'");
        }
        std::string member = IdentifierStr;
        next(); // consume member name

        // Proveri da li je single component ili swizzle
        if (member.length() == 1) {
            expr = std::make_unique<MemberAccessExprAST>(std::move(expr), member);
        } else {
            expr = std::make_unique<SwizzleExprAST>(std::move(expr), member);
        }
    }

    while (Cur == tok_lbracket) {
        next(); // '['
        auto index = parseExpression();
        if (!index) return nullptr;
        if (Cur != tok_rbracket) return err("Expected ']'");
        next(); // ']'

        // Proveri da li sledi još jedan '[' za mat[i][j]
        if (Cur == tok_lbracket) {
            next(); // '['
            auto index2 = parseExpression();
            if (!index2) return nullptr;
            if (Cur != tok_rbracket) return err("Expected ']'");
            next(); // ']'
            expr = std::make_unique<MatrixAccessExprAST>(std::move(expr), std::move(index), std::move(index2));
        } else {
            expr = std::make_unique<MatrixAccessExprAST>(std::move(expr), std::move(index));
        }
    }

    return expr;
}


// ---- unary (+/-) ----
std::unique_ptr<ExprAST> Parser::parseUnary() {
    if (Cur == tok_minus || Cur == '-' || Cur == tok_plus || Cur == '+') {
        int Op = Cur;
        next(); // pojedi unarni operator
        auto operand = parseUnary(); // desna asocijativnost: --x
        if (!operand) return nullptr;
        return std::make_unique<UnaryExprAST>(Op, std::move(operand));
    }
    return parsePrimary();
}

// ---- binarni (precedence climbing) ----
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int exprPrec, std::unique_ptr<ExprAST> lhs) {
    while (true) {
        int tokPrec = precedence(Cur);
        if (tokPrec < exprPrec) return lhs;

        int op = Cur;
        next(); // operator

        auto rhs = parseUnary();
        if (!rhs) return nullptr;

        int nextPrec = precedence(Cur);
        if (tokPrec < nextPrec) {
            rhs = parseBinOpRHS(tokPrec + 1, std::move(rhs));
            if (!rhs) return nullptr;
        }

        lhs = std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));
    }
}

std::unique_ptr<ExprAST> Parser::parseExpression() {
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    return parseBinOpRHS(0, std::move(lhs));
}

std::unique_ptr<ExprAST> Parser::parseIfExpr() {
    // očekujemo da je Cur == tok_if u trenutku poziva
    next(); // pojedi 'if'

    if (Cur != tok_lparen /* ili '(' */) {
        std::cerr << "Expected '(' after 'if'\n";
        return nullptr;
    }
    next(); // '('

    auto cond = parseExpression();
    if (!cond) return nullptr;

    if (Cur != tok_rparen /* ili ')' */) {
        std::cerr << "Expected ')' in if condition\n";
        return nullptr;
    }
    next(); // ')'

    // then: ili blok { ... } ili jedan statement
    std::unique_ptr<ExprAST> thenExpr;
    if (Cur == tok_lbrace /* ili '{' */) {
        thenExpr = parseBlockExpr();
    } else {
        thenExpr = parseStatement();
    }
    if (!thenExpr) return nullptr;

    std::unique_ptr<ExprAST> elseExpr;
    if (Cur == tok_else) {
        next(); // 'else'
        if (Cur == tok_lbrace /* ili '{' */) {
            elseExpr = parseBlockExpr();
        } else {
            elseExpr = parseStatement();
        }
        if (!elseExpr) return nullptr;
    }

    return std::make_unique<IfExprAST>(std::move(cond), std::move(thenExpr), std::move(elseExpr));
}

std::unique_ptr<ExprAST> Parser::parseBlockExpr() {
    // očekujemo '{'
    next(); // '{'

    std::vector<std::unique_ptr<ExprAST>> statements;
    while (Cur != tok_rbrace /* ili '}' */ && Cur != tok_eof) {
        auto stmt = parseStatement();
        if (!stmt) return nullptr;
        statements.push_back(std::move(stmt));
    }

    if (Cur != tok_rbrace /* ili '}' */) {
        std::cerr << "Expected '}' at end of block\n";
        return nullptr;
    }
    next(); // '}'

    return std::make_unique<BlockExprAST>(std::move(statements));
}


// ---- statement ----
std::unique_ptr<ExprAST> Parser::parseStatement() {
    if (Cur == tok_if) {
        return parseIfExpr();
    }
    if (Cur == tok_lbrace /* ili '{' */) {
        return parseBlockExpr();
    }
    // 1) Deklaracija: tip ident ['=' expr] ';'
    if (isTypeTok(Cur)) {
        std::string type = std::string(tokToTypeName(Cur));
        next(); // tip

        if (Cur != tok_identifier) {
            std::string varName = IdentifierStr;
            next();

            if (Cur == tok_dot) {
                next(); // '.'
                if (Cur != tok_identifier) return err("Expected swizzle after '.'");
                std::string swizzle = IdentifierStr;
                next();
                if (Cur != tok_assign) return err("Expected '=' after swizzle");
                next(); // '='
                auto value = parseExpression();
                if (!value) return nullptr;
                if (Cur != tok_semicolon) return err("Expected ';'");
                next();

                auto var = std::make_unique<VariableExprAST>(varName);
                return std::make_unique<SwizzleAssignmentExprAST>(std::move(var), swizzle, std::move(value));
            }
        }

        std::string name = IdentifierStr;
        next(); // ime

        if (Cur == tok_assign) {
            // Sa inicijalizacijom
            next(); // '='
            auto value = parseExpression();
            if (!value) return nullptr;
            if (Cur != tok_semicolon) {
                return err("Expected ';' at end of declaration");
            }
            next(); // ';'
            return std::make_unique<AssignmentExprAST>(type, name, std::move(value));
        } else if (Cur == tok_semicolon) {
            // Bez inicijalizacije - kreiraj default vrednost
            next(); // ';'
            std::unique_ptr<ExprAST> defaultValue;

            // Default vrednosti po tipovima
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
            } else if (type == "vec2" || type == "vec3" || type == "vec4"
                    || type == "mat2" || type == "mat3" || type == "mat4"
                    || type == "mat2x2" || type == "mat3x3" || type == "mat4x4"
                    || type == "mat2x3" || type == "mat2x4"
                    || type == "mat3x2" || type == "mat3x4"
                    || type == "mat4x2" || type == "mat4x3") {
                            std::vector<std::unique_ptr<ExprAST>> args; // prazno → identitet za mat, nule za vec
                            defaultValue = std::make_unique<CallExprAST>(type, std::move(args));
                    }
            else {
                return err("Unknown type for default initialization");
            }

            return std::make_unique<AssignmentExprAST>(type, name, std::move(defaultValue));
        } else {
            return err("Expected '=' or ';' after variable name in declaration");
        }
    }

    if (Cur == tok_identifier) {
        std::string baseName = IdentifierStr;
        next(); // pojedi ident

        // VAŽNO: lhs je ExprAST, ne VariableExprAST
        std::unique_ptr<ExprAST> lhs = std::make_unique<VariableExprAST>(baseName);
        lhs = parsePostfixAfterIdent(std::move(lhs));
        if (!lhs) return nullptr;

        if (Cur == tok_assign) {
            next(); // '='
            auto rhs = parseExpression();
            if (!rhs) return nullptr;
            if (Cur != tok_semicolon) return err("Expected ';' after assignment");
            next(); // ';'

            if (auto* maccRaw = dynamic_cast<MatrixAccessExprAST*>(lhs.get())) {
                std::unique_ptr<MatrixAccessExprAST> macc(maccRaw);
                lhs.release();

                return std::make_unique<MatrixAssignmentExprAST>(
                    std::move(macc->Object),
                    std::move(macc->Index),
                    std::move(macc->Index2),
                    std::move(rhs)
                );
            }
            // 3.1) Swizzle dodela: someVec.wzyx = ...
            if (auto* swRaw = dynamic_cast<SwizzleExprAST*>(lhs.get())) {
                // preuzmi ownership iz lhs da bi izvukla Object
                std::unique_ptr<SwizzleExprAST> sw(swRaw);
                lhs.release(); // lhs više ne upravlja pokazivačem

                auto obj  = std::move(sw->Object); // pretpostavka: public field
                auto mask = sw->Swizzle;           // string
                return std::make_unique<SwizzleAssignmentExprAST>(std::move(obj), mask, std::move(rhs));
            }

            // 3.2) Member (jedna komponenta): someVec.x = ...
            if (auto* memRaw = dynamic_cast<MemberAccessExprAST*>(lhs.get())) {
                std::unique_ptr<MemberAccessExprAST> mem(memRaw);
                lhs.release();

                auto obj  = std::move(mem->Object);
                std::string mask = mem->Member; // "x"/"y"/"z"/"w"
                return std::make_unique<SwizzleAssignmentExprAST>(std::move(obj), mask, std::move(rhs));
            }

            // 3.3) Obična promenljiva: a = ...
            if (auto* var = dynamic_cast<VariableExprAST*>(lhs.get())) {
                return std::make_unique<AssignmentExprAST>("", var->Name, std::move(rhs));
            }

            return err("Unsupported lvalue in assignment");
        }

        // expression statement (npr. someVec.x; ili a + 1;)
        auto expr = parseBinOpRHS(0, std::move(lhs));
        if (!expr) return nullptr;
        if (Cur != tok_semicolon) return err("Expected ';' after expression");
        next(); // ';'
        return expr;
    }
}

// Jedan globalni parser
static Parser GParser;

// Spoljašnji API (kompatibilnost)
int getNextToken() { return GParser.next(); }
std::unique_ptr<ExprAST> ParsePrimary()    { return GParser.parsePrimary(); }
std::unique_ptr<ExprAST> ParseUnary()      { return GParser.parseUnary(); }
std::unique_ptr<ExprAST> ParseExpression() { return GParser.parseExpression(); }
std::unique_ptr<ExprAST> ParseStatement()  { return GParser.parseStatement(); }
