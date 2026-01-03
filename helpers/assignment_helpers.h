#ifndef COMPILER_GLSL_ASSIGNMENT_HELPERS_H
#define COMPILER_GLSL_ASSIGNMENT_HELPERS_H

#include <string>
#include <vector>
#include <memory>

// Forward declarations - NO circular includes!
class ExprAST;
class VariableExprAST;
class MatrixAccessExprAST;

// LLVM forward declarations
namespace llvm {
    class Value;
    class AllocaInst;
    class Type;
    class FixedVectorType;
    class StructType;
    class IRBuilderBase;
}

// Base class
class AssignmentHelper {
protected:
    ExprAST* Object;
    std::string Member;
    ExprAST* Init;

    std::vector<unsigned> parseSwizzle(unsigned maxDim, bool allowOverlap = false);
    llvm::AllocaInst* findVariableAlloca();

public:
    AssignmentHelper(ExprAST* obj, const std::string& mem, ExprAST* rhs);
    virtual llvm::Value* codegen() = 0;
    virtual ~AssignmentHelper() = default;
};

// Matrix column assignment: mat[col].xy = ...
class MatrixColumnAssigner : public AssignmentHelper {
private:
    MatrixAccessExprAST* MatAccess;

public:
    MatrixColumnAssigner(MatrixAccessExprAST* matAccess, 
                        const std::string& swizzle, ExprAST* rhs);
    llvm::Value* codegen() override;
};

// Struct field assignment: light.pos.x = ...
class StructFieldAssigner : public AssignmentHelper {
private:
    unsigned findFieldIndex(llvm::StructType* st);
    llvm::Value* coerceRHS(llvm::Type* fieldTy);

public:
    StructFieldAssigner(ExprAST* obj, const std::string& field, ExprAST* rhs);
    llvm::Value* codegen() override;
};

// Vector swizzle assignment: pos.xy = ...
class VectorSwizzleAssigner : public AssignmentHelper {
public:
    VectorSwizzleAssigner(ExprAST* vecVar, const std::string& swizzle, ExprAST* rhs);
    llvm::Value* codegen() override;
};

#endif
