#include "ast.h"
#include <fmt/core.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include "../codegen_state/codegen_state.h"
#include "../error_utils.h"
#include "../helpers/utils.h"
#include "../helpers/assignment_helpers.h"
#include "../helpers/call_helpers.h"

using namespace llvm;

std::vector<StageVar> StageInputVars;
std::vector<StageVar> StageOutputVars;

// Check if all paths in a function return a value
static bool allPathsReturn(const ExprAST* body) {
    if (!body) return false;

    if (auto* block = dynamic_cast<const BlockExprAST*>(body)) {
        if (block->Statements.empty()) return false;
        // Only the last statement can guarantee function termination
        return allPathsReturn(block->Statements.back().get());
    }

    if (dynamic_cast<const ReturnStmtAST*>(body)) {
        return true;
    }

    if (auto* ifExpr = dynamic_cast<const IfExprAST*>(body)) {
        // Must have else, and both branches must return
        if (!ifExpr->ThenExpr) return false;
        if (!ifExpr->ElseExpr) return false;
        return allPathsReturn(ifExpr->ThenExpr.get()) &&
               allPathsReturn(ifExpr->ElseExpr.get());
    }

    return false;
}

static Value* toBool(Value* v) {
    Type* t = v->getType();
    if (t->isIntegerTy(1)) return v;

    if (t->isIntegerTy() || t->isIntOrIntVectorTy()) {
        Value* zero = ConstantInt::get(t, 0);
        return Builder->CreateICmpNE(v, zero, "tobool");
    }

    if (t->isFloatTy() || t->isDoubleTy() || t->isFPOrFPVectorTy()) {
        Value* zero = ConstantFP::get(t, 0.0);
        // UNE => NaN counts as true (common “nonzero” behavior); use ONE if you want NaN->false
        return Builder->CreateFCmpUNE(v, zero, "tobool");
    }

    logError("Cannot convert to bool");
    return nullptr;
}

// Number literal
Value* NumberExprAST::codegen() {
    return llvm::ConstantFP::get(llvm::Type::getFloatTy(*Context), Val);
}

// Variable reference
Value* VariableExprAST::codegen() {
    // Check local variables first
    auto it = NamedValues.find(Name);
    if (it != NamedValues.end()) {
        AllocaInst* A = it->second;
        return Builder->CreateLoad(A->getAllocatedType(), A, Name);
    }
    // Try module global (uniform)
    if (TheModule) {
        if (auto *G = TheModule->getGlobalVariable(Name)) {
            // global is a pointer to its value type, load it
            return Builder->CreateLoad(G->getValueType(), G, Name);
        }
    }
    logError(fmt::format("Unknown variable or uniform: {}", Name));
    return nullptr;
}

// Unary operator
Value* UnaryExprAST::codegen() {
    Value* v = Operand->codegen();
    if (!v) {
        logError("Unary codegen: operand is null");
        return nullptr;
    }

    if (Op == tok_minus || Op == '-') {
        if (v->getType()->isFPOrFPVectorTy()) {
            return Builder->CreateFNeg(v, "neg");
        }
        if (v->getType()->isIntOrIntVectorTy()) {
            if (v->getType()->isIntegerTy(1)) {
                logError("Negation not supported for boolean type");
                return nullptr;
            }
            return Builder->CreateNeg(v, "neg");
        }
        logError(fmt::format("Negation not supported for type: {}", v->getType()->getStructName()));
        return nullptr;
    }

    if (Op == tok_not || Op == '!') {
        if (v->getType()->isIntegerTy(1)) {
            return Builder->CreateNot(v, "not");
        }
        if (v->getType()->isIntOrIntVectorTy()) {
            auto *one = ConstantInt::get(v->getType(), 1);
            return Builder->CreateXor(v, one, "not");
        }
        logError("Logical NOT (!) requires boolean type");
        return nullptr;
    }
    
    logError("Unknown unary operator: " + std::string(1, Op));
    return nullptr;
}

// Binary operator
Value* BinaryExprAST::codegen() {
    Value* L = LHS->codegen();
    Value* R = RHS->codegen();
    if (!L || !R) return nullptr;

    if (!makeTypesMatch(L, R)) {
        logError("Type mismatch in binary op");
        return nullptr;
    }

    Type* opType = L->getType();

    // arithmetic operators
    if (Op == tok_plus || Op == '+') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFAdd(L, R, "addtmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateAdd(L, R, "addtmp");
    }
    
    if (Op == tok_minus || Op == '-') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFSub(L, R, "subtmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateSub(L, R, "subtmp");
    }
    
    if (Op == tok_multiply || Op == '*') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFMul(L, R, "multmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateMul(L, R, "multmp");
    }
    
    if (Op == tok_divide || Op == '/') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFDiv(L, R, "divtmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateSDiv(L, R, "divtmp");
    }

    if (Op == '%') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFRem(L, R, "remtmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateURem(L, R, "remtmp");
    }

    // comparison operators
    if (Op == tok_less || Op == '<') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOLT(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpSLT(L, R, "cmptmp");
    }
    
    if (Op == tok_less_equal) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOLE(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpSLE(L, R, "cmptmp");
    }
    
    if (Op == tok_greater || Op == '>') {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOGT(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpSGT(L, R, "cmptmp");
    }
    
    if (Op == tok_greater_equal) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOGE(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpSGE(L, R, "cmptmp");
    }
    
    if (Op == tok_equal) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOEQ(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpEQ(L, R, "cmptmp");
    }
    
    if (Op == tok_not_equal) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpONE(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpNE(L, R, "cmptmp");
    }

    // logical operators
    if (Op == tok_and || Op == tok_or) {
        Function* F = Builder->GetInsertBlock()->getParent();

        Value* L = LHS->codegen();
        if (!L) return nullptr;

        BasicBlock* LHSBB = Builder->GetInsertBlock();

        if (LHSBB->getTerminator()) {
            return nullptr;
        }

        Value* Lbool = toBool(L);

        BasicBlock* RHSBB   = BasicBlock::Create(*Context, "logical.rhs", F);
        BasicBlock* MergeBB = BasicBlock::Create(*Context, "logical.merge", F);

        if (Op == tok_and) {
            Builder->CreateCondBr(Lbool, RHSBB, MergeBB);
        } else {
            Builder->CreateCondBr(Lbool, MergeBB, RHSBB);
        }

        Builder->SetInsertPoint(RHSBB);
        Value* R = RHS->codegen();
        if (!R) return nullptr;

        BasicBlock* RHSBBEnd = Builder->GetInsertBlock();

        if (!RHSBBEnd->getTerminator()) {
            Value* Rbool = toBool(R);
            Builder->CreateBr(MergeBB);

            Builder->SetInsertPoint(MergeBB);
            PHINode* PN = Builder->CreatePHI(Type::getInt1Ty(*Context), 2, "logic.result");

            Value* shortVal = (Op == tok_and)
                ? ConstantInt::getFalse(*Context)
                : ConstantInt::getTrue(*Context);

            PN->addIncoming(shortVal, LHSBB);
            PN->addIncoming(Rbool, RHSBBEnd);
            return PN;
        }


        Builder->SetInsertPoint(MergeBB);
        return (Op == tok_and)
            ? ConstantInt::getFalse(*Context)
            : ConstantInt::getTrue(*Context);

    }


    logError("Unsupported binary operator: " + std::string(1, Op));
    return nullptr;
}

// Boolean literal ast
Value* BooleanExprAST::codegen() {
    return ConstantInt::get(Type::getInt1Ty(*Context), Val ? 1 : 0);
}

// Ternary expression: cond ? thenExpr : elseExpr
Value* TernaryExprAST::codegen() {
    Value* condVal = Cond->codegen();
    if (!condVal) return nullptr;

    if (!condVal->getType()->isIntegerTy(1)) {
        if (condVal->getType()->isIntegerTy())
            condVal = Builder->CreateICmpNE(condVal, ConstantInt::get(condVal->getType(), 0), "ternary.cond");
        else if (condVal->getType()->isFPOrFPVectorTy())
            condVal = Builder->CreateFCmpONE(condVal, ConstantFP::get(condVal->getType(), 0.0), "ternary.cond");
    }

    Function* F = Builder->GetInsertBlock()->getParent();
    BasicBlock* ThenBB  = BasicBlock::Create(*Context, "ternary.then", F);
    BasicBlock* ElseBB  = BasicBlock::Create(*Context, "ternary.else", F);
    BasicBlock* MergeBB = BasicBlock::Create(*Context, "ternary.merge", F);

    Builder->CreateCondBr(condVal, ThenBB, ElseBB);

    Builder->SetInsertPoint(ThenBB);
    Value* thenVal = ThenExpr->codegen();
    if (!thenVal) return nullptr;
    Builder->CreateBr(MergeBB);
    ThenBB = Builder->GetInsertBlock();

    Builder->SetInsertPoint(ElseBB);
    Value* elseVal = ElseExpr->codegen();
    if (!elseVal) return nullptr;
    Builder->CreateBr(MergeBB);
    ElseBB = Builder->GetInsertBlock();

    Builder->SetInsertPoint(MergeBB);
    if (thenVal->getType() != elseVal->getType())
        makeTypesMatch(thenVal, elseVal);

    PHINode* PN = Builder->CreatePHI(thenVal->getType(), 2, "ternary.result");
    PN->addIncoming(thenVal, ThenBB);
    PN->addIncoming(elseVal, ElseBB);
    return PN;
}

// Member access - struct field or vector swizzle
Value *MemberAccessExprAST::codegen() {
  Value *obj = Object->codegen();
  if (!obj)
    return nullptr;

  Type *objTy = obj->getType();

  // Handle pointers - load the value
  if (objTy->isPointerTy()) {
    Type *pointedTy = nullptr;
    
    // Try to get the pointed-to type from known instruction types
    if (auto *AI = dyn_cast<AllocaInst>(obj)) {
      pointedTy = AI->getAllocatedType();
    } else if (auto *GV = dyn_cast<GlobalVariable>(obj)) {
      pointedTy = GV->getValueType();
    } else {
      logError("Cannot determine pointed-to type for member access");
      return nullptr;
    }

    if (!pointedTy) {
      logError("Cannot determine pointed-to type for member access");
      return nullptr;
    }

    obj = Builder->CreateLoad(pointedTy, obj, ("load." + Member).c_str());
    objTy = pointedTy;
  }

  // STRUCT access
  if (objTy->isStructTy()) {
    return extractStructMember(obj, cast<StructType>(objTy), Member);
  }

  // VECTOR swizzle
  if (objTy->isVectorTy()) {
    return extractVectorComponents(obj, cast<FixedVectorType>(objTy), Member);
  }

  logError(fmt::format("Member access only supported on structs and vectors, got type with ID {}", objTy->getTypeID()));
  return nullptr;
}

// Member assignment - struct field or vector swizzle
llvm::Value* MemberAssignmentExprAST::codegen() {
    // MATRIX column: mat[col].xy = ...
    if (auto* matAccess = dynamic_cast<MatrixAccessExprAST*>(Object.get())) {
        MatrixColumnAssigner assigner(matAccess, Member, Init.get());
        return assigner.codegen();
    }
    
    // Find variable alloca (struct/vector)
    AllocaInst* alloca = nullptr;
    VariableExprAST* baseVar = nullptr;
    if (auto* varExpr = dynamic_cast<VariableExprAST*>(Object.get())) {
        auto it = NamedValues.find(varExpr->Name);
        if (it != NamedValues.end()) {
            alloca = it->second;
            baseVar = varExpr;
        }
    }
    
    if (!alloca) {
        logError("Assignment LHS must be variable (e.g., a.xy = ..., lights[0].prop1 = ...)");
        return nullptr;
    }
    
    Type* objTy = alloca->getAllocatedType();
    
    // STRUCT field: light.pos.x = ...
    if (objTy->isStructTy()) {
        StructFieldAssigner assigner(Object.get(), Member, Init.get());
        return assigner.codegen();
    }
    
    // VECTOR swizzle: pos.xy = ...
    if (objTy->isVectorTy()) {
        VectorSwizzleAssigner assigner(Object.get(), Member, Init.get());
        return assigner.codegen();
    }
    
    logError("Unsupported member assignment type");
    return nullptr;
}

// Function call or constructor call ast
Value* CallExprAST::codegen() {
    if (auto* v = tryCodegenMatrixConstructor(this))
        return v;

    if (auto* v = tryCodegenVectorConstructor(this))
        return v;

    if (auto* v = tryCodegenScalarConstructor(this))
        return v;

    if (auto* v = tryCodegenStructConstructor(this))
        return v;

    // Generate arguments once
    std::vector<llvm::Value*> ArgsV;
    ArgsV.reserve(Args.size());
    for (auto& A : Args) {
        llvm::Value* v = A->codegen();
        if (!v) return nullptr;
        ArgsV.push_back(v);
    }

    // Try builtin with generated args
    if (auto* bv = codegenBuiltin(*Builder, TheModule.get(), Callee, ArgsV))
        return bv;

    // User-defined function
    Function* CalleeF = TheModule->getFunction(Callee);
    if (!CalleeF) {
        logError("Unknown function referenced: " + Callee);
        return nullptr;
    }

    if (CalleeF->arg_size() != Args.size()) {
        logError(fmt::format("Incorrect number of arguments for function {}. Expected {}, got {}",
                           Callee, CalleeF->arg_size(), Args.size()));
        return nullptr;
    }

    // Cast arguments to expected types
    unsigned i = 0;
    for (auto& argVal : ArgsV) {
        Type* expectedTy = CalleeF->getFunctionType()->getParamType(i);
        
        if (argVal->getType() != expectedTy) {
            Value* casted = castScalarTo(argVal, expectedTy);
            if (!casted) {
                logError(fmt::format("Cannot cast argument {} to expected type for function {}", i, Callee));
                return nullptr;
            }
            argVal = casted;
            ArgsV[i] = argVal;
        }
        ++i;
    }

    return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

llvm::Value* IfExprAST::codegen() {
    using namespace llvm;

    Value* condVal = Condition->codegen();
    if (!condVal) return nullptr;

    // Ensure condition is i1 (adjust if your language allows float/bool conditions)
    if (condVal->getType()->isFloatTy()) {
        condVal = Builder->CreateFCmpONE(
            condVal,
            ConstantFP::get(Type::getFloatTy(*Context), 0.0f),
            "ifcond"
        );
    } else if (condVal->getType()->isIntegerTy() && !condVal->getType()->isIntegerTy(1)) {
        condVal = Builder->CreateICmpNE(
            condVal,
            ConstantInt::get(condVal->getType(), 0),
            "ifcond"
        );
    }

    Function* F = Builder->GetInsertBlock()->getParent();

    BasicBlock* thenBB  = BasicBlock::Create(*Context, "then", F);
    BasicBlock* elseBB  = ElseExpr ? BasicBlock::Create(*Context, "else", F) : nullptr;
    BasicBlock* mergeBB = BasicBlock::Create(*Context, "ifend", F);

    Builder->CreateCondBr(condVal, thenBB, ElseExpr ? elseBB : mergeBB);

    // ---- THEN ----
    Builder->SetInsertPoint(thenBB);
    if (!ThenExpr->codegen()) return nullptr;

    BasicBlock* thenEndBB = Builder->GetInsertBlock();
    bool thenTerminated = (thenEndBB->getTerminator() != nullptr);

    // ---- ELSE ----
    BasicBlock* elseEndBB = nullptr;
    bool elseTerminated = false;

    if (ElseExpr) {
        Builder->SetInsertPoint(elseBB);
        if (!ElseExpr->codegen()) return nullptr;

        elseEndBB = Builder->GetInsertBlock();
        elseTerminated = (elseEndBB->getTerminator() != nullptr);
    }

    // If THEN didn't terminate, jump to merge from the *actual* end block.
    if (!thenTerminated) {
        Builder->SetInsertPoint(thenEndBB);
        Builder->CreateBr(mergeBB);
    }

    // If ELSE exists and didn't terminate, jump to merge from the *actual* end block.
    if (ElseExpr && !elseTerminated) {
        Builder->SetInsertPoint(elseEndBB);
        Builder->CreateBr(mergeBB);
    }

    // If no ELSE, false edge already goes to mergeBB, so merge is reachable.
    Builder->SetInsertPoint(mergeBB);

    // Your language currently treats `if` as statement-like expression -> dummy value
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

llvm::Value* BlockExprAST::codegen() {
    llvm::Value* last = nullptr;

    for (auto& s : Statements) {
        if (Builder->GetInsertBlock()->getTerminator()) {
            break;
        }
        last = s->codegen();
        if (!last) return nullptr;
    }

    if (!last)
        last = llvm::ConstantFP::get(llvm::Type::getFloatTy(*Context), 0.0f);
    return last;
}


// Assignment statement
llvm::Value* AssignmentExprAST::codegen() {
    Value* val = Init->codegen();
    if (!val) return nullptr;

    if (VarType.empty()) {
        auto it = NamedValues.find(VarName);
        if (it == NamedValues.end()) {
            logError(fmt::format("Undefined variable in assignment: {}", VarName));
            return nullptr;
        }
        AllocaInst* Alloca = it->second;

        // Type cast if needed
        Type* targetType = Alloca->getAllocatedType();
        if (val->getType() != targetType) {
            if (targetType->isVectorTy() && !val->getType()->isVectorTy()) {
                auto* vecTy = cast<FixedVectorType>(targetType);
                Type* elemTy = vecTy->getElementType();
                Value* scalar = castScalarTo(val, elemTy);
                if (!scalar) {
                    logError("Cannot cast scalar to vector element type");
                    return nullptr;
                }
                val = splatScalarToVector(scalar, vecTy);
            } else {
                Value* casted = castScalarTo(val, targetType);
                if (!casted) {
                    logError("Cannot cast value to target type");
                    return nullptr;
                }
                val = casted;
            }
        }

        Builder->CreateStore(val, Alloca);
        return val;
    }

    // EXISTING CODE for declaration with initialization - remains the same!
    Type* Ty = resolveTypeByName(VarType);
    if (!Ty) {
        logError(fmt::format("Unknown type in assignment: {}", VarType));
        return nullptr;
    }

    Function* F = Builder->GetInsertBlock()->getParent();
    AllocaInst* Alloca = nullptr;

    auto it = NamedValues.find(VarName);
    if (it == NamedValues.end()) {
        Alloca = CreateEntryBlockAlloca(F, VarName, Ty);
        NamedValues[VarName] = Alloca;
    } else {
        Alloca = it->second;
        if (Alloca->getAllocatedType() != Ty) {
            logError(fmt::format("Type mismatch for variable '{}'", VarName));
            return nullptr;
        }
    }

    if (val->getType() != Ty) {
        if (Ty->isVectorTy()) {
            auto* vecTy = cast<FixedVectorType>(Ty);
            Type* elemTy = vecTy->getElementType();

            if (val->getType()->isVectorTy()) {
                logError(fmt::format("Vector type mismatch in assignment to {}", VarName));
                return nullptr;
            } else {
                Value* scalar = castScalarTo(val, elemTy);
                if (!scalar) {
                    logError("Cannot cast scalar to vector element type");
                    return nullptr;
                }
                val = splatScalarToVector(scalar, vecTy);
            }
        } else if (Ty->isDoubleTy() || Ty->isFloatTy() || Ty->isIntegerTy()) {
            Value* casted = castScalarTo(val, Ty);
            if (!casted) {
                logError("Cannot cast initializer to target scalar type");
                return nullptr;
            }
            val = casted;
        } else {
            logError("Unsupported target type in assignment");
            return nullptr;
        }
    }

    Builder->CreateStore(val, Alloca);
    return val;
}

Value *MatrixAccessExprAST::codegen() {
    auto *lhsVar = dynamic_cast<VariableExprAST *>(Object.get());
    if (!lhsVar) {
        logError("Matrix/array access LHS must be a variable (e.g., M[i] or M[i][j])");
        return nullptr;
    }

    auto it = NamedValues.find(lhsVar->Name);
    if (it != NamedValues.end()) {
        AllocaInst *alloca = it->second;
        Type *baseTy       = alloca->getAllocatedType();

        if (!baseTy->isArrayTy()) {
            logError("Matrix/array access only supported on array types for local vars");
            return nullptr;
        }

        auto *arrTy  = cast<ArrayType>(baseTy);
        Type *elemTy = arrTy->getElementType();

        // first index
        if (!Index) {
            logError("Array/matrix access requires first index");
            return nullptr;
        }

        Value *iVal = Index->codegen();
        if (!iVal) return nullptr;
        iVal = toI32(iVal);
        if (!iVal) {
            logError("First index is not convertible to i32");
            return nullptr;
        }

        Value *zero    = ConstantInt::get(Type::getInt32Ty(*Context), 0);
        Value *elemPtr = Builder->CreateInBoundsGEP(
            baseTy, alloca, { zero, iVal }, lhsVar->Name + ".elem.ptr"
        );

        if (!Index2) {
            if (elemTy->isVectorTy()) {
                return Builder->CreateLoad(elemTy, elemPtr, lhsVar->Name + ".col");
            }
            return Builder->CreateLoad(elemTy, elemPtr, lhsVar->Name + ".elem");
        }

        if (!elemTy->isVectorTy()) {
            logError("Second index is only valid for matrix (array-of-vector)");
            return nullptr;
        }

        auto *colTy  = cast<FixedVectorType>(elemTy);
        Value *col   = Builder->CreateLoad(colTy, elemPtr, lhsVar->Name + ".col");

        Value *jVal = Index2->codegen();
        if (!jVal) return nullptr;
        jVal = toI32(jVal);
        if (!jVal) {
            logError("Second index is not convertible to i32");
            return nullptr;
        }

        return Builder->CreateExtractElement(col, jVal, "m.elem");
    }

    // Storage buffer (compute shader): @name = external global ptr
    auto sit = StorageBufferInfos.find(lhsVar->Name);
    if (sit != StorageBufferInfos.end()) {
        auto& info = sit->second;
        auto* ptrTy = PointerType::getUnqual(*Context);
        // Load the pointer: %p = load ptr, ptr @name
        // Mark invariant.load so LICM can hoist this out of any surrounding loop —
        // the pointer stored in @src / @dst doesn't change during a dispatch.
        auto* ptrLoad = Builder->CreateLoad(ptrTy, info.gv, lhsVar->Name + ".ptr");
        ptrLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                             llvm::MDNode::get(*Context, {}));
        Value* bufPtr = ptrLoad;
        // Compute the index
        Value* iVal = Index->codegen();
        if (!iVal) return nullptr;
        iVal = toI32(iVal);
        if (!iVal) { logError("Storage buffer index not convertible to i32"); return nullptr; }
        // GEP into the buffer: getelementptr elemTy, ptr %p, i32 idx
        Value* elemPtr = Builder->CreateInBoundsGEP(
            info.elemTy, bufPtr, {iVal}, lhsVar->Name + ".elem.ptr");
        return Builder->CreateLoad(info.elemTy, elemPtr, lhsVar->Name + ".elem");
    }

    auto uit = UniformArrays.find(lhsVar->Name);
    if (uit == UniformArrays.end()) {
        logError(fmt::format("Unknown variable '{}' in matrix/array access", lhsVar->Name));
        return nullptr;
    }

    auto *gv    = uit->second;
    Type *baseTy = gv->getValueType();

    if (!baseTy->isArrayTy()) {
        logError(fmt::format("Uniform '{}' is not an array type", lhsVar->Name));
        return nullptr;
    }

    auto *arrTy  = cast<ArrayType>(baseTy);
    Type *elemTy = arrTy->getElementType();

    if (!Index) {
        logError(fmt::format("Uniform array '{}' requires an index", lhsVar->Name));
        return nullptr;
    }

    Value *iVal = Index->codegen();
    if (!iVal) return nullptr;
    iVal = toI32(iVal);
    if (!iVal) {
        logError("First index is not convertible to i32 for uniform array");
        return nullptr;
    }

    Value *zero    = ConstantInt::get(Type::getInt32Ty(*Context), 0);
    Value *elemPtr = Builder->CreateInBoundsGEP(
        baseTy, gv, { zero, iVal }, lhsVar->Name + ".u.elem.ptr"
    );

    if (!Index2) {
        if (elemTy->isVectorTy()) {
            return Builder->CreateLoad(elemTy, elemPtr, lhsVar->Name + ".u.col");
        }
        return Builder->CreateLoad(elemTy, elemPtr, lhsVar->Name + ".u.elem");
    }

    // Uniform matrix = uniform array-of-vector (e.g., uniform mat4x4 in Mat[4])
    if (!elemTy->isVectorTy()) {
        logError("Second index is only valid for matrix-like uniform (array-of-vector)");
        return nullptr;
    }

    auto *colTy  = cast<FixedVectorType>(elemTy);
    Value *col   = Builder->CreateLoad(colTy, elemPtr, lhsVar->Name + ".u.col");

    Value *jVal = Index2->codegen();
    if (!jVal) return nullptr;
    jVal = toI32(jVal);
    if (!jVal) {
        logError("Second index is not convertible to i32 for uniform matrix");
        return nullptr;
    }

    return Builder->CreateExtractElement(col, jVal, "u.m.elem");
}

// Matrix / storage-buffer element assignment: a[i] = val  or  a[i][j] = val
Value* MatrixAssignmentExprAST::codegen() {
    auto* lhsVar = dynamic_cast<VariableExprAST*>(Object.get());
    if (!lhsVar) {
        logError("Array assignment LHS must be a named variable");
        return nullptr;
    }

    // Storage buffer write: dst[idx] = val
    auto sit = StorageBufferInfos.find(lhsVar->Name);
    if (sit != StorageBufferInfos.end()) {
        auto& info = sit->second;
        auto* ptrTy = PointerType::getUnqual(*Context);
        auto* wrPtrLoad = Builder->CreateLoad(ptrTy, info.gv, lhsVar->Name + ".ptr");
        wrPtrLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                               llvm::MDNode::get(*Context, {}));
        Value* bufPtr = wrPtrLoad;
        Value* iVal = Index->codegen();
        if (!iVal) return nullptr;
        iVal = toI32(iVal);
        if (!iVal) { logError("Storage buffer index not convertible to i32"); return nullptr; }
        Value* elemPtr = Builder->CreateInBoundsGEP(
            info.elemTy, bufPtr, {iVal}, lhsVar->Name + ".wr.ptr");
        Value* rhs = RHS->codegen();
        if (!rhs) return nullptr;
        if (rhs->getType() != info.elemTy) {
            if (info.elemTy->isIntegerTy() && rhs->getType()->isIntegerTy())
                rhs = Builder->CreateIntCast(rhs, info.elemTy, false, "sb.cast");
            else if (info.elemTy->isFloatTy() && rhs->getType()->isDoubleTy())
                rhs = Builder->CreateFPTrunc(rhs, info.elemTy, "sb.cast");
            else if (info.elemTy->isIntegerTy() && rhs->getType()->isFloatingPointTy())
                rhs = Builder->CreateFPToUI(rhs, info.elemTy, "sb.cast");
        }
        Builder->CreateStore(rhs, elemPtr);
        return rhs;
    }

    // Local array write: arr[i] = val
    auto it = NamedValues.find(lhsVar->Name);
    if (it == NamedValues.end()) {
        logError(fmt::format("Unknown variable '{}' in array assignment", lhsVar->Name));
        return nullptr;
    }
    AllocaInst* alloca = it->second;
    Type* matTy = alloca->getAllocatedType();
    if (!matTy->isArrayTy()) {
        logError(fmt::format("'{}' is not an array type", lhsVar->Name));
        return nullptr;
    }
    auto* arrTy = cast<ArrayType>(matTy);
    Type* colTy = arrTy->getElementType();
    Value* iVal = Index->codegen();
    if (!iVal) return nullptr;
    iVal = toI32(iVal);
    Value* zero   = ConstantInt::get(Type::getInt32Ty(*Context), 0);
    Value* colPtr = Builder->CreateInBoundsGEP(matTy, alloca, {zero, iVal},
                                               lhsVar->Name + ".col.ptr");
    if (!Index2) {
        Value* rhs = RHS->codegen();
        if (!rhs) return nullptr;
        Builder->CreateStore(rhs, colPtr);
        return rhs;
    }
    auto* vecColTy = cast<FixedVectorType>(colTy);
    Value* rhs = RHS->codegen();
    if (!rhs) return nullptr;
    Value* jVal = Index2->codegen();
    if (!jVal) return nullptr;
    jVal = toI32(jVal);
    Value* col  = Builder->CreateLoad(vecColTy, colPtr, lhsVar->Name + ".col");
    Value* upd  = Builder->CreateInsertElement(col, rhs, jVal, lhsVar->Name + ".upd");
    Builder->CreateStore(upd, colPtr);
    return rhs;
}

// FUNCTION DECLARATION
llvm::Value* PrototypeAST::codegen() {
    using namespace llvm;
    // make return type
        Type* retTy = nullptr;
    if (RetType == "void") {
        retTy = Type::getVoidTy(*Context);
    } else {
        retTy = resolveTypeByName(RetType);
        if (!retTy) { 
            logError(fmt::format("Unknown return type: {}", RetType));
            return nullptr; 
        }
    }

    // make param types
    std::vector<Type*> paramTys;
    paramTys.reserve(Args.size());
    for (auto& [tyName, argName] : Args) {
        Type* ty = resolveTypeByName(tyName);
        if (!ty) { 
            logError(fmt::format("Unknown param type: {}", tyName));
            return nullptr; 
        }
        paramTys.push_back(ty);
    }

    // create function type and function
    FunctionType* FT = FunctionType::get(retTy, paramTys, false);
    Function* F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    // set names for arguments
    unsigned idx = 0;
    for (auto &Arg : F->args()) {
        Arg.setName(Args[idx++].second);
    }
    // return the function
    return F;
}

llvm::Value* FunctionAST::codegen() {
    auto savedIP = Builder->saveIP();

    // ── Entry-point (stage-aware) codegen ─────────────────────────────
    if (Attrs.isEntry) {
        ShaderStage stage = *Attrs.stage;

        auto* i32Ty   = Type::getInt32Ty(*Context);
        auto* f32Ty   = Type::getFloatTy(*Context);
        auto* vec4Ty  = FixedVectorType::get(f32Ty, 4);
        auto* uvec3Ty = FixedVectorType::get(i32Ty, 3);
        auto* ptrTy   = PointerType::getUnqual(*Context);

        // ── Build output struct type ──────────────────────────────────
        StructType* outStructTy = nullptr;
        std::vector<std::string> outVarNames;
        std::vector<Type*>       outVarTypes;

        if (stage == ShaderStage::Vertex) {
            outVarNames.push_back("gl_Position");
            outVarTypes.push_back(vec4Ty);
            for (auto& sv : StageOutputVars) {
                Type* t = resolveTypeByName(sv.typeName);
                if (!t) { logError(fmt::format("Unknown out-var type: {}", sv.typeName)); return nullptr; }
                outVarNames.push_back(sv.name);
                outVarTypes.push_back(t);
            }
            outStructTy = StructType::create(*Context, outVarTypes, "VS_Output");
        } else if (stage == ShaderStage::Fragment) {
            for (auto& sv : StageOutputVars) {
                Type* t = resolveTypeByName(sv.typeName);
                if (!t) { logError(fmt::format("Unknown out-var type: {}", sv.typeName)); return nullptr; }
                outVarNames.push_back(sv.name);
                outVarTypes.push_back(t);
            }
            if (!outVarTypes.empty())
                outStructTy = StructType::create(*Context, outVarTypes, "FS_Output");
        }

        // ── Build parameter list ──────────────────────────────────────
        std::vector<Type*>       paramTys;
        std::vector<std::string> paramNames;

        if (stage == ShaderStage::Vertex) {
            paramTys.push_back(i32Ty);  paramNames.push_back("gl_VertexID");
            paramTys.push_back(i32Ty);  paramNames.push_back("gl_InstanceID");
            paramTys.push_back(ptrTy);  paramNames.push_back("_out");
        } else if (stage == ShaderStage::Fragment) {
            paramTys.push_back(vec4Ty); paramNames.push_back("gl_FragCoord");
            for (auto& sv : StageInputVars) {
                Type* t = resolveTypeByName(sv.typeName);
                if (!t) { logError(fmt::format("Unknown in-var type: {}", sv.typeName)); return nullptr; }
                paramTys.push_back(t);
                paramNames.push_back(sv.name);
            }
            if (outStructTy) { paramTys.push_back(ptrTy); paramNames.push_back("_out"); }
        } else { // Compute
            paramTys.push_back(uvec3Ty); paramNames.push_back("gl_GlobalInvocationID");
            paramTys.push_back(uvec3Ty); paramNames.push_back("gl_LocalInvocationID");
            paramTys.push_back(uvec3Ty); paramNames.push_back("gl_WorkGroupID");
            paramTys.push_back(uvec3Ty); paramNames.push_back("gl_NumWorkgroups");
        }

        // ── Create function ───────────────────────────────────────────
        // Use stable stage-based names so VS+FS modules can be linked without conflicts
        std::string stageFuncName;
        if      (stage == ShaderStage::Vertex)   stageFuncName = "vs_main";
        else if (stage == ShaderStage::Fragment)  stageFuncName = "fs_main";
        else                                      stageFuncName = "cs_main";

        FunctionType* FT = FunctionType::get(Type::getVoidTy(*Context), paramTys, false);
        Function* F = Function::Create(FT, Function::ExternalLinkage, stageFuncName, TheModule.get());
        { unsigned idx = 0; for (auto& a : F->args()) a.setName(paramNames[idx++]); }

        BasicBlock* BB = BasicBlock::Create(*Context, "entry", F);
        Builder->SetInsertPoint(BB);
        NamedValues.clear();

        // Store params as allocas
        for (auto& arg : F->args()) {
            std::string an = std::string(arg.getName());
            AllocaInst* A = CreateEntryBlockAlloca(F, an, arg.getType());
            Builder->CreateStore(&arg, A);
            NamedValues[an] = A;
        }

        // Create output-variable allocas (so body can assign to them)
        if (stage == ShaderStage::Vertex) {
            AllocaInst* A = CreateEntryBlockAlloca(F, "gl_Position", vec4Ty);
            Builder->CreateStore(Constant::getNullValue(vec4Ty), A);
            NamedValues["gl_Position"] = A;
            for (size_t i = 1; i < outVarNames.size(); ++i) {
                AllocaInst* B2 = CreateEntryBlockAlloca(F, outVarNames[i], outVarTypes[i]);
                Builder->CreateStore(Constant::getNullValue(outVarTypes[i]), B2);
                NamedValues[outVarNames[i]] = B2;
            }
        } else if (stage == ShaderStage::Fragment) {
            for (size_t i = 0; i < outVarNames.size(); ++i) {
                AllocaInst* A = CreateEntryBlockAlloca(F, outVarNames[i], outVarTypes[i]);
                Builder->CreateStore(Constant::getNullValue(outVarTypes[i]), A);
                NamedValues[outVarNames[i]] = A;
            }
        }

        // ── Codegen body ──────────────────────────────────────────────
        if (!Body->codegen()) {
            F->eraseFromParent();
            Builder->restoreIP(savedIP);
            return nullptr;
        }

        // ── Epilogue: store outputs into _out struct ──────────────────
        BasicBlock* cur = Builder->GetInsertBlock();
        if (!cur->getTerminator() && outStructTy) {
            auto outIt = NamedValues.find("_out");
            if (outIt != NamedValues.end()) {
                Value* outPtr = Builder->CreateLoad(ptrTy, outIt->second, "out.ptr");
                for (size_t i = 0; i < outVarNames.size(); ++i) {
                    auto varIt = NamedValues.find(outVarNames[i]);
                    if (varIt == NamedValues.end()) continue;
                    Value* loaded = Builder->CreateLoad(outVarTypes[i], varIt->second, outVarNames[i] + ".ld");
                    Value* gep = Builder->CreateStructGEP(outStructTy, outPtr, (unsigned)i);
                    Builder->CreateStore(loaded, gep);
                }
            }
        }

        cur = Builder->GetInsertBlock();
        if (!cur->getTerminator())
            Builder->CreateRetVoid();

        // ── Shader metadata ───────────────────────────────────────────
        const char* stageStr = stage == ShaderStage::Vertex   ? "vertex"
                             : stage == ShaderStage::Fragment  ? "fragment"
                                                               : "compute";
        F->setMetadata("shader.stage",
            MDNode::get(*Context, MDString::get(*Context, stageStr)));

        if (Attrs.workgroupSize.has_value()) {
            auto& ws = *Attrs.workgroupSize;
            Metadata* wsMDs[] = {
                ConstantAsMetadata::get(ConstantInt::get(i32Ty, ws[0])),
                ConstantAsMetadata::get(ConstantInt::get(i32Ty, ws[1])),
                ConstantAsMetadata::get(ConstantInt::get(i32Ty, ws[2]))
            };
            F->setMetadata("shader.workgroup_size", MDNode::get(*Context, wsMDs));
        }

        if (verifyFunction(*F, &errs())) {
            logError(fmt::format("Entry function verification failed for '{}'", Proto->Name));
            F->eraseFromParent();
            Builder->restoreIP(savedIP);
            return nullptr;
        }
        Builder->restoreIP(savedIP);
        return F;
    }

    // ── Regular (non-entry) function ──────────────────────────────────
    Function* F = nullptr;
    if (auto* V = Proto->codegen()) F = cast<Function>(V);
    else return nullptr;

    BasicBlock* BB = BasicBlock::Create(*Context, "entry", F);
    Builder->SetInsertPoint(BB);

    NamedValues.clear();
    for (auto& Arg : F->args()) {
        AllocaInst* A = CreateEntryBlockAlloca(F, std::string(Arg.getName()), Arg.getType());
        Builder->CreateStore(&Arg, A);
        NamedValues[std::string(Arg.getName())] = A;
    }

    Value* RetVal = Body->codegen();
    if (!RetVal) {
        F->eraseFromParent();
        Builder->restoreIP(savedIP);
        return nullptr;
    }

    BasicBlock* CurrentBlock = Builder->GetInsertBlock();
    if (!CurrentBlock->getTerminator()) {
        if (F->getReturnType()->isVoidTy()) {
            Builder->CreateRetVoid();
        } else {
            if (allPathsReturn(Body.get())) {
                Builder->CreateUnreachable();
            } else {
                logError(fmt::format("Missing return terminator in '{}'", Proto->Name));
                F->eraseFromParent();
                Builder->restoreIP(savedIP);
                return nullptr;
            }
        }
    }

    if (verifyFunction(*F, &errs())) {
        logError(fmt::format("Function verification failed for '{}'", Proto->Name));
        F->eraseFromParent();
        Builder->restoreIP(savedIP);
        return nullptr;
    }

    Builder->restoreIP(savedIP);
    return F;
}

// Return statement
llvm::Value* ReturnStmtAST::codegen() {
    Function* F = Builder->GetInsertBlock()->getParent();
    Type* RTy = F->getReturnType();
    
    // Handle void return
    if (RTy->isVoidTy()) {
        if (Expr) {
            logError("Cannot return a value from void function");
            return nullptr;
        }
        return Builder->CreateRetVoid();
    }
    
    // Non-void return must have an expression
    if (!Expr) {
        logError("Non-void function must return a value");
        return nullptr;
    }
    
    Value* V = Expr->codegen();
    if (!V) {
        logError("Return expression codegen failed");
        return nullptr;
    }
    
    // Cast if needed
    if (V->getType() != RTy) {
        V = castScalarTo(V, RTy);
        if (!V) {
            logError("Cannot cast return value to function return type");
            return nullptr;
        }
    }

    return Builder->CreateRet(V);
}

// Break statement
Value* BreakStmtAST::codegen() {
    if (BreakStack.empty()) {
        logError("Break statement outside of loop");
        return nullptr;
    }
    Builder->CreateBr(BreakStack.back());
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

// Continue statement
Value* ContinueStmtAST::codegen() {
    if (ContinueStack.empty()) {
        logError("Continue statement outside of loop");
        return nullptr;
    }
    Builder->CreateBr(ContinueStack.back());
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

// Discard statement
Value* DiscardStmtAST::codegen() {
    Function* discardFn = TheModule->getFunction("__frag_discard");
    if (!discardFn) {
        FunctionType* FT = FunctionType::get(Type::getVoidTy(*Context), {}, false);
        discardFn = Function::Create(FT, Function::ExternalLinkage, "__frag_discard", TheModule.get());
    }
    Builder->CreateCall(discardFn, {});
    Builder->CreateUnreachable();
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

// While statement
Value* WhileExprAST::codegen() {
    Function* F = Builder->GetInsertBlock()->getParent();

    BasicBlock* CondBB  = BasicBlock::Create(*Context, "while.cond", F);
    BasicBlock* BodyBB  = BasicBlock::Create(*Context, "while.body", F);
    BasicBlock* AfterBB = BasicBlock::Create(*Context, "while.end",  F);

    // while jumps to condition first
    Builder->CreateBr(CondBB);

    // condition
    Builder->SetInsertPoint(CondBB);
    Value* C = Condition->codegen();
    if (!C) return nullptr;
    Builder->CreateCondBr(C, BodyBB, AfterBB);

    // body
    Builder->SetInsertPoint(BodyBB);
    BreakStack.push_back(AfterBB);
    ContinueStack.push_back(CondBB);

    if (!Body->codegen()) {
        BreakStack.pop_back();
        ContinueStack.pop_back();
        return nullptr;
    }

    // Only loop back if body did not already terminate (break/return)
    if (!Builder->GetInsertBlock()->getTerminator()) {
        Builder->CreateBr(CondBB);
    }

    BreakStack.pop_back();
    ContinueStack.pop_back();

    // continue after loop
    Builder->SetInsertPoint(AfterBB);
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

// For statement
Value* ForExprAST::codegen() {
    Function* F = Builder->GetInsertBlock()->getParent();

    // init
    if (Init && !Init->codegen()) return nullptr;

    BasicBlock* CondBB  = BasicBlock::Create(*Context, "for.cond", F);
    BasicBlock* BodyBB  = BasicBlock::Create(*Context, "for.body", F);
    BasicBlock* IncBB   = BasicBlock::Create(*Context, "for.inc",  F);
    BasicBlock* AfterBB = BasicBlock::Create(*Context, "for.end",  F);

    // go to condition
    Builder->CreateBr(CondBB);

    // condition
    Builder->SetInsertPoint(CondBB);
    Value* C = Condition ? Condition->codegen() : ConstantInt::getTrue(*Context);
    if (!C) return nullptr;
    Builder->CreateCondBr(C, BodyBB, AfterBB);

    // body
    Builder->SetInsertPoint(BodyBB);
    BreakStack.push_back(AfterBB);
    ContinueStack.push_back(IncBB);

    if (!Body->codegen()) {
        BreakStack.pop_back();
        ContinueStack.pop_back();
        return nullptr;
    }

    // if body didn't terminate -> go to increment
    if (!Builder->GetInsertBlock()->getTerminator()) {
        Builder->CreateBr(IncBB);
    }

    BreakStack.pop_back();
    ContinueStack.pop_back();

    // increment
    Builder->SetInsertPoint(IncBB);
    if (Increment && !Increment->codegen()) return nullptr;

    // inc always goes back to condition (unless increment terminated, which it shouldn't)
    if (!Builder->GetInsertBlock()->getTerminator()) {
        Builder->CreateBr(CondBB);
    }

    // after
    Builder->SetInsertPoint(AfterBB);
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}


// STRUCT DECLARATION

llvm::Value* StructDeclExprAST::codegen() {
    using namespace llvm;
    if (NamedStructTypes.find(Name) != NamedStructTypes.end()) {
        logError(fmt::format("Struct {} already declared", Name));
        return nullptr;
    }

    StructType* ST = StructType::create(*Context, Name);
    std::vector<Type*> elems;
    elems.reserve(Fields.size());
    std::vector<std::string> fieldNames;
    for (auto &f : Fields) {
        Type* t = resolveTypeByName(f.first);
        if (!t) { 
            logError(fmt::format("Unknown field type '{}' in struct {}", f.first, Name));
            return nullptr; 
        }
        elems.push_back(t);
        fieldNames.push_back(f.second);
    }
    ST->setBody(elems, /*isPacked=*/false);
    
    // Register struct with its field names
    StructInfo info;
    info.Type = ST;
    info.FieldNames = std::move(fieldNames);
    NamedStructTypes[Name] = std::move(info);

    return Constant::getNullValue(Type::getInt32Ty(*Context));
}

// Stage variable declaration codegen
Value* StageVarDeclAST::codegen() {
    StageVar sv;
    sv.name = Name;
    sv.typeName = TypeName;
    sv.isInput = isInput;
    sv.binding = binding;
    if (isInput)
        StageInputVars.push_back(sv);
    else
        StageOutputVars.push_back(sv);
    return Constant::getNullValue(Type::getInt32Ty(*Context));
}

// Uniform declaration
Value* UniformDeclExprAST::codegen() {
    using namespace llvm;
    Type* t = resolveTypeByName(TypeName);
    if (!t) {
        logError(fmt::format( "Unknown type for uniform: {}", TypeName));
        return nullptr;
    }

    GlobalVariable* G = TheModule->getGlobalVariable(Name);
    if (!G) {
        auto *init = Constant::getNullValue(t);
        G = new GlobalVariable(
            *TheModule,
            t,
            false,
            GlobalValue::ExternalLinkage,
            init,
            Name
        );
    } else {
        if (G->getValueType() != t) {
            logError(fmt::format( "Uniform re-declared with different type: {}", Name));
            return nullptr;
        }
    }
    return Constant::getNullValue(t);
}

// ArrayInitExprAST codegen

Value* ArrayInitExprAST::codegen() {
    if (Elements.empty()) {
        logError("Array initializer cannot be empty");
        return nullptr;
    }

    // Generate code for all elements
    std::vector<Value*> values;
    values.reserve(Elements.size());

    Type* elemType = nullptr;
    for (auto& elem : Elements) {
        Value* v = elem->codegen();
        if (!v) return nullptr;

        if (!elemType) {
            elemType = v->getType();
        } else if (v->getType() != elemType) {
            logError("Array initializer elements must have the same type");
            return nullptr;
        }
        values.push_back(v);
    }

    // Create array type and a temporary alloca to hold the initialized data
    ArrayType* arrTy = ArrayType::get(elemType, values.size());
    Function* F = Builder->GetInsertBlock()->getParent();
    AllocaInst* tmpAlloca = CreateEntryBlockAlloca(F, "arr.init.tmp", arrTy);

    for (size_t i = 0; i < values.size(); ++i) {
        Value* indices[] = { Builder->getInt32(0), Builder->getInt32((uint32_t)i) };
        Value* elemPtr = Builder->CreateInBoundsGEP(arrTy, tmpAlloca, indices, "init.elem.ptr");
        Builder->CreateStore(values[i], elemPtr);
    }

    return tmpAlloca;
}

// ArrayDeclExprAST codegen
Value* ArrayDeclExprAST::codegen() {
    // Resolve element type
    Type* elemType = resolveTypeByName(ElementType);
    if (!elemType) {
        logError(fmt::format("Unknown type '{}' in array declaration", ElementType));
        return nullptr;
    }

    // Check if array size is valid
    if (Size <= 0) {
        logError("Array size must be positive");
        return nullptr;
    }

    // Create array type
    ArrayType* arrayType = ArrayType::get(elemType, Size);

    // Get current function
    Function* F = Builder->GetInsertBlock()->getParent();
    if (!F) {
        logError("Array declaration must be inside a function");
        return nullptr;
    }

    // Create alloca for the array
    AllocaInst* alloca = CreateEntryBlockAlloca(F, Name, arrayType);

    // Register in symbol table
    NamedValues[Name] = alloca;

    // If there's an initializer, process it
    if (Init) {
        auto* initExpr = dynamic_cast<ArrayInitExprAST*>(Init.get());
        if (!initExpr) {
            logError("Array initializer must be ArrayInitExprAST");
            return nullptr;
        }

        // Check initializer size matches array size
        if (initExpr->Elements.size() != static_cast<size_t>(Size)) {
            logError(fmt::format("Array initializer size ({}) doesn't match array size ({})",
                               initExpr->Elements.size(), Size));
            return nullptr;
        }

        // Store each element
        for (size_t i = 0; i < initExpr->Elements.size(); ++i) {
            Value* elemValue = initExpr->Elements[i]->codegen();
            if (!elemValue) return nullptr;

            // Create GEP to access array element
            Value* indices[] = {
                Builder->getInt32(0),  // Dereference the pointer
                Builder->getInt32(i)   // Index into array
            };
            Value* elemPtr = Builder->CreateInBoundsGEP(
                arrayType,
                alloca,
                indices,
                fmt::format("{}[{}]", Name, i)
            );

            // Store the value
            Builder->CreateStore(elemValue, elemPtr);
        }
    } else {
        // No initializer - zero initialize
        Builder->CreateStore(
            Constant::getNullValue(arrayType),
            alloca
        );
    }

    return alloca;
}

Value* UniformArrayDeclExprAST::codegen() {
    Type* elemType = resolveTypeByName(TypeName);
    if (!elemType) {
        logError(fmt::format("Unknown type for uniform array: {}", TypeName));
        return nullptr;
    }
    // Create array type
    ArrayType* arrayType = ArrayType::get(elemType, Size);
    GlobalVariable* G = TheModule->getGlobalVariable(Name);
    if (!G) {
        auto *init = Constant::getNullValue(arrayType);
        G = new GlobalVariable(
            *TheModule,
            arrayType,
            false,
            GlobalValue::ExternalLinkage,
            init,
            Name
        );
    } else {
        if (G->getValueType() != arrayType) {
            logError(fmt::format("Uniform array re-declared with different type: {}", Name));
            return nullptr;
        }
    }
    UniformArrays[Name] = G;
    return G;
}

// Storage buffer declaration (compute shaders)
// Declares an external global pointer: e.g. @src = external global ptr
Value* StorageBufferDeclAST::codegen() {
    Type* elemTy = resolveTypeByName(ElemType);
    if (!elemTy) {
        logError(fmt::format("Unknown element type for storage buffer: {}", ElemType));
        return nullptr;
    }
    // Declare as an external opaque pointer global.
    // The host sets this pointer before calling cs_invoke().
    auto* ptrTy = PointerType::getUnqual(*Context);
    GlobalVariable* G = TheModule->getGlobalVariable(Name);
    if (!G) {
        G = new GlobalVariable(
            *TheModule,
            ptrTy,
            false,                          // not constant
            GlobalValue::ExternalLinkage,
            nullptr,                        // external — no initializer
            Name
        );
    }
    StorageBufferInfos[Name] = {G, elemTy, isReadOnly};
    return G;
}
