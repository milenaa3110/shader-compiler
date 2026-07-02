// sema_test.cpp - Semantic Analyzer Type-System Unit Tests

#include "frontend/ast/ast.h"
#include "frontend/ast/ast_context.h"
#include "frontend/parser/parser.h"
#include "frontend/sema/sema.h"

#include <llvm/Support/Casting.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

static int failures = 0;

// Basic test assertion recorder.
static void check(bool cond, const char* msg) {
    if (!cond) {
        std::printf("  FAIL: %s\n", msg);
        ++failures;
    }
}

// Retrieves the last top-level function in the parsed AST program array.
static FunctionAST* lastFn(const std::vector<ExprAST*>& prog) {
    FunctionAST* fn = nullptr;
    for (auto* node : prog)
        if (auto* f = llvm::dyn_cast<FunctionAST>(node)) fn = f;
    return fn;
}

// Retrieves the i-th statement inside the body of the last function.
static ExprAST* fnStmt(const std::vector<ExprAST*>& prog, size_t i) {
    FunctionAST* fn = lastFn(prog);
    auto* body = fn ? llvm::dyn_cast_or_null<BlockExprAST>(fn->Body) : nullptr;
    if (!body || i >= body->Statements.size()) return nullptr;
    return body->Statements[i];
}

// Retrieves the initializer expression of an assignment/declaration statement.
static ExprAST* firstFnStmtInit(const std::vector<ExprAST*>& prog, size_t i) {
    auto* a = llvm::dyn_cast_or_null<AssignmentExprAST>(fnStmt(prog, i));
    return a ? a->Init : nullptr;
}

// Checks if the node is an ImplicitCastExprAST matching the requested type string.
static bool isCastTo(const ExprAST* e, const char* want) {
    auto* c = llvm::dyn_cast_or_null<ImplicitCastExprAST>(e);
    return c && c->getType() && c->getType()->toString() == want;
}

// Returns the string representation of the stamped type, or "<none>" if null.
static std::string typeOf(const ExprAST* e) {
    if (!e || !e->getType()) return "<none>";
    return e->getType()->toString();
}

// Parses and runs the semantic pass over the provided source buffer.
static std::vector<ExprAST*> analyze(ASTContext& ctx, std::string_view src) {
    auto prog = ParseProgram(ctx, src);
    SemanticAnalyzer sema(ctx);
    sema.run(prog);
    return prog;
}

// Returns the total number of diagnostic errors flagged by SemanticAnalyzer.
static int analyzeErrors(std::string_view src) {
    ASTContext ctx;
    auto prog = ParseProgram(ctx, src);
    SemanticAnalyzer sema(ctx);
    return sema.run(prog);
}

// Asserts that the stamped type of an initializer statement matches the expectation.
static void expectInitType(const char* name, std::string_view src, size_t stmt,
                           const char* want) {
    ASTContext ctx;
    auto prog = analyze(ctx, src);
    std::string got = typeOf(firstFnStmtInit(prog, stmt));
    if (got != want) {
        std::printf("  FAIL: %s — stmt %zu init type is '%s', expected '%s'\n",
                    name, stmt, got.c_str(), want);
        ++failures;
    }
}

int main() {
    std::printf("Running sema type-stamp tests...\n");

    //  GLSL §4.1 Literals ─
    {
        std::string src =
            "fn void f() {\n"
            "  float a = 1.0;\n"
            "  int b = 42;\n"
            "  uint c = 7u;\n"
            "  bool d = true;\n"
            "  float e = 2.5e3;\n"
            "}\n";
        expectInitType("float literal", src, 0, "float");
        expectInitType("int literal", src, 1, "int");
        expectInitType("uint literal (u suffix)", src, 2, "uint");
        expectInitType("bool literal", src, 3, "bool");
        expectInitType("scientific literal", src, 4, "float");
    }

    //  Symbol Table Tracking 
    expectInitType("param identifier", "fn void g(vec2 uv) { vec2 p = uv; }", 0, "vec2");

    //  Vector Swizzles 
    expectInitType("swizzle .x → float", "fn void h(vec3 v) { float x = v.x; }", 0, "float");
    expectInitType("swizzle .xy → vec2", "fn void h(vec3 v) { vec2 q = v.xy; }", 0, "vec2");

    //  Operator Type Propagation 
    expectInitType("float + float → float",
                   "fn void k(float a) { float b = a + 1.0; }", 0, "float");
    expectInitType("scalar * vector → vector",
                   "fn void k(vec3 v) { vec3 w = 2.0 * v; }", 0, "vec3");
    expectInitType("comparison → bool",
                   "fn void k(float a) { bool b = a < 1.0; }", 0, "bool");

    //  Constructors and Call Expressions 
    expectInitType("vec4(...) → vec4", "fn void m() { vec4 c = vec4(1.0); }", 0, "vec4");
    expectInitType("user fn call → return type",
                   "fn float scale(float t) { return t; }\n"
                   "fn void m() { float x = scale(2.0); }", 0, "float");

    //  Struct Field Resolution 
    expectInitType("struct field → field type",
                   "struct S { vec3 pos; };\n"
                   "fn void n(S s) { vec3 p = s.pos; }", 0, "vec3");

    //  Implicit Widening Cast Insertion ─
    {
        ASTContext ctx;
        std::string src =
            "fn void f() {\n"
            "  float x = 42;\n"   // int -> float: requires cast
            "  float y = 1.0;\n"  // float -> float: no cast
            "  int z = 5;\n"      // int -> int: no cast
            "}\n";
        auto prog = analyze(ctx, src);
        check(isCastTo(firstFnStmtInit(prog, 0), "float"),
              "int initializer widened to float via ImplicitCast");
        check(!llvm::isa<ImplicitCastExprAST>(firstFnStmtInit(prog, 1)),
              "float initializer needs no cast");
        check(!llvm::isa<ImplicitCastExprAST>(firstFnStmtInit(prog, 2)),
              "int initializer needs no cast");
    }

    // Binary Operand Promotion: float + int -> float + (float)int
    {
        ASTContext ctx;
        auto prog = analyze(ctx, "fn void f(float a) { float b = a + 2; }");
        auto* bin = llvm::dyn_cast_or_null<BinaryExprAST>(firstFnStmtInit(prog, 0));
        check(bin != nullptr, "initializer is a binary op");
        if (bin) {
            check(!llvm::isa<ImplicitCastExprAST>(bin->LHS), "float operand left as-is");
            check(isCastTo(bin->RHS, "float"), "int operand promoted to float");
        }
    }

    // Return Value Promotion
    {
        ASTContext ctx;
        auto prog = analyze(ctx, "fn float f(int n) { return n; }");
        auto* body = llvm::dyn_cast_or_null<BlockExprAST>(lastFn(prog)->Body);
        auto* ret = body ? llvm::dyn_cast_or_null<ReturnStmtAST>(body->Statements.at(0)) : nullptr;
        check(ret && isCastTo(ret->Expr, "float"), "returned int widened to float");
    }

    //  Narrowing Rejection and Widening Allowance ─
    check(analyzeErrors("fn void f() { int x = 1.5; }") > 0, "narrowing float->int in initialization is an error");
    check(analyzeErrors("fn int f() { return 2.5; }") > 0, "narrowing float->int in return is an error");
    check(analyzeErrors("fn void f() { float x = 1; }") == 0, "widening int->float in initialization is allowed");
    check(analyzeErrors("fn float f(int n) { return n; }") == 0, "widening int->float in return is allowed");

    //  Bitwise Shift Rules (GLSL §5.9: Type follows LHS) 
    expectInitType("int >> uint → int (LHS type, not promoted)",
                   "fn void f(int x) { int y = x >> 1u; }", 0, "int");
    expectInitType("uint << int → uint (LHS type)",
                   "fn void f(uint x) { uint y = x << 2; }", 0, "uint");

    //  Logical Unary Operator Invariants 
    check(analyzeErrors("fn void f() { int i = 3; bool b = !i; }") > 0, "logical ! on a non-bool int is an error");
    check(analyzeErrors("fn void f() { bool a = true; bool b = !a; }") == 0, "logical ! on a bool is allowed");

    //  Implicit Conversion Constraints Across AST Sites 
    check(analyzeErrors("fn void f() { int i = 0; i = 2.5; }") > 0, "narrowing float->int in re-assignment is an error");
    check(analyzeErrors("fn void f() { float x = 0.0; x = 1; }") == 0, "widening int->float in re-assignment is allowed");
    check(analyzeErrors("fn int g(int n) { return n; }\nfn void h() { int r = g(2.5); }") > 0, "narrowing float->int call argument is an error");
    check(analyzeErrors("fn float g(float x) { return x; }\nfn void h() { float r = g(2; }") == 0, "widening int->float call argument is allowed");
    check(analyzeErrors("fn void f() { bool b = true; float x = b ? b : 1.0; }") > 0, "ternary mixing bool and float arms is an error");
    check(analyzeErrors("fn void f(int n) { float x = n > 0 ? n : 1.0; }") == 0, "ternary with int/float arms widens (allowed)");

    //  Scalar-to-Vector Restrictions (Operators Only, No Assignment) 
    check(analyzeErrors("fn void f() { vec3 v = 1.0; }") > 0, "scalar->vector in initialization is an error");
    check(analyzeErrors("fn vec3 f() { return 1.0; }") > 0, "scalar->vector in return is an error");
    check(analyzeErrors("fn vec3 g(vec3 v) { return v; }\nfn void f() { vec3 r = g(1.0); }") > 0, "scalar->vector call argument is an error");
    check(analyzeErrors("fn void f() { vec3 v = vec3(1.0); }") == 0, "explicit vec3(scalar) is allowed");
    check(analyzeErrors("fn void f(vec3 w) { vec3 v = w * 2.0; }") == 0, "scalar*vector broadcast operator context is allowed");

    //  Type Domain Validation ─
    check(analyzeErrors("fn void f() { bool a=true; bool b=false; bool c=a+b; }") > 0, "bool arithmetic (+) is an error");
    check(analyzeErrors("fn void f() { bool a=true; bool b=false; bool c=a<b; }") > 0, "bool relational (<) is an error");
    check(analyzeErrors("fn void f() { bool a=true; bool b=false; bool c=a==b; }") == 0, "bool equality (==) is allowed");
    check(analyzeErrors("fn void f() { float a=1.0; int b=a & 3; }") > 0, "bitwise on a float operand is an error");

    //  Literal Integer Opacity and Range Constraints 
    check(analyzeErrors("fn void f() { int x = 3000000000; }") > 0, "signed literal above 2^31-1 is an error");
    check(analyzeErrors("fn void f() { uint x = 3000000000u; }") == 0, "unsigned literal up to 2^32-1 is allowed");

    //  Matrix Binary Evaluation ─
    expectInitType("mat4 * vec4 → vec4", "uniform mat4 M;\nfn void f(vec4 p) { vec4 r = M * p; }", 0, "vec4");
    expectInitType("mat4x3 * vec4 → vec3", "uniform mat4x3 M;\nfn void f(vec4 p) { vec3 r = M * p; }", 0, "vec3");
    expectInitType("mat3 * mat3 → mat3", "uniform mat3 A; uniform mat3 B;\nfn void f() { mat3 c=A*B; }", 0, "mat3");
    expectInitType("mat4x2 * mat2x4 → mat2", "uniform mat4x2 A; uniform mat2x4 B;\nfn void f() { mat2 c = A * B; }", 0, "mat2");
    expectInitType("mat3 * scalar → mat3", "uniform mat3 A;\nfn void f() { mat3 c = A * 2.0; }", 0, "mat3");
    check(analyzeErrors("uniform mat3 M;\nfn void f(vec4 p) { vec4 r = M*p; }") > 0, "mat3 * vec4 dimension mismatch is an error");
    check(analyzeErrors("uniform mat4x2 A; uniform mat2x4 B;\nfn void f() { mat4x2 c = A + B; }") > 0, "mat + mat of different shapes is an error");
    check(analyzeErrors("uniform mat3 A; uniform mat3 B;\nfn void f() { mat3 c = A / B; }") > 0, "matrix / matrix division is an error");

    //  Modulo Constraints ─
    check(analyzeErrors("fn void f() { float x = 2.5 % 1.0; }") > 0, "float modulo is an error");
    check(analyzeErrors("fn void f() { int n = 7; int m = n % 3; }") == 0, "integer modulo is allowed");

    //  Vector Constraints ─
    check(analyzeErrors("fn void f(vec3 a, vec2 b) { vec3 c = a + b; }") > 0, "adding vectors of different sizes is an error");

    //  Builtin Overload Resolution and Type Propagation 
    expectInitType("sin(float) → float", "fn void f(float x) { float y = sin(x); }", 0, "float");
    expectInitType("sin(vec3) → vec3 (genType)", "fn void f(vec3 v) { vec3 y = sin(v); }", 0, "vec3");
    expectInitType("length(vec3) → float (reduction)", "fn void f(vec3 v) { float y = length(v); }", 0, "float");
    expectInitType("dot(vec3,vec3) → float", "fn void f(vec3 a, vec3 b) { float y = dot(a, b); }", 0, "float");
    expectInitType("cross(vec3,vec3) → vec3", "fn void f(vec3 a, vec3 b) { vec3 y = cross(a, b); }", 0, "vec3");
    expectInitType("normalize(vec3) → vec3", "fn void f(vec3 v) { vec3 y = normalize(v); }", 0, "vec3");
    expectInitType("max(vec3, float) → vec3", "fn void f(vec3 v) { vec3 y = max(v, 0.0); }", 0, "vec3");
    expectInitType("texture(sampler2D, vec2) → vec4", "uniform sampler2D s;\nfn void f(vec2 uv) { vec4 c = texture(s, uv); }", 0, "vec4");
    check(analyzeErrors("fn void f(vec3 v) { int n = length(v); }") > 0, "narrowing a builtin float result to int is an error");

    //  Compound Lvalue and Element Assignment Coercion 
    check(analyzeErrors("fn void f() { int a[2]; a[0] = 1.5; }") > 0, "narrowing float into an int array element is an error");
    check(analyzeErrors("fn void f() { float a[2]; a[0] = 1; }") == 0, "widening int into a float array element is allowed");
    check(analyzeErrors("struct S { int n; };\nfn void f() { S s; s.n = 1.5; }") > 0, "narrowing float into an int struct field is an error");
    check(analyzeErrors("fn void f(vec3 v) { v.x = 1; }") == 0, "widening int into a float vector component is allowed");
    check(analyzeErrors("struct S { float x; };\nfn void f() { S s; s.x += 2.0; }") == 0, "compound assignment on a struct member is allowed");
    check(analyzeErrors("fn void f() { float a[3]; a[0] += 1.0; a[1] *= 2.0; }") == 0, "compound assignment on an array element is allowed");
    check(analyzeErrors("fn void f() { mat3 m; m[0][0] += 2.0; }") == 0, "compound assignment on a matrix element is allowed");
    check(analyzeErrors("struct S { float x; };\nfn void f() { S s; ++s.x; s.x++; }") == 0, "prefix/postfix ++ on a struct member is allowed");
    check(analyzeErrors("fn void f() { float a[3]; ++a[0]; a[1]--; }") == 0, "prefix/postfix ++/-- on an array element is allowed");
    check(analyzeErrors("fn void f() { int i = 0; ++i; i--; }") == 0, "++/-- on an int variable stays int");

    //  Def-driven Opaque Type Bindings 
    check(analyzeErrors("uniform sampler2DArray t;\nfn void f() {}") == 0, "sampler2DArray is a known type");
    check(analyzeErrors("uniform imageBuffer b;\nfn void f() {}") == 0, "imageBuffer is a known type");
    check(analyzeErrors("uniform sampler2D s;\nfn void f() {}") == 0, "sampler2D resolves correctly");

    //  Pipeline Stage I/O Location Collision Audits ─
    check(analyzeErrors("layout(location=2) in vec2 a;\nlayout(location=2) in vec2 b;\nfn void f() {}") > 0, "duplicate explicit locations collide");
    check(analyzeErrors("in vec2 a;\nlayout(location=0) in vec2 b;\nfn void f() {}") > 0, "auto location conflicts with explicit location");
    check(analyzeErrors("layout(location=3) out vec4 a;\nlayout(location=3) out vec4 b;\nfn void f() {}") > 0, "duplicate explicit outputs collide");
    check(analyzeErrors("layout(location=0) in vec2 a;\nlayout(location=0) out vec4 b;\nfn void f() {}") == 0, "input and output can share location index");
    check(analyzeErrors("layout(location=0) in vec2 a;\nlayout(location=1) in vec2 b;\nfn void f() {}") == 0, "distinct explicit input locations are safe");

    //  Subscript Domain Invariants 
    check(analyzeErrors("fn void f() { float a[3]; a[1.5] = 2.0; }") > 0, "float literal array index is an error");
    check(analyzeErrors("fn void f() { float a[3]; float i=1.0; a[i]=2.0; }") > 0, "float-typed array index is an error");
    check(analyzeErrors("fn void f() { mat3 m; m[1.0][0] = 2.0; }") > 0, "float matrix index is an error");
    check(analyzeErrors("fn void f() { float a[3]; a[1] = 2.0; }") == 0, "int literal array index is allowed");
    check(analyzeErrors("fn void f() { float a[3]; int i=1; a[i]=2.0; }") == 0, "int-typed array index is allowed");

    //  Double Precision Stage I/O Constraints 
    check(analyzeErrors("out double v;\n@entry @stage(vertex) fn void main() {}") > 0, "double VS output is rejected");
    check(analyzeErrors("in double v;\n@entry @stage(fragment) fn void main() {}") > 0, "double FS input is rejected");
    check(analyzeErrors("out double v[2];\n@entry @stage(vertex) fn void main() {}") > 0, "double array VS output is rejected");
    check(analyzeErrors("struct S { double d; };\nout S s;\n@entry @stage(vertex) fn void main() {}") > 0, "struct with double VS output is rejected");
    check(analyzeErrors("in double v;\n@entry @stage(vertex) fn void main() {}") == 0, "double VS input attribute is allowed");
    check(analyzeErrors("out double v;\n@entry @stage(fragment) fn void main() {}") == 0, "double FS output target is allowed");
    check(analyzeErrors("out float v;\n@entry @stage(vertex) fn void main() {}") == 0, "float VS output varying is allowed");

    //  Global Report Status ─
    if (failures == 0) {
        std::printf("All sema type-stamp tests passed.\n");
        return 0;
    }
    std::printf("%d sema test(s) FAILED.\n", failures);
    return 1;
}