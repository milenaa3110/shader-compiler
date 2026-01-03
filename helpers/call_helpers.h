// helpers/call_helpers.h
#ifndef CALL_HELPERS_H
#define CALL_HELPERS_H

namespace llvm { class Value; }
class CallExprAST;

llvm::Value* tryCodegenMatrixConstructor(const CallExprAST* call);
llvm::Value* tryCodegenVectorConstructor(const CallExprAST* call);
llvm::Value* tryCodegenScalarConstructor(const CallExprAST* call);
llvm::Value* tryCodegenStructConstructor(const CallExprAST* call);


#endif
