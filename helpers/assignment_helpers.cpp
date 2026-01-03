// helpers/assignment_helpers.cpp
#include "assignment_helpers.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"
#include "../error_utils.h"
#include "../helpers/utils.h" 
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <fmt/core.h>

using namespace llvm;

// Base class constructor
AssignmentHelper::AssignmentHelper(ExprAST* obj, const std::string& mem, ExprAST* rhs)
    : Object(obj), Member(mem), Init(rhs) {}

std::vector<unsigned> AssignmentHelper::parseSwizzle(unsigned maxDim, bool allowOverlap) {
    std::vector<unsigned> indices;
    std::vector<bool> touched(maxDim, false);
    
    for (char c : Member) {
        int idx = charToIndex(c);
        if (idx < 0 || (unsigned)idx >= maxDim) {
            logErrorFmt("Invalid swizzle '{}': '{}' out of range [0-{}]", 
                       Member, c, maxDim-1);
            return {};
        }
        if (!allowOverlap && touched[idx]) {
            logError("Overlapping swizzle not allowed");
            return {};
        }
        touched[idx] = true;
        indices.push_back((unsigned)idx);
    }
    return indices;
}

AllocaInst* AssignmentHelper::findVariableAlloca() {
    if (auto* var = dynamic_cast<VariableExprAST*>(Object)) {
        auto it = NamedValues.find(var->Name);
        return it != NamedValues.end() ? it->second : nullptr;
    }
    return nullptr;
}

// ================= MATRIX COLUMN ASSIGNMENT =================
MatrixColumnAssigner::MatrixColumnAssigner(MatrixAccessExprAST* matAccess, 
    const std::string& swizzle, ExprAST* rhs)
    : AssignmentHelper(matAccess->Object.get(), swizzle, rhs), MatAccess(matAccess) {}

Value* MatrixColumnAssigner::codegen() {
    auto* lhsVar = dynamic_cast<VariableExprAST*>(MatAccess->Object.get());
    if (!lhsVar) {
        logError("Matrix swizzle assignment LHS must be variable matrix");
        return nullptr;
    }

    auto it = NamedValues.find(lhsVar->Name);
    if (it == NamedValues.end()) {
        logErrorFmt("Unknown variable '{}' in matrix assignment", lhsVar->Name);
        return nullptr;
    }
    AllocaInst* alloca = it->second;

    Type* matTy = alloca->getAllocatedType();
    if (!matTy->isArrayTy()) {
        logError("Expected matrix type for swizzle assignment");
        return nullptr;
    }

    auto* arrTy = cast<ArrayType>(matTy);
    auto* colTy = cast<FixedVectorType>(arrTy->getElementType());
    Type* elemTy = colTy->getElementType();
    unsigned rows = colTy->getNumElements();

    auto indices = parseSwizzle(rows);
    if (indices.empty()) return nullptr;
    const unsigned K = (unsigned)indices.size();

    Value* colIdx = MatAccess->Index->codegen();
    if (!colIdx) return nullptr;
    colIdx = toI32(colIdx);
    if (!colIdx) return nullptr;

    Value* rhs = Init->codegen();
    if (!rhs) return nullptr;

    std::vector<Value*> comps;
    comps.reserve(K);
    if (rhs->getType()->isVectorTy()) {
        auto* rVecTy = cast<FixedVectorType>(rhs->getType());
        if (rVecTy->getNumElements() != K) {
            logError("RHS vector size mismatch in matrix swizzle assignment");
            return nullptr;
        }
        Type* rElemTy = rVecTy->getElementType();
        for (unsigned i = 0; i < K; ++i) {
            Value* e = Builder->CreateExtractElement(rhs, Builder->getInt32(i));
            if (rElemTy != elemTy) {
                e = castScalarTo(e, elemTy);
                if (!e) return nullptr;
            }
            comps.push_back(e);
        }
    } else {
        Value* s = rhs;
        if (s->getType() != elemTy) {
            s = castScalarTo(s, elemTy);
            if (!s) return nullptr;
        }
        for (unsigned i = 0; i < K; ++i)
            comps.push_back(s);
    }

    Value* curMat = Builder->CreateLoad(matTy, alloca);
    AllocaInst* tmpA = CreateEntryBlockAlloca(
        Builder->GetInsertBlock()->getParent(), "tmp.mat", matTy);
    Builder->CreateStore(curMat, tmpA);

    Value* zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
    Value* colPtr = Builder->CreateGEP(matTy, tmpA, {zero, colIdx}, "col.ptr");
    Value* colVal = Builder->CreateLoad(colTy, colPtr);

    Value* newCol = colVal;
    for (unsigned i = 0; i < K; ++i) {
        newCol = Builder->CreateInsertElement(newCol, comps[i],
                                              Builder->getInt32(indices[i]));
    }

    Builder->CreateStore(newCol, colPtr);
    Value* newMat = Builder->CreateLoad(matTy, tmpA);
    Builder->CreateStore(newMat, alloca);
    return newMat;
}

StructFieldAssigner::StructFieldAssigner(ExprAST* obj, const std::string& field, ExprAST* rhs)
    : AssignmentHelper(obj, field, rhs) {}

// Struct field assignment
unsigned StructFieldAssigner::findFieldIndex(StructType* st) {
    std::string stName = st->hasName() ? st->getName().str() : "";
    if (stName.empty()) {
        logError("Cannot assign field of unnamed struct");
        return -1u;
    }

    auto itNames = NamedStructTypes.find(stName);
    if (itNames == NamedStructTypes.end()) {
        logErrorFmt("Struct field names not registered for {}", stName);
        return -1u;
    }

    const auto& fields = itNames->second.FieldNames;
    auto fit = std::find(fields.begin(), fields.end(), Member);
    if (fit == fields.end()) {
        logErrorFmt("Struct {} has no field '{}'", stName, Member);
        return -1u;
    }

    return std::distance(fields.begin(), fit);
}

Value* StructFieldAssigner::coerceRHS(Type* fieldTy) {
    Value* rhs = Init->codegen();
    if (!rhs) return nullptr;

    if (rhs->getType() != fieldTy) {
        if (fieldTy->isVectorTy() && !rhs->getType()->isVectorTy()) {
            auto* vecTy = cast<FixedVectorType>(fieldTy);
            Type* elemTy = vecTy->getElementType();
            Value* scalar = castScalarTo(rhs, elemTy);
            if (!scalar) return nullptr;
            return splatScalarToVector(scalar, vecTy);
        } else {
            return castScalarTo(rhs, fieldTy);
        }
    }
    return rhs;
}

Value* StructFieldAssigner::codegen() {
    AllocaInst* alloca = findVariableAlloca();
    if (!alloca) {
        logError("Struct assignment LHS must be variable");
        return nullptr;
    }

    Type* objTy = alloca->getAllocatedType();
    if (!objTy->isStructTy()) return nullptr;

    auto* st = cast<StructType>(objTy);
    unsigned idx = findFieldIndex(st);
    if (idx == -1u) return nullptr;

    Type* fieldTy = st->getElementType(idx);
    Value* rhs = coerceRHS(fieldTy);
    if (!rhs) return nullptr;

    if (auto* baseVar = dynamic_cast<VariableExprAST*>(Object)) {
        Value* cur = Builder->CreateLoad(objTy, alloca, (baseVar->Name + ".old").c_str());
        Value* updated = Builder->CreateInsertValue(cur, rhs, {idx});
        Builder->CreateStore(updated, alloca);
        return updated;
    }
    return nullptr;
}
VectorSwizzleAssigner::VectorSwizzleAssigner(ExprAST* vecVar, const std::string& swizzle, ExprAST* rhs)
    : AssignmentHelper(vecVar, swizzle, rhs) {}

// Vector swizzle assignment
Value* VectorSwizzleAssigner::codegen() {
    AllocaInst* alloca = findVariableAlloca();
    if (!alloca) {
        logError("Vector swizzle LHS must be variable");
        return nullptr;
    }

    Type* objTy = alloca->getAllocatedType();
    if (!objTy->isVectorTy()) {
        logError("Swizzle assignment only supported on vector variables");
        return nullptr;
    }

    auto* vecTy = cast<FixedVectorType>(objTy);
    Type* elemTy = vecTy->getElementType();
    unsigned N = vecTy->getNumElements();

    auto indices = parseSwizzle(N, false);
    if (indices.empty()) return nullptr;
    const unsigned K = (unsigned)indices.size();

    Value* rhs = Init->codegen();
    if (!rhs) return nullptr;

    std::vector<Value*> comps;
    comps.reserve(K);

    if (rhs->getType()->isVectorTy()) {
        auto* rVecTy = cast<FixedVectorType>(rhs->getType());
        if (rVecTy->getNumElements() != K) {
            logErrorFmt("RHS vector width ({}) must match swizzle length ({})", 
                       rVecTy->getNumElements(), K);
            return nullptr;
        }
        Type* rElemTy = rVecTy->getElementType();
        for (unsigned i = 0; i < K; ++i) {
            Value* e = Builder->CreateExtractElement(rhs, Builder->getInt32(i));
            if (rElemTy != elemTy) {
                e = castScalarTo(e, elemTy);
                if (!e) return nullptr;
            }
            comps.push_back(e);
        }
    } else {
        Value* s = rhs;
        if (s->getType() != elemTy) {
            s = castScalarTo(s, elemTy);
            if (!s) return nullptr;
        }
        for (unsigned i = 0; i < K; ++i)
            comps.push_back(s);
    }

    if (auto* baseVar = dynamic_cast<VariableExprAST*>(Object)) {
        Value* base = Builder->CreateLoad(objTy, alloca, (baseVar->Name + ".old").c_str());
        Value* newVec = base;
        for (unsigned i = 0; i < K; ++i) {
            newVec = Builder->CreateInsertElement(newVec, comps[i],
                                                  Builder->getInt32(indices[i]), "ins");
        }
        Builder->CreateStore(newVec, alloca);
        return newVec;
    }
    return nullptr;
}
