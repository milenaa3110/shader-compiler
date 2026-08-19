// helpers/call_helpers.h
#ifndef CALL_HELPERS_H
#define CALL_HELPERS_H

namespace llvm { class Value; }
class CallExprAST;

// A constructor handler has three outcomes, and `nullptr` alone conflates the
// last two: "the callee doesn't name my kind of type" and "it does, but the
// arguments were wrong". The dispatcher in CallExprAST::codegen must tell them
// apart, or a genuinely bad `mat3(1.0, 2.0)` reports its own diagnostic, falls
// through the vector/scalar/struct constructors, and picks up a misleading
// "Unknown function referenced: mat3" as well.
enum class CtorStatus { NotMine, Ok, Failed };

struct CtorResult {
    CtorStatus status;
    llvm::Value* value;  // only meaningful when status == Ok

    static CtorResult notMine() { return {CtorStatus::NotMine, nullptr}; }
    static CtorResult ok(llvm::Value* v) { return {CtorStatus::Ok, v}; }
    static CtorResult failed() { return {CtorStatus::Failed, nullptr}; }
};

CtorResult tryCodegenMatrixConstructor(const CallExprAST* call);
CtorResult tryCodegenVectorConstructor(const CallExprAST* call);
CtorResult tryCodegenScalarConstructor(const CallExprAST* call);
CtorResult tryCodegenStructConstructor(const CallExprAST* call);

#endif
