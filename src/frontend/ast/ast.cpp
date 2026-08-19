#include "ast.h"
#include <fmt/core.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include "../../codegen/codegen_state/codegen_state.h"
#include "../../common/error_utils_fmt.h"
#include "../../codegen/helpers/utils.h"
#include "../../codegen/helpers/assignment_helpers.h"
#include "../../codegen/helpers/call_helpers.h"

using namespace llvm;

std::vector<StageVar> StageInputVars;
std::vector<StageVar> StageOutputVars;

// Check if all paths in a function return a value
static bool allPathsReturn(const ExprAST* body) {
    if (!body) return false;

    if (auto* block = llvm::dyn_cast<BlockExprAST>(body)) {
        if (block->Statements.empty()) return false;
        // Only the last statement can guarantee function termination
        return allPathsReturn(block->Statements.back());
    }

    if (llvm::isa<ReturnStmtAST>(body)) {
        return true;
    }

    if (auto* ifExpr = llvm::dyn_cast<IfExprAST>(body)) {
        // Must have else, and both branches must return
        if (!ifExpr->ThenExpr) return false;
        if (!ifExpr->ElseExpr) return false;
        return allPathsReturn(ifExpr->ThenExpr) &&
               allPathsReturn(ifExpr->ElseExpr);
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
        // UNE => NaN counts as true;
        return Builder->CreateFCmpUNE(v, zero, "tobool");
    }
    logError("Cannot convert to bool");
    return nullptr;
}

// Lowers a canonical scalar glsl::Type to its signless LLVM representation.
// Returns nullptr for vectors, matrices, or aggregates, signaling the caller 
// to fall back to the shape-driven legacy lowering path.
static llvm::Type* lowerScalarType(const glsl::Type* t) {
    if (!t) return nullptr;
    switch (t->kind()) {
        case glsl::Type::Kind::Bool:   return llvm::Type::getInt1Ty(*Context);
        case glsl::Type::Kind::Int:
        case glsl::Type::Kind::Uint:   return llvm::Type::getInt32Ty(*Context);
        case glsl::Type::Kind::Float:  return llvm::Type::getFloatTy(*Context);
        case glsl::Type::Kind::Double: return llvm::Type::getDoubleTy(*Context);
        default:                       return nullptr;
    }
}

// Is an integral operand unsigned
static bool isUnsignedOperand(const ExprAST* e) {
    const glsl::Type* t = e ? e->getType() : nullptr;
    if (!t) return false;
    if (t->isUint()) return true;
    if (t->isVector() && t->elementType()->isUint()) return true;
    return false;
}

// Emits an LLVM constant representation of a numeric literal.
Value* NumberExprAST::codegen() {
    const glsl::Type* ty = getType();
    if (ty) {
        if (ty->isIntegral())
            return ConstantInt::get(Type::getInt32Ty(*Context),
                                    static_cast<uint64_t>(Val),
                                    !ty->isUint());
        if (ty->isDouble())
            return ConstantFP::get(Type::getDoubleTy(*Context), Val);
        if (ty->isFloat())
            return ConstantFP::get(Type::getFloatTy(*Context), Val);
    }
    return ConstantFP::get(Type::getFloatTy(*Context), Val);
}

// Emits an elementwise widening conversion for scalar or vector operand pairs
static Value* emitWiden(Value* v, bool zeroExt, llvm::Type* dst) {
    llvm::Type* src = v->getType();
    if (src == dst) return v;
    llvm::Type* se = src->getScalarType();
    llvm::Type* de = dst->getScalarType();
    // int/uint/bool -> float/double
    if (se->isIntegerTy() && de->isFloatingPointTy())
        return zeroExt ? Builder->CreateUIToFP(v, dst, "uitofp")
                       : Builder->CreateSIToFP(v, dst, "sitofp");
    // float -> double  
    if (se->isFloatTy() && de->isDoubleTy())
        return Builder->CreateFPExt(v, dst, "fpext");
    // integer widening (e.g. i1 → i32)
    if (se->isIntegerTy() && de->isIntegerTy() &&
        se->getIntegerBitWidth() < de->getIntegerBitWidth())
        return zeroExt ? Builder->CreateZExt(v, dst, "zext")
                       : Builder->CreateSExt(v, dst, "sext");
    if (!src->isVectorTy())
        if (Value* c = castScalarTo(v, dst)) return c;
    return v;
}

// Evaluates widening conversions for scalars and vectors
Value* ImplicitCastExprAST::codegen() {
    if (!Operand) return nullptr;
    Value* v = Operand->codegen();
    if (!v) return nullptr;

    const glsl::Type* dstTy = getType();
    if (!dstTy) return v;
    const glsl::Type* srcTy = Operand->getType();

    // Implicit conversion of Matrix is not possible
    assert(!dstTy->isMatrix() && "implicit cast to a matrix violates invariant");

    // Vector target
    if (dstTy->isVector()) {
        llvm::Type* elemTy = lowerScalarType(dstTy->elementType());
        if (!elemTy) return v;
        auto* vecTy = FixedVectorType::get(elemTy, dstTy->vectorSize());
        if (v->getType() == vecTy) return v;
        if (v->getType()->isVectorTy()) {
            bool uns = srcTy && srcTy->isVector() && srcTy->elementType()->isUint();
            return emitWiden(v, uns, vecTy);
        }
        bool uns = (srcTy && srcTy->isUint()) || v->getType()->isIntegerTy(1);
        Value* s = emitWiden(v, uns, elemTy);
        return s ? splatScalarToVector(s, vecTy) : v;
    }

    // Scalar target.
    llvm::Type* dst = lowerScalarType(dstTy);
    if (!dst || v->getType() == dst) return v;  // untyped/unhandled or no-op
    bool uns = (srcTy && srcTy->isUint()) || v->getType()->isIntegerTy(1);
    return emitWiden(v, uns, dst);
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
    logErrorAt(loc,fmt::format("Unknown variable or uniform: {}", Name));
    return nullptr;
}

// Unary operator
// Defined below with the other matrix kernels.
static bool isMatrixType(Type* t);
static Value* matNegate(Value* M);

Value* UnaryExprAST::codegen() {
    Value* v = Operand->codegen();
    if (!v) {
        logErrorAt(loc,"Unary codegen: operand is null");
        return nullptr;
    }

    if (Op == TokenKind::Minus) {
        if (v->getType()->isFPOrFPVectorTy()) {
            return Builder->CreateFNeg(v, "neg");
        }
        if (isMatrixType(v->getType())) {
            return matNegate(v);
        }
        if (v->getType()->isIntOrIntVectorTy()) {
            if (v->getType()->isIntegerTy(1)) {
                logErrorAt(loc,"Negation not supported for boolean type");
                return nullptr;
            }
            return Builder->CreateNeg(v, "neg");
        }
        // getStructName() is an unchecked cast<StructType> — printing the type
        // is the only safe way to name an arbitrary operand here.
        std::string ts;
        llvm::raw_string_ostream(ts) << *v->getType();
        logErrorAt(loc,fmt::format("Negation not supported for type: {}", ts));
        return nullptr;
    }

    if (Op == TokenKind::Not) {
        // Logical NOT is bool-only
        if (v->getType()->isIntegerTy(1)) {
            return Builder->CreateNot(v, "not");
        }
        logErrorAt(loc,"Logical NOT (!) requires boolean type");
        return nullptr;
    }

    // bitwise complement — integer scalars/vectors only
    if (Op == TokenKind::BitwiseNot) {
        if (auto* cf = dyn_cast<ConstantFP>(v)) {
            const APFloat& apf = cf->getValueAPF();
            double d = cf->getType()->isDoubleTy()
                         ? apf.convertToDouble()
                         : (double)apf.convertToFloat();
            v = ConstantInt::get(Type::getInt32Ty(*Context), (int64_t)d);
        }
        if (v->getType()->isIntegerTy(1)) {
            logErrorAt(loc,"Bitwise NOT (~) requires an integer type, not bool");
            return nullptr;
        }
        if (v->getType()->isIntOrIntVectorTy()) {
            return Builder->CreateNot(v, "bnot");
        }
        logErrorAt(loc,"Bitwise NOT (~) requires an integer type");
        return nullptr;
    }

    logErrorAt(loc,
               std::string("Unknown unary operator: ") + tokenKindName(Op));
    return nullptr;
}

// A matrix value is the named struct %glsl.matCxR wrapping the column-major
// [Cols x <Rows x float>] array. See matrixTypeFor in utils.h for why the
// wrapper exists.
static bool isMatrixType(Type* t) { return isMatrixTy(t); }

// Matrix binary-op lowering
// mat(CxR) * vecC → vecR.  res = Σ_c column_c * splat(v[c])  (vector FMA per col)
static Value* matTimesVec(Value* M, Value* V) {
    auto* matTy = cast<ArrayType>(M->getType());
    unsigned cols = matTy->getNumElements();
    auto* colTy = cast<FixedVectorType>(matTy->getElementType());
    unsigned rows = colTy->getNumElements();

    Value* acc = ConstantAggregateZero::get(colTy);  // <rows x float>
    for (unsigned c = 0; c < cols; ++c) {
        Value* col = Builder->CreateExtractValue(M, c, "mv.col");
        Value* vc  = Builder->CreateExtractElement(V, Builder->getInt32(c), "mv.vc");
        Value* vcS = Builder->CreateVectorSplat(rows, vc, "mv.splat");
        acc = Builder->CreateFAdd(acc, Builder->CreateFMul(col, vcS, "mv.scaled"),
                                  "mv.acc");
    }
    return acc;  // <rows x float> = vecR
}

// vecR * mat(CxR) → vecC.  res[c] = dot(v, column_c)
static Value* vecTimesMat(Value* V, Value* M) {
    auto* matTy = cast<ArrayType>(M->getType());
    unsigned cols = matTy->getNumElements();
    auto* colTy = cast<FixedVectorType>(matTy->getElementType());
    Type* f = colTy->getElementType();

    Value* res = UndefValue::get(FixedVectorType::get(f, cols));  // <cols x float>
    for (unsigned c = 0; c < cols; ++c) {
        Value* col = Builder->CreateExtractValue(M, c, "vm.col");
        Value* d   = dotFloatVector(*Builder, V, col);
        res = Builder->CreateInsertElement(res, d, Builder->getInt32(c), "vm.ins");
    }
    return res;  // <cols x float> = vecC
}

// mat(CxR) * mat(KxC) → mat(KxR).  result column k = M * (N's column k)
static Value* matTimesMat(Value* M, Value* N) {
    auto* nTy = cast<ArrayType>(N->getType());
    unsigned nCols = nTy->getNumElements();  // K
    auto* mColTy =
        cast<FixedVectorType>(cast<ArrayType>(M->getType())->getElementType());
    unsigned rows = mColTy->getNumElements();  // R
    Type* f = mColTy->getElementType();

    auto* resTy = ArrayType::get(FixedVectorType::get(f, rows), nCols);  // [K x <R x f>]
    Value* res = UndefValue::get(resTy);
    for (unsigned k = 0; k < nCols; ++k) {
        Value* nCol  = Builder->CreateExtractValue(N, k, "mm.ncol");  // <C x float>
        Value* mvCol = matTimesVec(M, nCol);                          // <R x float>
        res = Builder->CreateInsertValue(res, mvCol, k, "mm.col");
    }
    return res;  // [K x <R x float>]
}

// Component-wise mat/scalar operation. Broadcasts the scalar to each column, 
// handles operand order for non-commutative ops (-, /), and auto-casts the 
// scalar (e.g. int/double) to float.
static Value* matScalarOp(TokenKind op, Value* M, Value* s, bool scalarOnLeft) {
    auto* matTy = cast<ArrayType>(M->getType());
    unsigned cols = matTy->getNumElements();
    auto* colTy = cast<FixedVectorType>(matTy->getElementType());
    unsigned rows = colTy->getNumElements();
    if (s->getType() != colTy->getElementType()) {
        s = castScalarTo(s, colTy->getElementType());
        if (!s) return nullptr;
    }
    Value* sV = Builder->CreateVectorSplat(rows, s, "ms.splat");
    Value* res = UndefValue::get(matTy);
    for (unsigned c = 0; c < cols; ++c) {
        Value* col = Builder->CreateExtractValue(M, c, "ms.col");
        Value* a = scalarOnLeft ? sV : col;
        Value* b = scalarOnLeft ? col : sV;
        Value* r = nullptr;
        switch (op) {
            case TokenKind::Multiply: r = Builder->CreateFMul(a, b, "ms.mul"); break;
            case TokenKind::Divide:   r = Builder->CreateFDiv(a, b, "ms.div"); break;
            case TokenKind::Plus:     r = Builder->CreateFAdd(a, b, "ms.add"); break;
            case TokenKind::Minus:    r = Builder->CreateFSub(a, b, "ms.sub"); break;
            default: return nullptr;
        }
        res = Builder->CreateInsertValue(res, r, c, "ms.ins");
    }
    return res;
}

// mat ± mat (same shape): column-wise add/sub.
static Value* matAddSub(TokenKind op, Value* M, Value* N) {
    auto* matTy = cast<ArrayType>(M->getType());
    unsigned cols = matTy->getNumElements();
    Value* res = UndefValue::get(matTy);
    for (unsigned c = 0; c < cols; ++c) {
        Value* a = Builder->CreateExtractValue(M, c, "ma.l");
        Value* b = Builder->CreateExtractValue(N, c, "ma.r");
        Value* r = (op == TokenKind::Plus) ? Builder->CreateFAdd(a, b, "ma.add")
                                           : Builder->CreateFSub(a, b, "ma.sub");
        res = Builder->CreateInsertValue(res, r, c, "ma.ins");
    }
    return res;
}

// Performs element-wise matrix division (M / N) column by column.
static Value* matCompDivide(Value* M, Value* N) {
    auto* matTy = cast<ArrayType>(M->getType());
    unsigned cols = matTy->getNumElements();
    Value* res = UndefValue::get(matTy);
    for (unsigned c = 0; c < cols; ++c) {
        Value* a = Builder->CreateExtractValue(M, c, "md.l");
        Value* b = Builder->CreateExtractValue(N, c, "md.r");
        res = Builder->CreateInsertValue(res, Builder->CreateFDiv(a, b, "md.div"),
                                         c, "md.ins");
    }
    return res;
}

// mat == mat / mat != mat → a single bool. GLSL compares all components, so the
// per-column <R x i1> results are and-reduced serially (same shape as
// dotFloatVector's reduction) rather than via a vector.reduce intrinsic, which
// the RISC-V and packet paths would each have to learn to lower.
static Value* matCompare(TokenKind op, Value* M, Value* N) {
    auto* matTy = cast<ArrayType>(M->getType());
    unsigned cols = matTy->getNumElements();
    unsigned rows = cast<FixedVectorType>(matTy->getElementType())->getNumElements();
    Value* acc = nullptr;
    for (unsigned c = 0; c < cols; ++c) {
        Value* a = Builder->CreateExtractValue(M, c, "mc.l");
        Value* b = Builder->CreateExtractValue(N, c, "mc.r");
        Value* eq = Builder->CreateFCmpOEQ(a, b, "mc.eq");  // <R x i1>
        for (unsigned r = 0; r < rows; ++r) {
            Value* lane = Builder->CreateExtractElement(eq, Builder->getInt32(r),
                                                        "mc.lane");
            acc = acc ? Builder->CreateAnd(acc, lane, "mc.and") : lane;
        }
    }
    return (op == TokenKind::NotEqual) ? Builder->CreateNot(acc, "mc.ne") : acc;
}

// -mat: component-wise negation, column by column.
static Value* matNegate(Value* M) {
    auto shape = matrixShapeOf(M->getType());
    Value* cols = matrixColumns(M);
    auto* arrTy = cast<ArrayType>(cols->getType());
    Value* res = UndefValue::get(arrTy);
    for (unsigned c = 0; c < shape->cols; ++c) {
        Value* a = Builder->CreateExtractValue(cols, c, "mn.col");
        res = Builder->CreateInsertValue(res, Builder->CreateFNeg(a, "mn.neg"), c,
                                         "mn.ins");
    }
    return matrixFromColumns(res, shape->cols, shape->rows);
}

// Dispatch the five sema-validated matrix shapes. Anything else reaching here is
// a sema/codegen split — sema should have rejected it.
// The kernels above all work on the inner column array, so the %glsl.matCxR
// wrapper is peeled here and put back on a matrix-typed result. Keeping the
// peel at this one boundary leaves every kernel untouched by the representation
// change; SROA/instcombine fold the extractvalue/insertvalue pair away.
static Value* codegenMatrixBinary(TokenKind op, Value* L, Value* R,
                                  SourceLocation loc) {
    const auto lShape = matrixShapeOf(L->getType());
    const auto rShape = matrixShapeOf(R->getType());
    const bool lMat = lShape.has_value();
    const bool rMat = rShape.has_value();

    Value* lv = lMat ? matrixColumns(L) : L;
    Value* rv = rMat ? matrixColumns(R) : R;

    // Result shape for the forms that produce a matrix.
    auto wrap = [&](Value* cols, unsigned c, unsigned r) -> Value* {
        return cols ? matrixFromColumns(cols, c, r) : nullptr;
    };

    if (op == TokenKind::Multiply) {
        if (lMat && rMat)
            return wrap(matTimesMat(lv, rv), rShape->cols, lShape->rows);
        if (lMat && R->getType()->isVectorTy()) return matTimesVec(lv, rv);
        if (L->getType()->isVectorTy() && rMat) return vecTimesMat(lv, rv);
        if (lMat && !rMat)
            return wrap(matScalarOp(op, lv, rv, false),
                        lShape->cols, lShape->rows);
        if (rMat && !lMat)
            return wrap(matScalarOp(op, rv, lv, true),
                        rShape->cols, rShape->rows);
    }
    // Dispatch component-wise matrix operations (+, -, /).
    if (op == TokenKind::Plus || op == TokenKind::Minus ||
        op == TokenKind::Divide) {
        if (lMat && rMat && op == TokenKind::Divide)
            return wrap(matCompDivide(lv, rv), lShape->cols, lShape->rows);
        if (lMat && !rMat)
            return wrap(matScalarOp(op, lv, rv, false),
                        lShape->cols, lShape->rows);
        if (rMat && !lMat)
            return wrap(matScalarOp(op, rv, lv, true),
                        rShape->cols, rShape->rows);
    }
    if (op == TokenKind::Plus || op == TokenKind::Minus)
        if (lMat && rMat)
            return wrap(matAddSub(op, lv, rv), lShape->cols, lShape->rows);
    if (op == TokenKind::Equal || op == TokenKind::NotEqual)
        if (lMat && rMat)                       return matCompare(op, lv, rv);
    logErrorAt(loc, "Internal: unsupported matrix operation reached codegen");
    return nullptr;
}

// Binary operator
Value* BinaryExprAST::codegen() {
    // Short-circuit `&&` / `||` is lowered in full here and returns early; it
    // never falls through to the arithmetic path below.
    if (Op == TokenKind::And || Op == TokenKind::Or) {
        Function* F = Builder->GetInsertBlock()->getParent();

        Value* L = LHS->codegen();
        if (!L) return nullptr;

        BasicBlock* LHSBB = Builder->GetInsertBlock();
        if (LHSBB->getTerminator()) {
            logErrorAt(loc, "Internal: logical-operator LHS terminated its block");
            return nullptr;
        }

        Value* Lbool = toBool(L);
        if (!Lbool) return nullptr;  // toBool already diagnosed (e.g. struct operand)

        BasicBlock* RHSBB   = BasicBlock::Create(*Context, "logical.rhs", F);
        BasicBlock* MergeBB = BasicBlock::Create(*Context, "logical.merge", F);

        if (Op == TokenKind::And) {
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
            if (!Rbool) return nullptr;  // toBool already diagnosed
            Builder->CreateBr(MergeBB);

            Builder->SetInsertPoint(MergeBB);
            PHINode* PN = Builder->CreatePHI(Type::getInt1Ty(*Context), 2, "logic.result");

            // The short-circuit edge carries the constant result; it is the only
            // live incoming edge when the RHS block terminates.
            Value* shortVal = (Op == TokenKind::And)
                ? ConstantInt::getFalse(*Context)
                : ConstantInt::getTrue(*Context);

            PN->addIncoming(shortVal, LHSBB);
            PN->addIncoming(Rbool, RHSBBEnd);
            return PN;
        }

        Builder->SetInsertPoint(MergeBB);
        return (Op == TokenKind::And)
            ? ConstantInt::getFalse(*Context)
            : ConstantInt::getTrue(*Context);
    }

    Value* L = LHS->codegen();
    Value* R = RHS->codegen();
    if (!L || !R) return nullptr;

    if (isMatrixType(L->getType()) || isMatrixType(R->getType()))
        return codegenMatrixBinary(Op, L, R, loc);

    if (L->getType() != R->getType()) {
        std::string ls, rs;
        llvm::raw_string_ostream(ls) << *L->getType();
        llvm::raw_string_ostream(rs) << *R->getType();
        logErrorAt(loc, fmt::format("operands of {} have incompatible types "
                                    "'{}' and '{}'", tokenKindName(Op), ls, rs));
        return nullptr;
    }

    Type* opType = L->getType();

    const bool uns = isUnsignedOperand(LHS) || isUnsignedOperand(RHS);

    // arithmetic operators
    if (Op == TokenKind::Plus) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFAdd(L, R, "addtmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateAdd(L, R, "addtmp");
    }

    if (Op == TokenKind::Minus) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFSub(L, R, "subtmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateSub(L, R, "subtmp");
    }

    if (Op == TokenKind::Multiply) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFMul(L, R, "multmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateMul(L, R, "multmp");
    }

    if (Op == TokenKind::Divide) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFDiv(L, R, "divtmp");
        if (opType->isIntOrIntVectorTy())
            return uns ? Builder->CreateUDiv(L, R, "divtmp")
                       : Builder->CreateSDiv(L, R, "divtmp");
    }

    if (Op == TokenKind::Percent) {
        // '%' is integer-only (sema rejects float operands; floats use mod()).
        if (opType->isIntOrIntVectorTy())
            return uns ? Builder->CreateURem(L, R, "remtmp")
                       : Builder->CreateSRem(L, R, "remtmp");
    }

    if (Op == TokenKind::BitwiseAnd || Op == TokenKind::BitwiseOr ||
        Op == TokenKind::BitwiseXor || Op == TokenKind::ShiftLeft ||
        Op == TokenKind::ShiftRight) {
        if (!opType->isIntOrIntVectorTy()) {
            logErrorAt(loc,"Bitwise operators require integer operands");
            return nullptr;
        }
        switch (Op) {
            case TokenKind::BitwiseAnd: return Builder->CreateAnd(L, R, "andtmp");
            case TokenKind::BitwiseOr:  return Builder->CreateOr (L, R, "ortmp");
            case TokenKind::BitwiseXor: return Builder->CreateXor(L, R, "xortmp");
            case TokenKind::ShiftLeft:  return Builder->CreateShl(L, R, "shltmp");
            case TokenKind::ShiftRight:
        
                return isUnsignedOperand(LHS) ? Builder->CreateLShr(L, R, "shrtmp")
                                              : Builder->CreateAShr(L, R, "shrtmp");
            default: break;
        }
    }

    // comparison operators
    if (Op == TokenKind::Less) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOLT(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy())
            return uns ? Builder->CreateICmpULT(L, R, "cmptmp")
                       : Builder->CreateICmpSLT(L, R, "cmptmp");
    }

    if (Op == TokenKind::LessEqual) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOLE(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy())
            return uns ? Builder->CreateICmpULE(L, R, "cmptmp")
                       : Builder->CreateICmpSLE(L, R, "cmptmp");
    }

    if (Op == TokenKind::Greater) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOGT(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy())
            return uns ? Builder->CreateICmpUGT(L, R, "cmptmp")
                       : Builder->CreateICmpSGT(L, R, "cmptmp");
    }

    if (Op == TokenKind::GreaterEqual) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOGE(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy())
            return uns ? Builder->CreateICmpUGE(L, R, "cmptmp")
                       : Builder->CreateICmpSGE(L, R, "cmptmp");
    }

    if (Op == TokenKind::Equal) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpOEQ(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpEQ(L, R, "cmptmp");
    }

    if (Op == TokenKind::NotEqual) {
        if (opType->isFPOrFPVectorTy()) return Builder->CreateFCmpONE(L, R, "cmptmp");
        if (opType->isIntOrIntVectorTy()) return Builder->CreateICmpNE(L, R, "cmptmp");
    }

    logErrorAt(loc,
               std::string("Unsupported binary operator: ") + tokenKindName(Op));
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
    BasicBlock* thenEnd = Builder->GetInsertBlock();

    Builder->SetInsertPoint(ElseBB);
    Value* elseVal = ElseExpr->codegen();
    if (!elseVal) return nullptr;
    BasicBlock* elseEnd = Builder->GetInsertBlock();

    if (thenVal->getType() != elseVal->getType()) {
        auto rank = [](Type* t) -> int {
            if (t->isDoubleTy())     return 3;
            if (t->isFloatTy())      return 2;
            if (t->isIntegerTy(1))   return 0;  // bool is NOT in the numeric ladder
            if (t->isIntegerTy())    return 1;
            return 0;
        };
        Type* common = rank(thenVal->getType()) >= rank(elseVal->getType())
                           ? thenVal->getType()
                           : elseVal->getType();
        if (thenVal->getType() != common) {
            Builder->SetInsertPoint(thenEnd);
            thenVal = castScalarTo(thenVal, common);
        }
        if (elseVal->getType() != common) {
            Builder->SetInsertPoint(elseEnd);
            elseVal = castScalarTo(elseVal, common);
        }
        if (!thenVal || !elseVal) {
            logErrorAt(loc, "Incompatible types in ternary branches");
            return nullptr;
        }
    }

    // Close both arms (now that any casts are in place), then build the PHI.
    Builder->SetInsertPoint(thenEnd);
    Builder->CreateBr(MergeBB);
    Builder->SetInsertPoint(elseEnd);
    Builder->CreateBr(MergeBB);

    Builder->SetInsertPoint(MergeBB);
    PHINode* PN = Builder->CreatePHI(thenVal->getType(), 2, "ternary.result");
    PN->addIncoming(thenVal, thenEnd);
    PN->addIncoming(elseVal, elseEnd);
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
      logErrorAt(loc,"Cannot determine pointed-to type for member access");
      return nullptr;
    }

    if (!pointedTy) {
      logErrorAt(loc,"Cannot determine pointed-to type for member access");
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

  logErrorAt(loc,fmt::format("Member access only supported on structs and vectors, got type with ID {}", objTy->getTypeID()));
  return nullptr;
}

static llvm::Value* lvalueAddr(ExprAST* e, llvm::Type*& outTy);

llvm::Value* MemberAssignmentExprAST::codegen() {
    Type* objTy = nullptr;
    Value* objPtr = lvalueAddr(Object, objTy);
    if (objPtr && objTy->isStructTy()) {
        auto* st = cast<StructType>(objTy);
        std::string sName = st->hasName() ? st->getName().str() : "";
        auto sit = NamedStructTypes.find(sName);
        if (sit == NamedStructTypes.end()) {
            logErrorAt(loc, fmt::format("Struct field names not registered for {}", sName));
            return nullptr;
        }
        const auto& names = sit->second.FieldNames;
        auto fit = std::find(names.begin(), names.end(), Member);
        if (fit == names.end()) {
            logErrorAt(loc, fmt::format("Struct {} has no field '{}'", sName, Member));
            return nullptr;
        }
        unsigned idx = static_cast<unsigned>(std::distance(names.begin(), fit));
        Type* fieldTy = st->getElementType(idx);
        Value* zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
        Value* fidx = ConstantInt::get(Type::getInt32Ty(*Context), idx);
        Value* fptr = Builder->CreateInBoundsGEP(st, objPtr, {zero, fidx}, Member + ".ptr");
        Value* rhs = Init->codegen();
        if (!rhs) return nullptr;
        if (rhs->getType() != fieldTy) {
            if (fieldTy->isVectorTy() && !rhs->getType()->isVectorTy()) {
                Value* sc = castScalarTo(rhs, cast<FixedVectorType>(fieldTy)->getElementType());
                if (!sc) return nullptr;
                rhs = splatScalarToVector(sc, cast<FixedVectorType>(fieldTy));
            } else if (Value* c = castScalarTo(rhs, fieldTy)) {
                rhs = c;
            }
        }
        Builder->CreateStore(rhs, fptr);
        return rhs;
    }

    // MATRIX column swizzle: `mat[col].xy = ...` — mat[col] is a vector column.
    if (auto* matAccess = llvm::dyn_cast<MatrixAccessExprAST>(Object)) {
        MatrixColumnAssigner assigner(matAccess, Member, Init);
        return assigner.codegen();
    }

    // VECTOR swizzle on a plain variable: `pos.xy = ...`
    if (objPtr && objTy->isVectorTy()) {
        VectorSwizzleAssigner assigner(Object, Member, Init);
        return assigner.codegen();
    }

    logErrorAt(loc,"Assignment LHS must be variable (e.g., a.xy = ..., lights[0].prop1 = ...)");
    return nullptr;
}

// Function call or constructor call ast
Value* CallExprAST::codegen() {
    // Each handler reports NotMine / Ok / Failed. Stopping on Failed is what
    // keeps a bad `mat3(1.0, 2.0)` from also being run through the vector,
    // scalar and struct constructors and finally reported as an unknown
    // function — one mistake would otherwise produce two unrelated diagnostics.
    using CtorFn = CtorResult (*)(const CallExprAST*);
    for (CtorFn ctor : {tryCodegenMatrixConstructor, tryCodegenVectorConstructor,
                        tryCodegenScalarConstructor, tryCodegenStructConstructor}) {
        CtorResult r = ctor(this);
        if (r.status == CtorStatus::Ok) return r.value;
        if (r.status == CtorStatus::Failed) return nullptr;
    }

    // Generate arguments once
    std::vector<llvm::Value*> ArgsV;
    ArgsV.reserve(Args.size());
    for (auto& A : Args) {
        llvm::Value* v = A->codegen();
        if (!v) return nullptr;
        ArgsV.push_back(v);
    }

    // Try builtin with generated args. Pass the first arg's GLSL type spelling so
    // sampler/image builtins can dispatch by sampler type (the LLVM value erases
    // 2D/3D/cube/array/image to an opaque ptr), plus its binding slot so the
    // runtime reads the right texture.
    std::string arg0TypeName;
    int arg0Binding = 0;
    if (!Args.empty() && Args[0]->getType()) {
        arg0TypeName = Args[0]->getType()->toString();
        if (auto* var = llvm::dyn_cast<VariableExprAST>(Args[0]))
            if (auto* g = TheModule ? TheModule->getGlobalVariable(var->Name) : nullptr)
                if (auto* md = g->getMetadata("shader.binding"))
                    arg0Binding = (int)llvm::mdconst::extract<llvm::ConstantInt>(
                                      md->getOperand(0))->getSExtValue();
    }
    if (auto* bv = codegenBuiltin(*Builder, TheModule.get(), Callee, ArgsV,
                                  arg0TypeName, arg0Binding))
        return bv;

    // User-defined function
    Function* CalleeF = TheModule->getFunction(Callee);
    if (!CalleeF) {
        logErrorAt(loc,"Unknown function referenced: " + Callee);
        return nullptr;
    }

    if (CalleeF->arg_size() != Args.size()) {
        logErrorAt(loc,fmt::format("Incorrect number of arguments for function {}. Expected {}, got {}",
                           Callee, CalleeF->arg_size(), Args.size()));
        return nullptr;
    }

    auto dirIt = FunctionParamDirs.find(Callee);
    if (dirIt != FunctionParamDirs.end()) {
        const auto& dirs = dirIt->second;
        for (size_t k = 0; k < Args.size() && k < dirs.size(); ++k) {
            if (dirs[k] == 0) continue;  // by value
            auto* var = llvm::dyn_cast<VariableExprAST>(Args[k]);
            if (!var) {
                logErrorAt(loc, fmt::format(
                    "argument {} of '{}' is 'out'/'inout' and must be a variable",
                    k, Callee));
                return nullptr;
            }
            auto vit = NamedValues.find(var->Name);
            if (vit == NamedValues.end()) {
                logErrorAt(loc, fmt::format("Undefined variable '{}'", var->Name));
                return nullptr;
            }
            ArgsV[k] = vit->second;  // pass the alloca pointer
        }
    }

    // Cast arguments to expected types
    unsigned i = 0;
    for (auto& argVal : ArgsV) {
        Type* expectedTy = CalleeF->getFunctionType()->getParamType(i);
        
        if (argVal->getType() != expectedTy) {
            Value* casted = castScalarTo(argVal, expectedTy);
            if (!casted) {
                logErrorAt(loc,fmt::format("Cannot cast argument {} to expected type for function {}", i, Callee));
                return nullptr;
            }
            argVal = casted;
            ArgsV[i] = argVal;
        }
        ++i;
    }

    // A void call must not be given a result name.
    return Builder->CreateCall(
        CalleeF, ArgsV,
        CalleeF->getReturnType()->isVoidTy() ? "" : "calltmp");
}

llvm::Value* IfExprAST::codegen() {
    using namespace llvm;

    Value* condVal = Condition->codegen();
    if (!condVal) return nullptr;

    // The condition is normalized to i1; float and wider integer conditions are
    // compared against zero first.
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

    // THEN 
    Builder->SetInsertPoint(thenBB);
    if (!ThenExpr->codegen()) return nullptr;

    BasicBlock* thenEndBB = Builder->GetInsertBlock();
    bool thenTerminated = (thenEndBB->getTerminator() != nullptr);

    // ELSE 
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

    // `if` is a statement-like expression, so the result is an unused dummy value.
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
            logErrorAt(loc,fmt::format("Undefined variable in assignment: {}", VarName));
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
                    logErrorAt(loc,"Cannot cast scalar to vector element type");
                    return nullptr;
                }
                val = splatScalarToVector(scalar, vecTy);
            } else {
                Value* casted = castScalarTo(val, targetType);
                if (!casted) {
                    logErrorAt(loc,"Cannot cast value to target type");
                    return nullptr;
                }
                val = casted;
            }
        }

        Builder->CreateStore(val, Alloca);
        return val;
    }

    Type* Ty = resolveTypeByName(VarType);
    if (!Ty) {
        logErrorAt(loc,fmt::format("Unknown type in assignment: {}", VarType));
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
            logErrorAt(loc,fmt::format("Type mismatch for variable '{}'", VarName));
            return nullptr;
        }
    }

    if (val->getType() != Ty) {
        if (Ty->isVectorTy()) {
            auto* vecTy = cast<FixedVectorType>(Ty);
            Type* elemTy = vecTy->getElementType();

            if (val->getType()->isVectorTy()) {
                logErrorAt(loc,fmt::format("Vector type mismatch in assignment to {}", VarName));
                return nullptr;
            } else {
                Value* scalar = castScalarTo(val, elemTy);
                if (!scalar) {
                    logErrorAt(loc,"Cannot cast scalar to vector element type");
                    return nullptr;
                }
                val = splatScalarToVector(scalar, vecTy);
            }
        } else if (Ty->isDoubleTy() || Ty->isFloatTy() || Ty->isIntegerTy()) {
            Value* casted = castScalarTo(val, Ty);
            if (!casted) {
                logErrorAt(loc,"Cannot cast initializer to target scalar type");
                return nullptr;
            }
            val = casted;
        } else {
            logErrorAt(loc,"Unsupported target type in assignment");
            return nullptr;
        }
    }

    Builder->CreateStore(val, Alloca);
    return val;
}

static Value* lvalueAddr(ExprAST* e, Type*& outTy) {
    using namespace llvm;
    if (auto* var = dyn_cast<VariableExprAST>(e)) {
        auto it = NamedValues.find(var->Name);
        if (it == NamedValues.end()) return nullptr;
        outTy = it->second->getAllocatedType();
        return it->second;
    }
    if (auto* mem = dyn_cast<MemberAccessExprAST>(e)) {
        Type* baseTy = nullptr;
        Value* basePtr = lvalueAddr(mem->Object, baseTy);
        if (!basePtr || !baseTy->isStructTy()) return nullptr;
        auto* st = cast<StructType>(baseTy);
        std::string sName = st->hasName() ? st->getName().str() : "";
        auto sit = NamedStructTypes.find(sName);
        if (sit == NamedStructTypes.end()) return nullptr;
        const auto& names = sit->second.FieldNames;
        auto fit = std::find(names.begin(), names.end(), mem->Member);
        if (fit == names.end()) return nullptr;
        unsigned idx = static_cast<unsigned>(std::distance(names.begin(), fit));
        Value* zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
        Value* fidx = ConstantInt::get(Type::getInt32Ty(*Context), idx);
        outTy = st->getElementType(idx);
        return Builder->CreateInBoundsGEP(st, basePtr, {zero, fidx},
                                          sName + "." + mem->Member + ".ptr");
    }
    if (auto* macc = dyn_cast<MatrixAccessExprAST>(e)) {
        Type* baseTy = nullptr;
        Value* basePtr = lvalueAddr(macc->Object, baseTy);
        if (!basePtr) return nullptr;
        // A matrix is %glsl.matCxR = { [C x <R x float>] }, so indexing a column
        // needs one more level than a plain array does.
        const bool isMat = isMatrixTy(baseTy);
        Value* zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
        if (macc->Index2) {
            // The only addressable two-index form is an array-of-matrix column,
            // `arr[i][j]` → a pointer to column j of matrix element i. A matrix's
            // own m[i][j] scalar has no stable address (dynamic vector component).
            if (isMat || !baseTy->isArrayTy()) return nullptr;
            auto ms = matrixShapeOf(cast<ArrayType>(baseTy)->getElementType());
            if (!ms) return nullptr;
            Value* iVal = toI32(macc->Index->codegen());
            Value* jVal = toI32(macc->Index2->codegen());
            if (!iVal || !jVal) return nullptr;
            outTy = FixedVectorType::get(Type::getFloatTy(*Context), ms->rows);
            return Builder->CreateInBoundsGEP(baseTy, basePtr,
                                              {zero, iVal, zero, jVal},
                                              "arr.mat.col.ptr");
        }
        if (!isMat && !baseTy->isArrayTy()) return nullptr;
        Value* iVal = toI32(macc->Index->codegen());
        if (!iVal) return nullptr;
        if (isMat) {
            auto* arrTy = matrixArrayTy(baseTy);
            outTy = arrTy->getElementType();
            return Builder->CreateInBoundsGEP(baseTy, basePtr, {zero, zero, iVal},
                                              "mat.col.ptr");
        }
        outTy = cast<ArrayType>(baseTy)->getElementType();
        return Builder->CreateInBoundsGEP(baseTy, basePtr, {zero, iVal},
                                          "elem.ptr");
    }
    return nullptr;
}

Value *MatrixAccessExprAST::codegen() {
    // Local variable array or struct array field: `a[i]`, `s.a[i]`, `m[i][j]`.
    Type* baseTy = nullptr;
    std::string baseName = "arr";
    if (Value* basePtr = lvalueAddr(Object, baseTy)) {
        // Object is itself an addressable column vector (arr[i][j] of an
        // array-of-matrix): a trailing [k] selects a component. `arr[i][j][k]`
        // parses as MatrixAccess(MatrixAccess(arr,i,j), k).
        if (baseTy->isVectorTy() && Index && !Index2) {
            Value* v = Builder->CreateLoad(baseTy, basePtr, baseName + ".col.ld");
            Value* kVal = toI32(Index->codegen());
            if (!kVal) return nullptr;
            return Builder->CreateExtractElement(v, kVal, baseName + ".elem");
        }
        // A matrix is %glsl.matCxR = { [C x <R x float>] }, so reaching a column
        // costs one more GEP index than a plain array element does.
        const bool isMat = isMatrixTy(baseTy);
        if (!isMat && !baseTy->isArrayTy()) {
            logErrorAt(loc, "Matrix/array access only supported on array types");
            return nullptr;
        }
        auto* arrTy  = isMat ? matrixArrayTy(baseTy) : cast<ArrayType>(baseTy);
        Type* elemTy = arrTy->getElementType();
        if (!Index) {
            logErrorAt(loc, "Array/matrix access requires first index");
            return nullptr;
        }
        Value* iVal = Index->codegen();
        if (!iVal) return nullptr;
        iVal = toI32(iVal);
        if (!iVal) { logErrorAt(loc, "First index is not convertible to i32"); return nullptr; }

        Value* zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
        llvm::SmallVector<Value*, 3> idx{zero};
        if (isMat) idx.push_back(zero);
        idx.push_back(iVal);
        Value* elemPtr = Builder->CreateInBoundsGEP(baseTy, basePtr, idx,
                                                    baseName + ".elem.ptr");
        if (!Index2) {
            return Builder->CreateLoad(elemTy, elemPtr,
                                       baseName + (elemTy->isVectorTy() ? ".col" : ".elem"));
        }
        // arr[i][j] where the array element is itself a matrix (`mat3 arr[N]`):
        // elemPtr points at that matrix, so index into its wrapper for column j.
        if (auto ms = matrixShapeOf(elemTy)) {
            Value* jVal = toI32(Index2->codegen());
            if (!jVal) { logErrorAt(this->loc, "Second index is not convertible to i32"); return nullptr; }
            Value* colPtr = Builder->CreateInBoundsGEP(
                elemTy, elemPtr, {zero, zero, jVal}, baseName + ".mat.col.ptr");
            auto* colTy = FixedVectorType::get(Type::getFloatTy(*Context), ms->rows);
            return Builder->CreateLoad(colTy, colPtr, baseName + ".mat.col");
        }
        if (!elemTy->isVectorTy()) {
            logErrorAt(loc, "Second index is only valid for matrix (array-of-vector)");
            return nullptr;
        }
        auto* colTy = cast<FixedVectorType>(elemTy);
        Value* col  = Builder->CreateLoad(colTy, elemPtr, baseName + ".col");
        Value* jVal = Index2->codegen();
        if (!jVal) return nullptr;
        jVal = toI32(jVal);
        if (!jVal) { logErrorAt(this->loc, "Second index is not convertible to i32"); return nullptr; }
        return Builder->CreateExtractElement(col, jVal, "m.elem");
    }

    // SSBO / uniform array — these require a plain named variable.
    auto *lhsVar = llvm::dyn_cast<VariableExprAST>(Object);
    if (!lhsVar) {
        logErrorAt(loc,"Matrix/array access LHS must be a variable (e.g., M[i] or M[i][j])");
        return nullptr;
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
        if (!iVal) { logErrorAt(loc,"Storage buffer index not convertible to i32"); return nullptr; }
        // GEP into the buffer: getelementptr elemTy, ptr %p, i32 idx
        Value* elemPtr = Builder->CreateInBoundsGEP(
            info.elemTy, bufPtr, {iVal}, lhsVar->Name + ".elem.ptr");
        return Builder->CreateLoad(info.elemTy, elemPtr, lhsVar->Name + ".elem");
    }

    // UniformArrays is populated only by UniformArrayDeclExprAST, so a plain
    // `uniform mat4 M` never appears there — fall back to the module global
    // exactly as VariableExprAST::codegen does.
    GlobalVariable* gv = nullptr;
    auto uit = UniformArrays.find(lhsVar->Name);
    if (uit != UniformArrays.end()) {
        gv = uit->second;
    } else if (TheModule) {
        gv = TheModule->getGlobalVariable(lhsVar->Name);
    }
    if (!gv) {
        logErrorAt(loc,fmt::format("Unknown variable '{}' in matrix/array access", lhsVar->Name));
        return nullptr;
    }

    Type *uBaseTy = gv->getValueType();

    // Either `uniform mat4 M` (%glsl.mat4x4 = { [4 x <4 x float>] }) or a real
    // uniform array like `uniform vec4 M[4]`. The matrix needs one more GEP
    // index to reach a column.
    const bool uIsMat = isMatrixTy(uBaseTy);
    if (!uIsMat && !uBaseTy->isArrayTy()) {
        logErrorAt(loc,fmt::format("Uniform '{}' is not an array or matrix type", lhsVar->Name));
        return nullptr;
    }

    auto *arrTy  = uIsMat ? matrixArrayTy(uBaseTy) : cast<ArrayType>(uBaseTy);
    Type *elemTy = arrTy->getElementType();

    if (!Index) {
        logErrorAt(loc,fmt::format("Uniform array '{}' requires an index", lhsVar->Name));
        return nullptr;
    }

    Value *iVal = Index->codegen();
    if (!iVal) return nullptr;
    iVal = toI32(iVal);
    if (!iVal) {
        logErrorAt(loc,"First index is not convertible to i32 for uniform array");
        return nullptr;
    }

    Value *zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
    llvm::SmallVector<Value*, 3> uIdx{zero};
    if (uIsMat) uIdx.push_back(zero);
    uIdx.push_back(iVal);
    Value *elemPtr = Builder->CreateInBoundsGEP(
        uBaseTy, gv, uIdx, lhsVar->Name + ".u.elem.ptr"
    );

    if (!Index2) {
        if (elemTy->isVectorTy()) {
            return Builder->CreateLoad(elemTy, elemPtr, lhsVar->Name + ".u.col");
        }
        return Builder->CreateLoad(elemTy, elemPtr, lhsVar->Name + ".u.elem");
    }

    // Uniform matrix = uniform array-of-vector (e.g., uniform mat4x4 in Mat[4])
    if (!elemTy->isVectorTy()) {
        logErrorAt(loc,"Second index is only valid for matrix-like uniform (array-of-vector)");
        return nullptr;
    }

    auto *colTy  = cast<FixedVectorType>(elemTy);
    Value *col   = Builder->CreateLoad(colTy, elemPtr, lhsVar->Name + ".u.col");

    Value *jVal = Index2->codegen();
    if (!jVal) return nullptr;
    jVal = toI32(jVal);
    if (!jVal) {
        logErrorAt(this->loc,"Second index is not convertible to i32 for uniform matrix");
        return nullptr;
    }

    return Builder->CreateExtractElement(col, jVal, "u.m.elem");
}

// Matrix / storage-buffer element assignment: a[i] = val  or  a[i][j] = val
Value* MatrixAssignmentExprAST::codegen() {
    // Storage buffer write: dst[idx] = val. Requires a plain named variable.
    auto* lhsVar = llvm::dyn_cast<VariableExprAST>(Object);
    auto sit = lhsVar ? StorageBufferInfos.find(lhsVar->Name)
                      : StorageBufferInfos.end();
    if (lhsVar && sit != StorageBufferInfos.end()) {
        auto& info = sit->second;
        auto* ptrTy = PointerType::getUnqual(*Context);
        auto* wrPtrLoad = Builder->CreateLoad(ptrTy, info.gv, lhsVar->Name + ".ptr");
        wrPtrLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                               llvm::MDNode::get(*Context, {}));
        Value* bufPtr = wrPtrLoad;
        Value* iVal = Index->codegen();
        if (!iVal) return nullptr;
        iVal = toI32(iVal);
        if (!iVal) { logErrorAt(loc,"Storage buffer index not convertible to i32"); return nullptr; }
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

    // Local array / struct array field write: `arr[i] = v`, `s.a[i] = v`.
    Type* matTy = nullptr;
    std::string baseName = "arr";
    Value* basePtr = lvalueAddr(Object, matTy);
    if (!basePtr) {
        logErrorAt(loc, "Array assignment LHS must be a named variable or struct field");
        return nullptr;
    }
    Value* zero = ConstantInt::get(Type::getInt32Ty(*Context), 0);
    // Object is an addressable column vector (arr[i][j] of an array-of-matrix):
    // `arr[i][j][k] = scalar` updates component k. `arr[i][j][k]` parses as
    // MatrixAssignment(Object = MatrixAccess(arr,i,j), Index = k).
    if (matTy->isVectorTy() && Index && !Index2) {
        Value* kVal = toI32(Index->codegen());
        if (!kVal) return nullptr;
        Value* rhs = RHS->codegen();
        if (!rhs) return nullptr;
        Value* v   = Builder->CreateLoad(matTy, basePtr, baseName + ".col.ld");
        Value* upd = Builder->CreateInsertElement(v, rhs, kVal, baseName + ".upd");
        Builder->CreateStore(upd, basePtr);
        return rhs;
    }
    // Matrix (%glsl.matCxR = { [C x <R x float>] }) or plain array; the matrix
    // takes one more GEP index to reach a column.
    const bool isMat = isMatrixTy(matTy);
    if (!isMat && !matTy->isArrayTy()) {
        logErrorAt(loc, fmt::format("'{}' is not an array or matrix type", baseName));
        return nullptr;
    }
    auto* arrTy = isMat ? matrixArrayTy(matTy) : cast<ArrayType>(matTy);
    Type* colTy = arrTy->getElementType();
    Value* iVal = Index->codegen();
    if (!iVal) return nullptr;
    iVal = toI32(iVal);
    // Array-of-matrix column store: `arr[i][j] = col`. The array element (colTy)
    // is a matrix, so the second index addresses its column.
    if (!isMat && Index2) {
        if (auto ms = matrixShapeOf(colTy)) {
            (void)ms;
            Value* jVal = toI32(Index2->codegen());
            if (!jVal) return nullptr;
            Value* mcolPtr = Builder->CreateInBoundsGEP(
                matTy, basePtr, {zero, iVal, zero, jVal}, baseName + ".mat.col.ptr");
            Value* rhs = RHS->codegen();
            if (!rhs) return nullptr;
            Builder->CreateStore(rhs, mcolPtr);
            return rhs;
        }
    }
    llvm::SmallVector<Value*, 3> aIdx{zero};
    if (isMat) aIdx.push_back(zero);
    aIdx.push_back(iVal);
    Value* colPtr = Builder->CreateInBoundsGEP(matTy, basePtr, aIdx,
                                               baseName + ".col.ptr");
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
    Value* col  = Builder->CreateLoad(vecColTy, colPtr, baseName + ".col");
    Value* upd  = Builder->CreateInsertElement(col, rhs, jVal, baseName + ".upd");
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
            logErrorAt(loc,fmt::format("Unknown return type: {}", RetType));
            return nullptr; 
        }
    }

    // make param types. `out`/`inout` params are passed by reference: their
    // LLVM type is an opaque pointer to the caller's storage.
    std::vector<Type*> paramTys;
    paramTys.reserve(Args.size());
    for (size_t i = 0; i < Args.size(); ++i) {
        uint8_t dir = i < ArgDir.size() ? ArgDir[i] : 0;
        if (dir != 0) {
            // Validate the (pointee) type still resolves, then pass a pointer.
            if (!resolveTypeByName(Args[i].first)) {
                logErrorAt(loc, fmt::format("Unknown param type: {}", Args[i].first));
                return nullptr;
            }
            paramTys.push_back(PointerType::getUnqual(*Context));
            continue;
        }
        Type* ty = resolveTypeByName(Args[i].first);
        if (!ty) {
            logErrorAt(loc,fmt::format("Unknown param type: {}", Args[i].first));
            return nullptr;
        }
        paramTys.push_back(ty);
    }
    if (!ArgDir.empty())
        FunctionParamDirs[Name] = ArgDir;

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
                if (!t) { logErrorAt(loc,fmt::format("Unknown out-var type: {}", sv.typeName)); return nullptr; }
                outVarNames.push_back(sv.name);
                outVarTypes.push_back(t);
            }
            outStructTy = StructType::create(*Context, outVarTypes, "VS_Output");
        } else if (stage == ShaderStage::Fragment) {
            for (auto& sv : StageOutputVars) {
                Type* t = resolveTypeByName(sv.typeName);
                if (!t) { logErrorAt(loc,fmt::format("Unknown out-var type: {}", sv.typeName)); return nullptr; }
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
            for (auto& sv : StageInputVars) {
                Type* t = resolveTypeByName(sv.typeName);
                if (!t) { logErrorAt(loc,fmt::format("Unknown in-var type: {}", sv.typeName)); return nullptr; }
                paramTys.push_back(t);
                paramNames.push_back(sv.name);
            }
            paramTys.push_back(ptrTy);  paramNames.push_back("_out");
        } else if (stage == ShaderStage::Fragment) {
            paramTys.push_back(vec4Ty); paramNames.push_back("gl_FragCoord");
            for (auto& sv : StageInputVars) {
                Type* t = resolveTypeByName(sv.typeName);
                if (!t) { logErrorAt(loc,fmt::format("Unknown in-var type: {}", sv.typeName)); return nullptr; }
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

        if (!Body->codegen()) {
            F->eraseFromParent();
            Builder->restoreIP(savedIP);
            return nullptr;
        }

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

        const char* stageStr = stage == ShaderStage::Vertex   ? "vertex"
                             : stage == ShaderStage::Fragment  ? "fragment"
                                                               : "compute";
        F->setMetadata("shader.stage",
            MDNode::get(*Context, MDString::get(*Context, stageStr)));

        {
            std::vector<Metadata*> ioLocs;
            auto addLocs = [&](const std::vector<StageVar>& vars) {
                for (auto& sv : vars) {
                    if (sv.location < 0) continue;
                    Metadata* pair[] = {
                        MDString::get(*Context, sv.name),
                        ConstantAsMetadata::get(
                            ConstantInt::get(i32Ty, sv.location))};
                    ioLocs.push_back(MDNode::get(*Context, pair));
                }
            };
            addLocs(StageInputVars);
            addLocs(StageOutputVars);
            if (!ioLocs.empty())
                F->setMetadata("shader.io.location",
                               MDNode::get(*Context, ioLocs));
        }

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
            logErrorAt(loc,fmt::format("Entry function verification failed for '{}'", Proto->Name));
            F->eraseFromParent();
            Builder->restoreIP(savedIP);
            return nullptr;
        }
        Builder->restoreIP(savedIP);
        return F;
    }

    Function* F = nullptr;
    if (auto* V = Proto->codegen()) F = cast<Function>(V);
    else return nullptr;

    BasicBlock* BB = BasicBlock::Create(*Context, "entry", F);
    Builder->SetInsertPoint(BB);

    NamedValues.clear();
    const std::vector<uint8_t>& dirs = Proto->ArgDir;
    std::vector<std::pair<AllocaInst*, Argument*>> copyOuts;
    {
        unsigned i = 0;
        for (auto& Arg : F->args()) {
            uint8_t dir = i < dirs.size() ? dirs[i] : 0;
            std::string an = std::string(Arg.getName());
            if (dir != 0) {
                // Pointee (value) type from the declaration spelling.
                Type* valTy = resolveTypeByName(Proto->Args[i].first);
                AllocaInst* A = CreateEntryBlockAlloca(F, an, valTy);
                if (dir == 2)  // inout: copy in the caller's current value
                    Builder->CreateStore(Builder->CreateLoad(valTy, &Arg, an + ".in"), A);
                NamedValues[an] = A;
                copyOuts.emplace_back(A, &Arg);
            } else {
                AllocaInst* A = CreateEntryBlockAlloca(F, an, Arg.getType());
                Builder->CreateStore(&Arg, A);
                NamedValues[an] = A;
            }
            ++i;
        }
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
            if (allPathsReturn(Body)) {
                Builder->CreateUnreachable();
            } else {
                logErrorAt(loc,fmt::format("Missing return terminator in '{}'", Proto->Name));
                F->eraseFromParent();
                Builder->restoreIP(savedIP);
                return nullptr;
            }
        }
    }

    if (!copyOuts.empty()) {
        for (auto& bb : *F) {
            auto* ret = llvm::dyn_cast_or_null<ReturnInst>(bb.getTerminator());
            if (!ret) continue;
            Builder->SetInsertPoint(ret);
            for (auto& [a, p] : copyOuts)
                Builder->CreateStore(
                    Builder->CreateLoad(a->getAllocatedType(), a, "co.ld"), p);
        }
    }

    if (verifyFunction(*F, &errs())) {
        logErrorAt(loc,fmt::format("Function verification failed for '{}'", Proto->Name));
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
            logErrorAt(loc,"Cannot return a value from void function");
            return nullptr;
        }
        return Builder->CreateRetVoid();
    }
    
    // Non-void return must have an expression
    if (!Expr) {
        logErrorAt(loc,"Non-void function must return a value");
        return nullptr;
    }
    
    Value* V = Expr->codegen();
    if (!V) {
        logErrorAt(loc,"Return expression codegen failed");
        return nullptr;
    }
    
    // Cast if needed
    if (V->getType() != RTy) {
        V = castScalarTo(V, RTy);
        if (!V) {
            logErrorAt(loc,"Cannot cast return value to function return type");
            return nullptr;
        }
    }

    return Builder->CreateRet(V);
}

// Break statement
Value* BreakStmtAST::codegen() {
    if (BreakStack.empty()) {
        logErrorAt(loc,"Break statement outside of loop");
        return nullptr;
    }
    Builder->CreateBr(BreakStack.back());
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

// Continue statement
Value* ContinueStmtAST::codegen() {
    if (ContinueStack.empty()) {
        logErrorAt(loc,"Continue statement outside of loop");
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

    BasicBlock* CondBB  = BasicBlock::Create(*Context, "while.cond",  F);
    BasicBlock* BodyBB  = BasicBlock::Create(*Context, "while.body",  F);
    BasicBlock* LatchBB = BasicBlock::Create(*Context, "while.latch", F);
    BasicBlock* AfterBB = BasicBlock::Create(*Context, "while.end",   F);

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
    ContinueStack.push_back(LatchBB);  // `continue` re-tests via the latch

    if (!Body->codegen()) {
        BreakStack.pop_back();
        ContinueStack.pop_back();
        return nullptr;
    }

    // Only fall through to the latch if the body did not already terminate
    // (break/return).
    if (!Builder->GetInsertBlock()->getTerminator()) {
        Builder->CreateBr(LatchBB);
    }

    BreakStack.pop_back();
    ContinueStack.pop_back();

    // latch: unconditional back-edge to the condition
    Builder->SetInsertPoint(LatchBB);
    Builder->CreateBr(CondBB);

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

    // the increment block branches back to the condition unless it terminated
    if (!Builder->GetInsertBlock()->getTerminator()) {
        Builder->CreateBr(CondBB);
    }

    // after
    Builder->SetInsertPoint(AfterBB);
    return ConstantFP::get(Type::getFloatTy(*Context), 0.0f);
}

// Handle non-body forward declaration tracking for structs to bypass circular tracking loops
void StructDeclExprAST::predeclare() {
    using namespace llvm;
    if (NamedStructTypes.find(Name) != NamedStructTypes.end()) return;
    StructType* ST = StructType::create(*Context, Name);
    StructInfo info;
    info.Type = ST;
    NamedStructTypes[Name] = std::move(info);
}

llvm::Value* StructDeclExprAST::codegen() {
    using namespace llvm;
    auto it = NamedStructTypes.find(Name);
    StructType* ST = nullptr;
    if (it != NamedStructTypes.end()) {
        if (it->second.Type->isOpaque()) {
            ST = it->second.Type;
        } else {
            logErrorAt(loc,
                       fmt::format("Struct {} already declared", Name));
            return nullptr;
        }
    } else {
        ST = StructType::create(*Context, Name);
    }

    std::vector<Type*> elems;
    elems.reserve(Fields.size());
    std::vector<std::string> fieldNames;
    for (auto &f : Fields) {
        Type* t = resolveTypeByName(f.first);
        if (!t) { 
            logErrorAt(loc,fmt::format("Unknown field type '{}' in struct {}", f.first, Name));
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
    sv.location = location;
    if (isInput)
        StageInputVars.push_back(sv);
    else
        StageOutputVars.push_back(sv);
    return Constant::getNullValue(Type::getInt32Ty(*Context));
}

// Uniform declaration
// Neutral sampler descriptor for the SPIR-V backend, decoupled from spirv.h.
// Packs dimCode (0=2D,1=3D,2=Cube,3=Buffer) | arrayed<<4 | isImage<<5, straight
// from builtin_types.def so there is one source of truth. -1 = not a sampler.
static int samplerMetaCode(const std::string& spelling) {
#define DIMCODE_Dim2D 0
#define DIMCODE_Dim3D 1
#define DIMCODE_DimCube 2
#define DIMCODE_DimBuffer 3
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) \
    if (spelling == Spelling)                                      \
        return DIMCODE_##Dim | ((Arrayed) << 4) | ((IsImage) << 5);
#include "builtin_types.def"
#undef DIMCODE_Dim2D
#undef DIMCODE_Dim3D
#undef DIMCODE_DimCube
#undef DIMCODE_DimBuffer
    return -1;
}

Value* UniformDeclExprAST::codegen() {
    using namespace llvm;
    Type* t = resolveTypeByName(TypeName);
    if (!t) {
        logErrorAt(loc,fmt::format( "Unknown type for uniform: {}", TypeName));
        return nullptr;
    }

    GlobalVariable* G = TheModule->getGlobalVariable(Name);
    if (!G) {
        auto *init = Constant::getNullValue(t);
        G = new GlobalVariable(
            *TheModule,
            t,
            false,
            GlobalValue::LinkOnceODRLinkage,
            init,
            Name
        );
    } else {
        if (G->getValueType() != t) {
            logErrorAt(loc,fmt::format( "Uniform re-declared with different type: {}", Name));
            return nullptr;
        }
    }
    if (binding >= 0)
        G->setMetadata("shader.binding",
            MDNode::get(*Context, ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(*Context), binding))));
    // Record the sampler/image dimensionality so the SPIR-V backend can emit the
    // right OpTypeImage — the LLVM type is an opaque ptr that erases 2D/3D/cube/
    // array/image. (RISC-V dispatch reads the AST sampler type directly instead.)
    if (int sc = samplerMetaCode(TypeName); sc >= 0)
        G->setMetadata("shader.sampler.kind",
            MDNode::get(*Context, ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(*Context), sc))));
    return Constant::getNullValue(t);
}

// ArrayInitExprAST codegen

Value* ArrayInitExprAST::codegen() {
    if (Elements.empty()) {
        logErrorAt(loc,"Array initializer cannot be empty");
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
            logErrorAt(loc,"Array initializer elements must have the same type");
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
        logErrorAt(loc,fmt::format("Unknown type '{}' in array declaration", ElementType));
        return nullptr;
    }

    // Check if array size is valid
    if (Size <= 0) {
        logErrorAt(loc,"Array size must be positive");
        return nullptr;
    }

    // Create array type
    ArrayType* arrayType = ArrayType::get(elemType, Size);

    // Get current function
    Function* F = Builder->GetInsertBlock()->getParent();
    if (!F) {
        logErrorAt(loc,"Array declaration must be inside a function");
        return nullptr;
    }

    // Create alloca for the array
    AllocaInst* alloca = CreateEntryBlockAlloca(F, Name, arrayType);

    // Register in symbol table
    NamedValues[Name] = alloca;

    // If there's an initializer, process it
    if (Init) {
        auto* initExpr = llvm::dyn_cast<ArrayInitExprAST>(Init);
        if (!initExpr) {
            logErrorAt(loc,"Array initializer must be ArrayInitExprAST");
            return nullptr;
        }

        // Check initializer size matches array size
        if (initExpr->Elements.size() != static_cast<size_t>(Size)) {
            logErrorAt(loc,fmt::format("Array initializer size ({}) doesn't match array size ({})",
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

// Declare global uniform structures interfaces for explicit array structures layout parameters
Value* UniformArrayDeclExprAST::codegen() {
    Type* elemType = resolveTypeByName(TypeName);
    if (!elemType) {
        logErrorAt(loc,fmt::format("Unknown type for uniform array: {}", TypeName));
        return nullptr;
    }
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
            logErrorAt(loc,fmt::format("Uniform array re-declared with different type: {}", Name));
            return nullptr;
        }
    }
    if (binding >= 0)
        G->setMetadata("shader.binding",
            MDNode::get(*Context, ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(*Context), binding))));
    UniformArrays[Name] = G;
    return G;
}

// Register explicit external pointer boundaries layouts for SSBO definitions structures
Value* StorageBufferDeclAST::codegen() {
    Type* elemTy = resolveTypeByName(ElemType);
    if (!elemTy) {
        logErrorAt(loc,fmt::format("Unknown element type for storage buffer: {}", ElemType));
        return nullptr;
    }
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
    if (binding >= 0)
        G->setMetadata("shader.binding",
            MDNode::get(*Context, ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(*Context), binding))));
    StorageBufferInfos[Name] = {G, elemTy, isReadOnly};
    return G;
}

// Increment / decrement (++x, x++, --x, x--):
Value* IncrDecrExprAST::codegen() {
    AllocaInst* varAlloca = nullptr;
    Value* oldVal = nullptr;
    if (auto* var = llvm::dyn_cast<VariableExprAST>(Target)) {
        auto it = NamedValues.find(var->Name);
        if (it == NamedValues.end()) {
            logErrorAt(loc,
                       fmt::format("Undefined variable in postfix expression: {}",
                                   var->Name));
            return nullptr;
        }
        varAlloca = it->second;
        oldVal = Builder->CreateLoad(varAlloca->getAllocatedType(), varAlloca, var->Name + ".old");
    } else {
        oldVal = Target->codegen();  // MemberAccess / MatrixAccess load
    }
    if (!oldVal) return nullptr;

    Type* ty = oldVal->getType();
    Value* newVal = nullptr;
    if (auto shape = matrixShapeOf(ty)) {
        Value* cols = matrixColumns(oldVal);
        auto* arrTy = cast<ArrayType>(cols->getType());
        auto* colTy = cast<FixedVectorType>(arrTy->getElementType());
        Value* onesCol = ConstantFP::get(colTy, 1.0);  // splatted <R x float> 1.0
        Value* onesArr = UndefValue::get(arrTy);
        for (unsigned c = 0; c < shape->cols; ++c)
            onesArr = Builder->CreateInsertValue(onesArr, onesCol, c);
        Value* resCols = matAddSub(
            isDecrement ? TokenKind::Minus : TokenKind::Plus, cols, onesArr);
        newVal = matrixFromColumns(resCols, shape->cols, shape->rows);
    } else {
        Value* one = nullptr;
        if (ty->isIntegerTy()) {
            one = ConstantInt::get(ty, 1);
        } else if (ty->isFloatingPointTy()) {
            one = ConstantFP::get(ty, 1.0);
        } else {
            logErrorAt(loc,
                       "Postfix ++/-- only supported on int / float scalars or matrices");
            return nullptr;
        }
        newVal = ty->isIntegerTy()
            ? (isDecrement ? Builder->CreateSub(oldVal, one, "dec")
                           : Builder->CreateAdd(oldVal, one, "inc"))
            : (isDecrement ? Builder->CreateFSub(oldVal, one, "dec")
                           : Builder->CreateFAdd(oldVal, one, "inc"));
    }

    if (varAlloca) {
        Builder->CreateStore(newVal, varAlloca);
    } else if (auto* mem = llvm::dyn_cast<MemberAccessExprAST>(Target)) {
        RawValueExprAST raw(newVal);
        MemberAssignmentExprAST store(mem->Object, mem->Member, &raw);
        store.loc = loc;
        if (!store.codegen()) return nullptr;
    } else if (auto* macc = llvm::dyn_cast<MatrixAccessExprAST>(Target)) {
        RawValueExprAST raw(newVal);
        MatrixAssignmentExprAST store(macc->Object, macc->Index, macc->Index2,
                                      &raw);
        store.loc = loc;
        if (!store.codegen()) return nullptr;
    } else {
        logErrorAt(loc,
                   "++/-- target must be a variable, member, or index");
        return nullptr;
    }
    return isPrefix ? newVal : oldVal;
}
