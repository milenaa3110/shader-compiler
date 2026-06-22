#include "parser.h"
#include "../ast/ast_context.h"
#include "../../common/error_utils_fmt.h"
#include "../lexer/lexer.h"
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fmt/core.h>

// The builtin type-keyword list is no longer hand-maintained here: isTypeTok,
// tokToTypeName, and parsePrimary all expand ../ast/builtin_types.def (the same
// source the lexer, sema, and codegen use), so the lists can't drift. `void` is
// not in it (function-return type only) and is handled separately at each site.

class Parser {
public:
    Parser(ASTContext& ctx, std::string_view source) : Ctx(ctx), Lex(source) {
        // LL(2) sliding window.
        Cur  = Lex.next();
        Next = Lex.next();
    }

    TokenKind next() {
        Cur  = Next;
        Next = Lex.next();
        return Cur.kind;
    }

    // Kind of the token the parser is currently looking at.
    TokenKind peek() const { return Cur.kind; }

    // Kind of the token after cur
    TokenKind peekNext() const { return Next.kind; }

    // Logs a syntax error at the current token position. 
    // Caps error output at 'kMaxErrors' to prevent terminal spam.
    // Returns std::nullptr_t so it can be chained directly as 'return error("...");'
    // in AST parsing functions that return pointers.
    std::nullptr_t error(const std::string& msg) {
        if (errorCount_ < kMaxErrors) {
            logErrorAt(Cur.loc, msg);
            ++errorCount_;
        } else if (errorCount_ == kMaxErrors) {
            logErrorAt(Cur.loc,
                       "too many parse errors; remaining errors suppressed");
            ++errorCount_;
        }
        return nullptr;
    }

    // Panic-mode recovery: advance until we hit a token that's likely to
    // begin a fresh, well-formed construct. Used after a parse failure to
    // skip the rest of the broken statement / top-level item and resume
    // parsing — gives the user *all* their errors in one compile.
    void synchronize() {
        while (peek() != TokenKind::Eof) {
            if (peek() == TokenKind::Semicolon) { next(); return; }
            if (peek() == TokenKind::Rbrace)    { next(); return; }
            // Don't consume — these *start* the next construct.
            if (peek() == TokenKind::Fn     ||
                peek() == TokenKind::Struct ||
                peek() == TokenKind::At) {
                return;
            }
            next();
        }
    }

    int errorCount() const { return errorCount_; }

    // Stamp a freshly-built AST node with a source position and return it.
    // Takes a raw pointer (the ASTContext owns the storage); a null pointer
    // passes through unchanged for the "log error, return nullptr" pattern.
    template <class T>
    static T* at(T* n, SourceLocation loc) {
        if (n) n->loc = loc;
        return n;
    }

    // RAII position-stamper. Construct at the start of a parse method to
    // snapshot the current token's location; call stamp(node) at every return
    // point to stamp without rewriting `SourceLocation _l = …; … at(node, _l)`
    // boilerplate. Stays a no-op for null nodes, matching at().
    class ScopedLoc {
     public:
        explicit ScopedLoc(const Parser& p) : loc_(p.Cur.loc) {}
        template <class T>
        T* stamp(T* n) const { return Parser::at(n, loc_); }
        SourceLocation loc() const { return loc_; }
     private:
        SourceLocation loc_;
    };

    std::vector<ExprAST*> ParseProgram();

    ExprAST* parseStruct();
    ExprAST* parseFunction();
    std::optional<FunctionAttrs> parseFunctionAttrs();
    static std::optional<ShaderStage> stageFromTok(TokenKind tok);
    ExprAST* parseStatement();
    ExprAST* parseBlockExpr();
    ExprAST* parseIfExpr();
    ExprAST* parseWhile();
    ExprAST* parseFor();
    ExprAST* parseReturn();
    ExprAST* parseBreak();
    ExprAST* parseStageVarDecl(bool isInput, int binding = -1);
    ExprAST* parseContinue();
    ExprAST* parseDiscard();
    ExprAST* parseUniform(int binding = -1);
    ExprAST* parseStorageBuffer(int binding = -1);
    ExprAST* parseVarDecl();

    // Parse an assignment / compound-assignment / postfix / expression
    // statement. With `consumeSemi=true` (default, statement context) the
    // trailing `;` is required and consumed; with `consumeSemi=false`
    // (used by parseFor for the increment slot, which is terminated by
    // `)`) no `;` is expected.
    ExprAST* parseAssignmentOrExprStatement(bool consumeSemi = true);

    ExprAST* parseExpression();
    ExprAST* parseUnary();
    ExprAST* parsePrimary();
    ExprAST* parseNumberExpr();
    ExprAST* parseIdentifierOrCtorExpr();
    ExprAST* parsePostfixAfterIdent(ExprAST* base);
    ExprAST* parseBinOpRHS(int exprPrecedence, ExprAST* lhs);

private:
    ASTContext& Ctx;  // arena that owns every node this parser builds
    Lexer Lex;        // buffer-backed scanner
    Token Cur;        // the token the parser is currently looking at
    Token Next;       // one-token lookahead (token *after* Cur)

    // Cap on emitted diagnostics; further errors are suppressed with one
    // "too many errors" notice so a broken file can't drown the terminal.
    static constexpr int kMaxErrors = 20;
    int errorCount_ = 0;

    // Which token kinds are builtin types. Expanded from ../ast/builtin_types.def
    // (the same source the lexer, sema, and codegen use); this switch,
    // tokToTypeName, and parsePrimary's constructor case all derive from it, so
    // adding a type is one row in that .def.
    static bool isTypeTok(TokenKind tok) {
        switch (tok) {
#define BTYPE(Tok, _Spelling) case TokenKind::Tok:
#include "../ast/builtin_types.def"
                return true;
            default: return false;
        }
    }

    // get precedence of binary operators (higher binds tighter).
    // Bitwise levels follow C ordering: | < ^ < & < equality, and the
    // shifts sit between additive and relational.
    static int precedence(TokenKind tok) {
        switch (tok) {
            case TokenKind::Or:
                return 5;
            case TokenKind::And:
                return 6;
            case TokenKind::BitwiseOr:
                return 7;
            case TokenKind::BitwiseXor:
                return 8;
            case TokenKind::BitwiseAnd:
                return 9;
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
            case TokenKind::Equal:
            case TokenKind::NotEqual:
                return 10;
            case TokenKind::ShiftLeft:
            case TokenKind::ShiftRight:
                return 15;
            case TokenKind::Plus:
            case TokenKind::Minus:
                return 20;
            case TokenKind::Multiply:
            case TokenKind::Divide:
            case TokenKind::Percent:
                return 40;
            default:
                return -1;
        }
    }

    // Map a type-keyword token to its spelling (from builtin_types.def). Void is
    // the one type keyword not in the .def (return-only, not a value type), so it
    // gets its own case.
    static std::string_view tokToTypeName(TokenKind tok) {
        switch (tok) {
#define BTYPE(Tok, Spelling) case TokenKind::Tok: return Spelling;
#include "../ast/builtin_types.def"
            case TokenKind::Void: return "void";
            default: return "";
        }
    }

    // Human-readable name for a token, for diagnostics.
    static std::string getTokName(TokenKind tok) {
        return tokenKindName(tok);
    }

    // ── Type validation moved to SemanticAnalyzer ─────────────────────────
    // The parser no longer owns `isUserTypeName` / struct-cycle detection.
    // It accepts any `Identifier` where a type is expected; SemanticAnalyzer
    // walks the AST after parsing and reports unknown types / recursive
    // struct definitions against the *complete* program.

    // parse index expression inside brackets: '[' expression ']' - helper function
    ExprAST* parseBracketIndex() {
        if (peek() != TokenKind::Lbracket) {
            error("Expected '[' for index expression");
            return nullptr;
        }
        next();

        auto idx = parseExpression();
        if (!idx) return nullptr;

        if (peek() != TokenKind::Rbracket) {
            error("Expected ']' after index expression");
            return nullptr;
        }

        next();
        return idx;
    }

    // Parse a positive integer literal in `[1, 65536]`. The parser is
    // positioned at the size token; consumes it and returns the value, or
    // -1 with an error already emitted. Shared by parseVarDecl and
    // parseUniform's array suffixes.
    int parseArraySize() {
        if (peek() != TokenKind::Number) {
            error("Expected array size");
            return -1;
        }
        if (Cur.num < 1.0 || Cur.num > 65536.0) {
            error(fmt::format("Array size must be between 1 and 65536, got {}",
                              (long long)Cur.num));
            return -1;
        }
        int sz = static_cast<int>(Cur.num);
        next();
        return sz;
    }
};

// Parse program.
//
// Contextual-keyword convention (intentional split):
//   * Annotation-style attributes — `@entry`, `@stage`, `@compute`,
//     `@vertex`, `@fragment` — are real lexer tokens because they form a
//     closed, parser-relevant set.
//   * Descriptor-set qualifiers — `layout`, `binding`, `std430`,
//     `workgroup_size` — are matched as identifier *text*. The set is
//     open-ended (any future qualifier slots in without a lexer change)
//     and ignoring an unknown qualifier is benign.
// Don't unify the two without first auditing every `Cur.text == "..."`
// site below.
std::vector<ExprAST*> Parser::ParseProgram() {
    std::vector<ExprAST*> program;
    std::unordered_map<ShaderStage, std::string> entryByStage;
    while (peek() != TokenKind::Eof) {
        // Swallow stray `}` left over from synchronize() inside a broken
        // function body — without this, the recovery cascade emits a
        // spurious "Unknown token '}'" between every real diagnostic.
        if (peek() == TokenKind::Rbrace) { next(); continue; }

        ExprAST* stmt = nullptr;
        // Snapshot the source position of this top-level construct's first
        // token; stamped onto whatever node the dispatch below produces.
        ScopedLoc loc(*this);

        if (peek() == TokenKind::At) {
            auto attrsOpt = parseFunctionAttrs();
            if (!attrsOpt) {
                next(); // skip invalid attributes
                continue;
            }
            if (peek() != TokenKind::Fn) {
                error("Attributes are only allowed before a function ('fn')");
                while (peek() != TokenKind::Fn && peek() != TokenKind::Eof) next();
                continue;
            }
            auto* fn = parseFunction();
            if (auto *F = llvm::dyn_cast_or_null<FunctionAST>(fn)) {
                F->Attrs = *attrsOpt;
            } else {
                error("Internal error: expected FunctionAST after parsing function");
            }
            stmt = fn;
            if (auto *F = llvm::dyn_cast_or_null<FunctionAST>(stmt)) {
                if (F->Attrs.isEntry) {
                    ShaderStage st = *F->Attrs.stage;
                    if (entryByStage.count(st)) {
                        error(fmt::format(
                            "Multiple entry points for stage. Already: {}, new: {}",
                            entryByStage[st], F->Proto->Name));
                    } else {
                        entryByStage[st] = F->Proto->Name;
                    }
                }
            }
        } else if (peek() == TokenKind::Fn) {
            stmt = parseFunction();
        } else if (peek() == TokenKind::In) {
            stmt = parseStageVarDecl(true);
        } else if (peek() == TokenKind::Out) {
            stmt = parseStageVarDecl(false);
        } else if (peek() == TokenKind::Identifier && Cur.text == "layout") {
            // layout(binding=N, ...) uniform/in/out ...
            int binding = -1;
            next(); // 'layout'
            if (peek() == TokenKind::Lparen) {
                next(); // '('
                while (peek() != TokenKind::Rparen && peek() != TokenKind::Eof) {
                    if (peek() == TokenKind::Identifier && Cur.text == "binding") {
                        next();
                        if (peek() == TokenKind::Assign) { next(); if (peek() == TokenKind::Number) { binding = (int)Cur.num; next(); } }
                    } else if (peek() == TokenKind::Identifier) {
                        next(); // skip unknown key
                        if (peek() == TokenKind::Assign) { next(); if (peek() == TokenKind::Number) next(); }
                    } else if (peek() == TokenKind::Comma) { next(); }
                    else { next(); }
                }
                if (peek() == TokenKind::Rparen) next();
            }
            if (peek() == TokenKind::Uniform)      stmt = parseUniform(binding);
            else if (peek() == TokenKind::In)      stmt = parseStageVarDecl(true, binding);
            else if (peek() == TokenKind::Out)     stmt = parseStageVarDecl(false, binding);
            else if (peek() == TokenKind::Readonly || peek() == TokenKind::Writeonly || peek() == TokenKind::Buffer)
                                                   stmt = parseStorageBuffer(binding);
            else {
                error("Expected 'uniform', 'in', 'out', or 'buffer' after layout(...)");
                while (peek() != TokenKind::Semicolon && peek() != TokenKind::Eof) next();
                if (peek() == TokenKind::Semicolon) next();
            }
        } else if (peek() == TokenKind::Uniform) {
            stmt = parseUniform();
        } else if (peek() == TokenKind::Struct) {
            stmt = parseStruct();
        } else {
            stmt = parseStatement();
        }
        if (stmt) {
            program.push_back(loc.stamp(std::move(stmt)));
        } else {
            // Failed statement: report and skip ahead to a stable boundary
            // (`;`, `}`, top-level `fn`/`struct`/`@`). Without this, a single
            // parse error cascades into dozens of follow-on diagnostics.
            if (errorCount_ == 0) {
                error(fmt::format("Unexpected token at top level: {}",
                                  getTokName(peek())));
            }
            synchronize();
            if (errorCount_ >= kMaxErrors) break;
        }
    }
    return program;
}

// Struct declaration
ExprAST* Parser::parseStruct() {
    next(); // 'struct'
    if (peek() != TokenKind::Identifier) {
        error("Expected struct name after 'struct'");
        return nullptr;
    }
    std::string structName(Cur.text);
    next(); // struct name

    if (peek() != TokenKind::Lbrace) {
        error("Expected '{' after struct name");
        return nullptr;
    }
    next(); // '{'
    std::vector<std::pair<std::string, std::string> > fields;
    std::unordered_set<std::string> seenFieldNames;
    while (peek() != TokenKind::Rbrace) {
        if (peek() != TokenKind::Identifier && !isTypeTok(peek())) {
            error("Expected field type in struct");
            return nullptr;
        }
        std::string fieldType;
        if (isTypeTok(peek())) {
            fieldType = std::string(tokToTypeName(peek()));
        } else {
            // Identifier — assumed to be a user struct name; SemanticAnalyzer
            // will report it if no such struct exists in the program.
            fieldType = std::string(Cur.text);
        }
        next(); // type
        if (peek() != TokenKind::Identifier) {
            error("Expected field name in struct");
            return nullptr;
        }
        std::string fieldName(Cur.text);
        if (!seenFieldNames.insert(fieldName).second) {
            error(fmt::format("Duplicate field name '{}' in struct", fieldName));
            return nullptr;
        }
        next(); // field name
        if (peek() != TokenKind::Semicolon) {
            error("Expected ';' after struct field");
            return nullptr;
        }
        next(); // ';'
        fields.emplace_back(fieldType, fieldName);
    }
    if (peek() != TokenKind::Rbrace) {
        error("Expected '}' after struct body");
        return nullptr;
    }
    next(); // '}'
    if (peek() != TokenKind::Semicolon) {
        error("Expected ';' after struct declaration");
        return nullptr;
    }
    next(); // ';'

    // Struct registration + recursive-definition checking happen in the
    // semantic pass, against the whole program — so forward references
    // and out-of-order declarations both Just Work.
    return Ctx.create<StructDeclExprAST>(structName, std::move(fields));
}

// Uniform declaration
ExprAST* Parser::parseUniform(int /*binding*/) {
    next(); // 'uniform'
    std::string type;

    // built-in type
    if (isTypeTok(peek())) {
        type = std::string(tokToTypeName(peek()));
        next(); // type
    }
    // Identifier — assumed to be a user struct name; SemanticAnalyzer validates.
    else if (peek() == TokenKind::Identifier) {
        type = std::string(Cur.text);
        next(); // type
    } else {
        error("Expected type after 'uniform'");
        return nullptr;
    }

    if (peek() != TokenKind::Identifier) {
        error("Expected uniform name");
        return nullptr;
    }
    std::string name(Cur.text);
    next(); // name

    // Check for array syntax
    bool isArray = false;
    int arraySize = 0;

    if (peek() == TokenKind::Lbracket) {
        next(); // '['
        isArray = true;

        arraySize = parseArraySize();
        if (arraySize < 0) return nullptr;

        if (peek() != TokenKind::Rbracket) {
            error("Expected ']' after array size");
            return nullptr;
        }
        next(); // ']'
    }

    if (peek() != TokenKind::Semicolon) {
        error("Expected ';' after uniform declaration");
        return nullptr;
    }
    next(); // ';'

    if (isArray) {
        return Ctx.create<UniformArrayDeclExprAST>(type, name, arraySize);
    }
    return Ctx.create<UniformDeclExprAST>(type, name);
}

// Storage buffer declaration (compute shaders)
// Syntax: layout(std430, binding=N) readonly buffer ElemType name[];
ExprAST* Parser::parseStorageBuffer(int binding) {
    bool isReadOnly = true;
    if (peek() == TokenKind::Readonly) {
        isReadOnly = true;
        next(); // 'readonly'
    } else if (peek() == TokenKind::Writeonly) {
        isReadOnly = false;
        next(); // 'writeonly'
    }

    if (peek() != TokenKind::Buffer) {
        error("Expected 'buffer' in storage buffer declaration");
        return nullptr;
    }
    next(); // 'buffer'

    // Element type
    std::string elemType;
    if (isTypeTok(peek())) {
        elemType = std::string(tokToTypeName(peek()));
        next();
    } else if (peek() == TokenKind::Identifier) {
        elemType = std::string(Cur.text);
        next();
    } else {
        error("Expected element type in buffer declaration");
        return nullptr;
    }

    // Variable name
    if (peek() != TokenKind::Identifier) {
        error("Expected buffer variable name");
        return nullptr;
    }
    std::string name(Cur.text);
    next(); // name

    // Consume unsized [] array suffix
    if (peek() == TokenKind::Lbracket) {
        next(); // '['
        if (peek() != TokenKind::Rbracket) {
            error("Expected ']' in storage buffer array suffix");
            return nullptr;
        }
        next(); // ']'
    }

    // Strict `;` — mirrors parseUniform; panic-mode recovery handles
    // missing terminators uniformly via synchronize().
    if (peek() != TokenKind::Semicolon) {
        error("Expected ';' after storage buffer declaration");
        return nullptr;
    }
    next();

    return Ctx.create<StorageBufferDeclAST>(elemType, name, isReadOnly, binding);
}


// Function definition
ExprAST* Parser::parseFunction() {
    if (peek() != TokenKind::Fn) {
        error("Expected 'fn' at function start");
        return nullptr;
    }
    next(); // 'fn'
    std::string ret;
    if (isTypeTok(peek()) || peek() == TokenKind::Void) {
        ret = std::string(tokToTypeName(peek()));
        next(); // built-in/void
    } else if (peek() == TokenKind::Identifier) {
        // e.g. Hit, Ray, Light...  Validated post-parse by SemanticAnalyzer.
        ret = std::string(Cur.text);
        next();
    } else {
        error("Function must start with return type");
        return nullptr;
    }
    if (peek() != TokenKind::Identifier) {
        error("Expected function name");
        return nullptr;
    }
    std::string name(Cur.text);
    next(); // function name
    if (peek() != TokenKind::Lparen) {
        error("Expected '(' after function name");
        return nullptr;
    }
    next(); // '('
    std::vector<std::pair<std::string, std::string>> params;
    if (peek() != TokenKind::Rparen) {
        while (true) {
            std::string pty;

            // === Optional qualifier: in, out, inout ===
            if (peek() == TokenKind::In || peek() == TokenKind::Out || peek() == TokenKind::Inout)
                next(); // consume (semantics reserved for future inout pointer passing)

            // === PARAM TYPE: built-in or user-defined struct ===
            if (isTypeTok(peek())) {
                pty = std::string(tokToTypeName(peek()));
                next();          // e.g. vec3
            } else if (peek() == TokenKind::Identifier) {
                // User struct name (e.g. Hit, Light). Sema validates.
                pty = std::string(Cur.text);
                next();
            } else {
                error("Expected param type");
                return nullptr;
            }

            if (peek() != TokenKind::Identifier) {
                error("Expected param name");
                return nullptr;
            }
            std::string pname(Cur.text);
            next(); // param name

            params.emplace_back(pty, pname);

            if (peek() == TokenKind::Rparen) break;
            if (peek() != TokenKind::Comma) {
                error("Expected ',' in parameter list");
                return nullptr;
            }
            next(); // ','
        }
    }
    next(); // ')'
    if (peek() != TokenKind::Lbrace) {
        error("Expected '{' to start function body");
        return nullptr;
    }
    auto body = parseBlockExpr();
    if (!body) return nullptr;

    auto proto = Ctx.create<PrototypeAST>(name, std::move(params), ret);
    return Ctx.create<FunctionAST>(std::move(proto), std::move(body));
}

std::optional<ShaderStage> Parser::stageFromTok(TokenKind tok) {
    switch (tok) {
        case TokenKind::Vertex:   return ShaderStage::Vertex;
        case TokenKind::Fragment: return ShaderStage::Fragment;
        case TokenKind::Compute:  return ShaderStage::Compute;
        default: return std::nullopt;
    }
}

std::optional<FunctionAttrs> Parser::parseFunctionAttrs() {
    FunctionAttrs attrs;

    while (peek() == TokenKind::At) {
        next(); // '@'

        if (peek() == TokenKind::Entry) {
            next(); // 'entry'
            if (attrs.isEntry) {
                error("Duplicate @entry");
                return std::nullopt;
            }
            attrs.isEntry = true;
            continue;
        }

        if (peek() == TokenKind::Stage) {
            next(); // 'stage'

            if (peek() != TokenKind::Lparen) {
                error("Expected '(' after @stage");
                return std::nullopt;
            }
            next(); // '('

            auto st = stageFromTok(peek());
            if (!st.has_value()) {
                error("Expected stage name: vertex|fragment|compute");
                return std::nullopt;
            }
            if (attrs.stage.has_value()) {
                error("Duplicate @stage");
                return std::nullopt;
            }
            attrs.stage = *st;
            next(); // consume vertex/fragment/compute

            if (peek() != TokenKind::Rparen) {
                error("Expected ')' after @stage(...)");
                return std::nullopt;
            }
            next(); // ')'
            continue;
        }

        if (peek() == TokenKind::Identifier && Cur.text == "workgroup_size") {
            next(); // 'workgroup_size'
            if (peek() != TokenKind::Lparen) { error("Expected '(' after @workgroup_size"); return std::nullopt; }
            next(); // '('
            // Range-check each dimension before narrowing — Cur.num is a
            // double, so a silent `(uint32_t)Cur.num` would wrap on values
            // outside [0, 2^32). Match the array-size pattern; a workgroup
            // dimension of 0 is also nonsensical so the lower bound is 1.
            // Callers always gate on peek() == Number, so the lambda can
            // assume Cur is a Number.
            auto parseDim = [&](uint32_t& out, const char* role) -> bool {
                if (Cur.num < 1.0 || Cur.num > 4294967295.0) {
                    error(fmt::format("@workgroup_size {} must be in [1, 2^32), got {}",
                                      role, (long long)Cur.num));
                    return false;
                }
                out = static_cast<uint32_t>(Cur.num);
                next();
                return true;
            };
            uint32_t x = 1, y = 1, z = 1;
            if (peek() == TokenKind::Number && !parseDim(x, "x")) return std::nullopt;
            if (peek() == TokenKind::Comma)  { next(); if (peek() == TokenKind::Number && !parseDim(y, "y")) return std::nullopt; }
            if (peek() == TokenKind::Comma)  { next(); if (peek() == TokenKind::Number && !parseDim(z, "z")) return std::nullopt; }
            if (peek() != TokenKind::Rparen) { error("Expected ')' after @workgroup_size(...)"); return std::nullopt; }
            next(); // ')'
            attrs.workgroupSize = std::array<uint32_t,3>{x, y, z};
            continue;
        }

        error(fmt::format("Unknown attribute after '@': {}", getTokName(peek())));
        return std::nullopt;
    }

    if (attrs.isEntry && !attrs.stage.has_value()) {
        error("@entry requires @stage(...)");
        return std::nullopt;
    }

    return attrs;
}

// Parse statement
ExprAST* Parser::parseStatement() {
    // parseStatement is a pure dispatcher: the parser is positioned at the
    // statement's first token on entry, so a single ScopedLoc here +
    // loc.stamp() on every return stamps every statement node — the
    // sub-parsers need no stamping of their own.
    ScopedLoc loc(*this);
    switch (peek()) {
        case TokenKind::Struct:    return loc.stamp(parseStruct());
        case TokenKind::Uniform:   return loc.stamp(parseUniform());
        case TokenKind::Return:    return loc.stamp(parseReturn());
        case TokenKind::While:     return loc.stamp(parseWhile());
        case TokenKind::For:       return loc.stamp(parseFor());
        case TokenKind::Break:     return loc.stamp(parseBreak());
        case TokenKind::Continue:  return loc.stamp(parseContinue());
        case TokenKind::Discard:   return loc.stamp(parseDiscard());
        case TokenKind::If:        return loc.stamp(parseIfExpr());
        case TokenKind::Lbrace:    return loc.stamp(parseBlockExpr());
        case TokenKind::Const:
            // Don't consume here — parseVarDecl skips an optional `const`
            // so its own ScopedLoc captures at the `const` token, keeping
            // the top node *and* synthetic children at the same position.
            return loc.stamp(parseVarDecl());
        case TokenKind::Increment:
        case TokenKind::Decrement: {
            // prefix ++/-- as a standalone statement (e.g. "++x;")
            auto expr = parseExpression();
            if (!expr) return nullptr;
            if (peek() == TokenKind::Semicolon) next();
            return loc.stamp(std::move(expr));
        }
        case TokenKind::Identifier:
            // `Foo bar` (two identifiers) is always a declaration; anything
            // else (`a = …`, `a.b`, `a()`, `a[i]`, `a + b`, `a++;`, …) is
            // an assignment or expression statement. The one-token
            // lookahead picks the path; SemanticAnalyzer validates that
            // `Foo` actually names a struct.
            if (peekNext() == TokenKind::Identifier) {
                return loc.stamp(parseVarDecl());
            }
            return loc.stamp(parseAssignmentOrExprStatement());
        default:
            if (isTypeTok(peek())) {
                return loc.stamp(parseVarDecl());
            }
            error(fmt::format("Unknown token at start of statement {}", getTokName(peek())));
            return nullptr;
    }
}

// Block expression
ExprAST* Parser::parseBlockExpr() {
    ScopedLoc loc(*this);  // the '{'
    next(); // '{'
    std::vector<ExprAST*> statements;
    while (peek() != TokenKind::Rbrace && peek() != TokenKind::Eof) {
        auto stmt = parseStatement();
        if (!stmt) return nullptr;
        statements.push_back(std::move(stmt));
    }
    if (peek() != TokenKind::Rbrace) {
        error("Expected '}' at end of block");
        return nullptr;
    }
    next(); // '}'
    return loc.stamp(Ctx.create<BlockExprAST>(std::move(statements)));
}

// parse if expression
// example: if (cond) { ... } else { ... }
ExprAST* Parser::parseIfExpr() {
    next(); // 'if'
    if (peek() != TokenKind::Lparen) {
        error("Expected '(' after 'if'");
        return nullptr;
    }
    next(); // '('
    auto cond = parseExpression();
    if (!cond) return nullptr;
    if (peek() != TokenKind::Rparen) {
        error("Expected ')' in if condition");
        return nullptr;
    }
    next(); // ')'
    // then block or statement
    ExprAST* thenExpr = nullptr;
    if (peek() == TokenKind::Lbrace) {
        thenExpr = parseBlockExpr();
    } else {
        thenExpr = parseStatement();
    }
    if (!thenExpr) return nullptr;
    ExprAST* elseExpr = nullptr;
    if (peek() == TokenKind::Else) {
        next(); // 'else'
        if (peek() == TokenKind::Lbrace) {
            elseExpr = parseBlockExpr();
        } else {
            elseExpr = parseStatement();
        }
        if (!elseExpr) return nullptr;
    }
    return Ctx.create<IfExprAST>(std::move(cond), std::move(thenExpr), std::move(elseExpr));
}

// parse while loop
// example: while (cond) { ... }
ExprAST* Parser::parseWhile() {
    next(); // 'while'
    if (peek() != TokenKind::Lparen) {
        error("Expected '(' after 'while'");
        return nullptr;
    }
    next(); // '('
    auto cond = parseExpression();
    if (!cond) return nullptr;
    if (peek() != TokenKind::Rparen) {
        error("Expected ')' in while condition");
        return nullptr;
    }
    next(); // ')'
    ExprAST* body = nullptr;
    if (peek() == TokenKind::Lbrace) {
        body = parseBlockExpr();
    } else {
        body = parseStatement();
    }
    if (!body) return nullptr;
    return Ctx.create<WhileExprAST>(std::move(cond), std::move(body));
}

// For loop
ExprAST* Parser::parseFor() {
    next(); // 'for'
    if (peek() != TokenKind::Lparen) {
        error("Expected '(' after 'for'");
        return nullptr;
    }
    next(); // '('
    // Initialization
    ExprAST* init = nullptr;
    if (peek() != TokenKind::Semicolon) {
        init = parseStatement();
    } else {
        next(); // ';'
    }
    // Condition
    ExprAST* cond = nullptr;
    if (peek() != TokenKind::Semicolon) {
        cond = parseExpression();
        if (!cond) return nullptr;
    }
    if (peek() != TokenKind::Semicolon) {
        error("Expected ';' after for condition");
        return nullptr;
    }
    next(); // ';'
    // Increment. The for-header terminator is `)`, not `;`, so we ask
    // parseAssignmentOrExprStatement not to consume a `;`. It already
    // handles `i = i + 1`, `i += 1`, `i++`, `i--`, function calls, and
    // bare expressions — no duplicate dispatch needed here.
    ExprAST* inc = nullptr;
    if (peek() != TokenKind::Rparen) {
        if (peek() == TokenKind::Identifier) {
            inc = parseAssignmentOrExprStatement(/*consumeSemi=*/false);
        } else {
            inc = parseExpression();
        }
        if (!inc) return nullptr;
    }
    if (peek() != TokenKind::Rparen) {
        error("Expected ')' after for increment");
        return nullptr;
    }
    next(); // ')'
    ExprAST* body = nullptr;
    if (peek() == TokenKind::Lbrace) {
        body = parseBlockExpr();
    } else {
        body = parseStatement();
    }
    if (!body) return nullptr;
    return Ctx.create<ForExprAST>(std::move(init), std::move(cond), std::move(inc), std::move(body));
}

// parse return statement
// example: return expr;
ExprAST* Parser::parseReturn() {
    next(); // 'return'
    // for void return
    if (peek() == TokenKind::Semicolon) {
        next(); // ';'
        return Ctx.create<ReturnStmtAST>(nullptr);
    }
    auto expr_ast = parseExpression();
    if (!expr_ast) return nullptr;
    if (peek() != TokenKind::Semicolon) {
        error("Expected ';' after return expression");
        return nullptr;
    }
    next(); // ';'
    return Ctx.create<ReturnStmtAST>(std::move(expr_ast));
}

// Break statement
// Stage variable declaration: in/out type name;
ExprAST* Parser::parseStageVarDecl(bool isInput, int binding) {
    next(); // consume 'in' or 'out'
    std::string type;
    if (isTypeTok(peek())) {
        type = std::string(tokToTypeName(peek()));
        next();
    } else if (peek() == TokenKind::Identifier) {
        // User struct name. Validated post-parse.
        type = std::string(Cur.text);
        next();
    } else {
        error("Expected type in in/out declaration");
        return nullptr;
    }
    if (peek() != TokenKind::Identifier) { error("Expected name in in/out declaration"); return nullptr; }
    std::string name(Cur.text);
    next();
    if (peek() != TokenKind::Semicolon) { error("Expected ';' after in/out declaration"); return nullptr; }
    next();
    return Ctx.create<StageVarDeclAST>(isInput, type, name, binding);
}

// Continue statement
ExprAST* Parser::parseContinue() {
    next(); // 'continue'
    if (peek() != TokenKind::Semicolon) { error("Expected ';' after continue"); return nullptr; }
    next();
    return Ctx.create<ContinueStmtAST>();
}

// Discard statement
ExprAST* Parser::parseDiscard() {
    next(); // 'discard'
    if (peek() != TokenKind::Semicolon) { error("Expected ';' after discard"); return nullptr; }
    next();
    return Ctx.create<DiscardStmtAST>();
}

ExprAST* Parser::parseBreak() {
    if (peek() != TokenKind::Break) {
        error("Expected 'break' keyword");
        return nullptr;
    }
    next(); // 'break'
    if (peek() != TokenKind::Semicolon) {
        error("Expected ';' after break");
        return nullptr;
    }
    next(); // ';'
    return Ctx.create<BreakStmtAST>();
}

// Variable declaration
ExprAST* Parser::parseVarDecl() {
    // Position of the declaration's first token — used to stamp the synthetic
    // default-value / array-initializer nodes built below. The returned
    // top-level node is stamped by parseStatement's dispatcher. Capture
    // BEFORE consuming an optional `const` so all nodes built here share
    // the same source position.
    ScopedLoc loc(*this);

    // Optional `const` qualifier. Currently parsed but not stored on the
    // AST (codegen treats every declaration uniformly); kept for forward
    // compat with a future const-correctness pass.
    if (peek() == TokenKind::Const) next();

    // Parse type (builtin or user-defined struct)
    std::string type;
    if (isTypeTok(peek())) {
        type = std::string(tokToTypeName(peek()));
        next();
    } else if (peek() == TokenKind::Identifier) {
        // User struct name. Validated post-parse.
        type = std::string(Cur.text);
        next();
    } else {
        error("Expected type at start of variable declaration");
        return nullptr;
    }

    // Variable name
    if (peek() != TokenKind::Identifier) {
        error("Expected variable name after type");
        return nullptr;
    }
    std::string name(Cur.text);
    next(); // consume name

    // Optional local array: [size]
    bool isArray   = false;
    int  arraySize = 0;

    if (peek() == TokenKind::Lbracket) {
        next(); // '['

        arraySize = parseArraySize();
        if (arraySize < 0) return nullptr;

        if (peek() != TokenKind::Rbracket) {
            error("Expected ']' in array declaration");
            return nullptr;
        }
        next(); // ']'

        isArray = true;
    }

    // No initializer: "type name;" or "type name[size];"
    if (peek() == TokenKind::Semicolon) {
        next(); // ';'

        if (isArray) {
            // local array without initialization
            return Ctx.create<ArrayDeclExprAST>(type, name, arraySize, nullptr);
        }

        // scalars/vectors/matrices get default value
        ExprAST* defaultValue = nullptr;

        if (type == "bool") {
            defaultValue = loc.stamp(Ctx.create<BooleanExprAST>(false));
        } else if (type == "int" || type == "uint") {
            defaultValue = loc.stamp(Ctx.create<NumberExprAST>(0.0));
        } else if (type == "float" || type == "double") {
            defaultValue = loc.stamp(Ctx.create<NumberExprAST>(0.0));
        } else if (type == "vec2" || type == "vec3" || type == "vec4") {
            std::vector<ExprAST*> args;
            args.push_back(loc.stamp(Ctx.create<NumberExprAST>(0.0)));
            defaultValue = loc.stamp(Ctx.create<CallExprAST>(type, std::move(args)));
        } else if (type == "mat2"   || type == "mat3"   || type == "mat4"   ||
                   type == "mat2x3" || type == "mat2x4" ||
                   type == "mat3x2" || type == "mat3x4" ||
                   type == "mat4x2" || type == "mat4x3") {
            // tokToTypeName never produces "matNxN" — Mat2 → "mat2", not
            // "mat2x2" — so listing the square forms here would be dead.
            // empty constructor for matrices
            std::vector<ExprAST*> args;
            defaultValue = loc.stamp(Ctx.create<CallExprAST>(type, std::move(args)));
        } else {
            // Assumed user-defined struct: emit empty constructor call.
            // SemanticAnalyzer reports if the type name does not resolve.
            std::vector<ExprAST*> args;
            defaultValue = loc.stamp(Ctx.create<CallExprAST>(type, std::move(args)));
        }

        return loc.stamp(Ctx.create<AssignmentExprAST>(type, name, std::move(defaultValue)));
    }

    // With initialization: '='
    if (peek() == TokenKind::Assign) {
        next(); // '='

        // array with initializer: type name[size] = { ... };
        if (isArray) {
            if (peek() != TokenKind::Lbrace) {
                error("Expected '{' for array initializer");
                return nullptr;
            }
            next(); // '{'

            std::vector<ExprAST*> elements;
            if (peek() != TokenKind::Rbrace) {
                while (true) {
                    auto elem = parseExpression();
                    if (!elem) return nullptr;
                    elements.push_back(std::move(elem));

                    if (peek() == TokenKind::Rbrace) break;
                    if (peek() != TokenKind::Comma) {
                        error("Expected ',' or '}' in array initializer");
                        return nullptr;
                    }
                    next(); // ','
                }
            }

            if (peek() != TokenKind::Rbrace) {
                error("Expected '}' at end of array initializer");
                return nullptr;
            }
            next(); // '}'

            if (static_cast<int>(elements.size()) != arraySize) {
                error("Array initializer size mismatch");
                return nullptr;
            }

            if (peek() != TokenKind::Semicolon) {
                error("Expected ';' after array declaration");
                return nullptr;
            }
            next(); // ';'

            auto init = loc.stamp(Ctx.create<ArrayInitExprAST>(std::move(elements)));
            return loc.stamp(Ctx.create<ArrayDeclExprAST>(type, name, arraySize, std::move(init)));
        }

        // 5b) regular variable: type name = expr;
        auto value = parseExpression();
        if (!value) return nullptr;

        if (peek() != TokenKind::Semicolon) {
            error("Expected ';' at end of declaration");
            return nullptr;
        }
        next(); // ';'

        return Ctx.create<AssignmentExprAST>(type, name, std::move(value));
    }

    error("Expected '=' or ';' after variable name in declaration");
    return nullptr;
}

ExprAST* Parser::parseAssignmentOrExprStatement(bool consumeSemi) {
    if (peek() != TokenKind::Identifier) {
        error("Expected identifier at start of statement");
        return nullptr;
    }
    // parse left-hand side using primary expression
    auto lhs = parsePrimary();
    if (!lhs) return nullptr;
    // Compound assignment: a += rhs, a -= rhs, a *= rhs, a /= rhs, a %= rhs
    if (peek() == TokenKind::PlusAssign || peek() == TokenKind::MinusAssign ||
        peek() == TokenKind::MulAssign  || peek() == TokenKind::DivAssign  ||
        peek() == TokenKind::ModAssign) {
        TokenKind op = peek();
        next();
        auto rhs = parseExpression();
        if (!rhs) return nullptr;
        if (consumeSemi) {
            if (peek() != TokenKind::Semicolon) { error("Expected ';' after compound assignment"); return nullptr; }
            next();
        }
        TokenKind binOp;
        switch (op) {
            case TokenKind::PlusAssign:  binOp = TokenKind::Plus;     break;
            case TokenKind::MinusAssign: binOp = TokenKind::Minus;    break;
            case TokenKind::MulAssign:   binOp = TokenKind::Multiply; break;
            case TokenKind::DivAssign:   binOp = TokenKind::Divide;   break;
            default:                     binOp = TokenKind::Percent;  break;  // ModAssign
        }
        if (auto* var = llvm::dyn_cast<VariableExprAST>(lhs)) {
            // Desugar `a += b` → `a = a + b`; the synthetic nodes inherit the
            // lvalue's source position.
            SourceLocation l = lhs->loc;
            auto* lhsCopy = at(Ctx.create<VariableExprAST>(var->Name), l);
            auto* binExpr = at(Ctx.create<BinaryExprAST>(binOp, lhsCopy, rhs), l);
            return at(Ctx.create<AssignmentExprAST>("", var->Name, binExpr), l);
        }
        error("Compound assignment only supported for simple variables");
        return nullptr;
    }

    // assignment statement - example : a = 5;
    // ReSharper disable once CppDFAConstantConditions
    if (peek() == TokenKind::Assign) {
        next(); // '='
        auto rhs = parseExpression();
        if (!rhs) return nullptr;

        if (consumeSemi) {
            if (peek() != TokenKind::Semicolon) {
                error("Expected ';' after assignment");
                return nullptr;
            }
            next(); // ';'
        }

        // matrix assignment: a[i] = col_vec  or  a[i][j] = scalar.
        // Since lhs is a raw pointer into the arena now, llvm::dyn_cast
        // is enough — the rewrap-into-the-new-node is just pointer copies.
        if (auto* macc = llvm::dyn_cast<MatrixAccessExprAST>(lhs)) {
            return Ctx.create<MatrixAssignmentExprAST>(
                macc->Object, macc->Index, macc->Index2, rhs);
        }

        // member assignment: v.xyz = ..., obj.field = ...
        if (auto* mem = llvm::dyn_cast<MemberAccessExprAST>(lhs)) {
            return Ctx.create<MemberAssignmentExprAST>(
                mem->Object, mem->Member, rhs);
        }

        // simple variable: a = ...
        if (auto* var = llvm::dyn_cast<VariableExprAST>(lhs)) {
            return Ctx.create<AssignmentExprAST>("", var->Name, rhs);
        }

        error("Unsupported lvalue in assignment");
        return nullptr;
    }
    // expression statement - example : a + 1;
    auto* expr = parseBinOpRHS(0, lhs);
    if (!expr) return nullptr;
    if (consumeSemi) {
        if (peek() != TokenKind::Semicolon) {
            error("Expected ';' after expression");
            return nullptr;
        }
        next(); // ';'
    }
    return expr;
}

// parse expression, including optional ternary
ExprAST* Parser::parseExpression() {
    ScopedLoc loc(*this);
    auto lhs = parseUnary();
    if (!lhs) return nullptr;
    auto expr = parseBinOpRHS(0, std::move(lhs));
    if (!expr) return nullptr;

    // Ternary: expr ? thenExpr : elseExpr. Right-associative because the
    // `then` branch recurses through parseExpression — `a ? b : c ? d : e`
    // parses as `a ? b : (c ? d : e)`, matching C/GLSL.
    if (peek() == TokenKind::Question) {
        next(); // '?'
        auto thenExpr = parseExpression();
        if (!thenExpr) return nullptr;
        if (peek() != TokenKind::Colon) { error("Expected ':' in ternary operator"); return nullptr; }
        next(); // ':'
        auto elseExpr = parseExpression();
        if (!elseExpr) return nullptr;
        return loc.stamp(Ctx.create<TernaryExprAST>(
            std::move(expr), std::move(thenExpr), std::move(elseExpr)));
    }
    return expr;  // already stamped by parseUnary / parseBinOpRHS
}

// for unary operators
// example : -x, ++x, --x
ExprAST* Parser::parseUnary() {
    ScopedLoc loc(*this);
    // prefix ++ / --
    if (peek() == TokenKind::Increment || peek() == TokenKind::Decrement) {
        bool isInc = (peek() == TokenKind::Increment);
        next();
        auto operand = parseUnary();
        if (!operand) return nullptr;
        // Prefix ++/-- correctly returns the *new* value via the desugar
        // `x = x ± 1`. Postfix ++/-- gets its own dedicated AST node — see
        // parsePostfixAfterIdent — so it can return the *old* value.
        if (auto* var = llvm::dyn_cast<VariableExprAST>(operand)) {
            auto one     = loc.stamp(Ctx.create<NumberExprAST>(1.0));
            auto varCopy = loc.stamp(Ctx.create<VariableExprAST>(var->Name));
            TokenKind binOp = isInc ? TokenKind::Plus : TokenKind::Minus;
            auto binExpr = loc.stamp(Ctx.create<BinaryExprAST>(
                binOp, std::move(varCopy), std::move(one)));
            return loc.stamp(Ctx.create<AssignmentExprAST>(
                "", var->Name, std::move(binExpr)));
        }
        error("Prefix ++/-- only supported on simple variables");
        return nullptr;
    }

    if (peek() == TokenKind::Minus || peek() == TokenKind::Plus ||
        peek() == TokenKind::Not   || peek() == TokenKind::BitwiseNot) {
        TokenKind Op = peek();
        next();
        auto operand = parseUnary();
        if (!operand) return nullptr;
        return loc.stamp(Ctx.create<UnaryExprAST>(Op, std::move(operand)));
    }
    return parsePrimary();  // parsePrimary stamps its own result
}

// parse primary expressions
// example: identifier, number, (expression), constructor call, member access, swizzle,
ExprAST* Parser::parsePrimary() {
    ScopedLoc loc(*this);
    ExprAST* expr = nullptr;
    switch (peek()) {
        // Identifier and every builtin type keyword dispatch to the same path
        // (a type keyword here begins a constructor call like `vec3(...)`). The
        // case list expands builtin_types.def, in sync with isTypeTok.
        case TokenKind::Identifier:
#define BTYPE(Tok, _Spelling) case TokenKind::Tok:
#include "../ast/builtin_types.def"
            expr = parseIdentifierOrCtorExpr();
            break;
        case TokenKind::Number:
            expr = parseNumberExpr();
            break;
        case TokenKind::Lparen: {
            next(); // '('
            expr = parseExpression();
            if (!expr) return nullptr;
            if (peek() != TokenKind::Rparen) {
                error("Expected ')'");
                return nullptr;
            }
            next(); // ')'
            break;
        }
        case TokenKind::True:
            next(); // 'true'
            expr = Ctx.create<BooleanExprAST>(true);
            break;
        case TokenKind::False:
            next(); // 'false'
            expr = Ctx.create<BooleanExprAST>(false);
            break;
        default:
            error("Unknown token when expecting an expression");
            return nullptr;
    }
    if (!expr) return nullptr;
    // Stamp the primary at its first token, then let parsePostfixAfterIdent
    // stamp any member-access / index nodes it layers on top.
    expr = loc.stamp(std::move(expr));
    expr = parsePostfixAfterIdent(std::move(expr));
    return expr;
}

// number literal
// example: 42, 3.14
ExprAST* Parser::parseNumberExpr() {
    auto result = Ctx.create<NumberExprAST>(Cur.num, Cur.isInt, Cur.isUnsigned);
    next();
    return result;
}

// identifier or constructor call
// example: myVar, vec3(1.0, 0.0, 0.0)
ExprAST* Parser::parseIdentifierOrCtorExpr() {
    std::string name;
    if (isTypeTok(peek())) {
        // Constructor type call e.g., vec3(1.0, 0.0, 0.0)
        name = std::string(tokToTypeName(peek()));
        next(); // type token
        if (peek() != TokenKind::Lparen) {
            error("Type name in expression must be followed by '(' (constructor)");
            return nullptr;
        }
    }
    // Function call, variable, or user-defined type constructor
    else if (peek() == TokenKind::Identifier) {
        name = std::string(Cur.text);
        next(); // identifier
        // simple variable if not followed by '('
        if (peek() != TokenKind::Lparen) {
            return Ctx.create<VariableExprAST>(name);
        }
    } else {
        error("Expected identifier or type constructor");
        return nullptr;
    }
    // function or constructor call
    next(); // '('
    std::vector<ExprAST*> args;
    if (peek() != TokenKind::Rparen) {
        while (true) {
            auto arg = parseExpression();
            if (!arg) return nullptr;
            args.push_back(std::move(arg));
            if (peek() == TokenKind::Rparen) break;
            if (peek() != TokenKind::Comma) {
                error("Expected ',' in argument list");
                return nullptr;
            }
            next(); // ','
        }
    }
    next(); // ')'
    // CallExprAST - function call or user-defined type constructor
    return Ctx.create<CallExprAST>(name, std::move(args));
}

ExprAST* Parser::parsePostfixAfterIdent(ExprAST* base) {
    while (true) {
        // member access or swizzle - someVec.x or someVec.xyz
        // Postfix nodes inherit the source position of their `base` operand
        // (e.g. `a.b` is located at `a`). Capture before the std::move.
        if (peek() == TokenKind::Dot) {
            next();
            if (peek() != TokenKind::Identifier) {
                error("Expected member/swizzle name after '.'");
                return nullptr;
            }
            // check if single component or swizzle
            std::string member(Cur.text);
            next();
            SourceLocation l = base->loc;
            base = at(Ctx.create<MemberAccessExprAST>(std::move(base), member), l);
            continue;
        }
        // matrix access - someMat[i] or someMat[i][j]
        if (peek() == TokenKind::Lbracket) {
            SourceLocation l = base->loc;
            auto idx1 = parseBracketIndex();
            if (!idx1) return nullptr;

            if (peek() == TokenKind::Lbracket) {
                auto idx2 = parseBracketIndex();
                if (!idx2) return nullptr;
                base = at(Ctx.create<MatrixAccessExprAST>(
                    std::move(base),
                    std::move(idx1),
                    std::move(idx2)
                ), l);
            } else {
                base = at(Ctx.create<MatrixAccessExprAST>(
                    std::move(base),
                    std::move(idx1)
                ), l);
            }
            continue;
        }
        // postfix ++ / -- — wraps in a dedicated PostfixIncrExprAST so
        // codegen can return the *old* value. Desugaring into `x = x ± 1`
        // (as prefix does) would silently give prefix semantics, breaking
        // `a[i++]`.
        if (peek() == TokenKind::Increment || peek() == TokenKind::Decrement) {
            bool isDec = (peek() == TokenKind::Decrement);
            next();
            if (!llvm::isa<VariableExprAST>(base)) {
                error("Postfix ++/-- only supported on simple variables");
                return nullptr;
            }
            SourceLocation l = base->loc;
            base = at(Ctx.create<PostfixIncrExprAST>(std::move(base), isDec),
                      l);
            continue;
        }
        break;
    }
    return base;
}

// binary operators - right-hand side
// exprPrecedence - minimal precedence for this level, lhs - already parsed left-hand side
ExprAST* Parser::parseBinOpRHS(int exprPrecedence, ExprAST* lhs) {
    while (true) {
        // get precedence of current
        int tokPrecedence = precedence(peek());
        // if this operator is lower precedence than what we are allowed, return lhs
        if (tokPrecedence < exprPrecedence) return lhs;
        // this is a binary operator
        TokenKind op = peek();
        next(); // operator
        // parse unary expression after the binary operator
        auto* rhs = parseUnary();
        if (!rhs) return nullptr;
        int nextPrecedence = precedence(peek());
        if (tokPrecedence < nextPrecedence) {
            rhs = parseBinOpRHS(tokPrecedence + 1, rhs);
            if (!rhs) return nullptr;
        }
        // The composite BinaryExprAST is located at its left operand's start.
        SourceLocation l = lhs->loc;
        lhs = at(Ctx.create<BinaryExprAST>(op, lhs, rhs), l);
    }
}

// Entry point
std::vector<ExprAST*> ParseProgram(ASTContext& ctx, std::string_view source) {
    Parser parser(ctx, source);
    return parser.ParseProgram();
}