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
        // UNE => NaN counts as true (common “nonzero” behavior); use ONE if you want NaN->false
        return Builder->CreateFCmpUNE(v, zero, "tobool");
    }

    // toBool is a free helper with no AST node in scope — left unlocated.
    logError("Cannot convert to bool");
    return nullptr;
}

// Lower a *scalar* glsl::Type to its LLVM representation. Keyed on the canonical
// type the analyzer stamped, so codegen no longer has to assume float. Returns
// nullptr for types this dispatch doesn't lower directly (vectors, matrices,
// aggregates) — callers fall back to the legacy, value-shape-driven path.
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

// Is an integral operand unsigned? Signedness lives in the *operation* (i32 is
// signless in LLVM), so it's read off the analyzer-stamped type — for a scalar
// uint or a uint vector (uvec). Absent type info ⇒ signed, preserving the
// historical behavior on nodes the analyzer left untyped.
static bool isUnsignedOperand(const ExprAST* e) {
    const glsl::Type* t = e ? e->getType() : nullptr;
    if (!t) return false;
    if (t->isUint()) return true;
    if (t->isVector() && t->elementType()->isUint()) return true;
    return false;
}

// Number literal — Step 4: emit a constant of the analyzer-assigned type.
// Integer literals become real i32 constants (uint shares the signless i32);
// float/double become ConstantFP. An untyped literal (no sema run, e.g. a unit
// test that calls codegen() directly) falls back to the historical float so
// nothing regresses.
Value* NumberExprAST::codegen() {
    const glsl::Type* ty = getType();
    if (ty) {
        if (ty->isIntegral())
            return ConstantInt::get(Type::getInt32Ty(*Context),
                                    static_cast<uint64_t>(Val),
                                    /*isSigned=*/!ty->isUint());
        if (ty->isDouble())
            return ConstantFP::get(Type::getDoubleTy(*Context), Val);
        if (ty->isFloat())
            return ConstantFP::get(Type::getFloatTy(*Context), Val);
    }
    return ConstantFP::get(Type::getFloatTy(*Context), Val);
}

// Emit a widening conversion of `v` to LLVM type `dst` — both scalar, or both
// vectors of equal length (LLVM's cast instructions are elementwise, so the same
// opcode applies). `zeroExt` selects zext/uitofp (an unsigned or bool source)
// over sext/sitofp. Falls back to castScalarTo for any scalar pair not matched,
// else returns `v` unchanged.
static Value* emitWiden(Value* v, bool zeroExt, llvm::Type* dst) {
    llvm::Type* src = v->getType();
    if (src == dst) return v;
    llvm::Type* se = src->getScalarType();
    llvm::Type* de = dst->getScalarType();
    // int/uint/bool → float/double
    if (se->isIntegerTy() && de->isFloatingPointTy())
        return zeroExt ? Builder->CreateUIToFP(v, dst, "uitofp")
                       : Builder->CreateSIToFP(v, dst, "sitofp");
    // float → double
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

// Step 4/5a: a real widening conversion. The analyzer inserts these for scalar
// widenings (GLSL §4.1.10) and, since Step 5a, scalar→vector splats and
// elementwise vector→vector widening. Signedness of an integer source comes from
// the operand's glsl type (uint/bool ⇒ zext/uitofp). A value already in the
// target representation, or an untyped target, passes through untouched.
Value* ImplicitCastExprAST::codegen() {
    if (!Operand) return nullptr;
    Value* v = Operand->codegen();
    if (!v) return nullptr;

    const glsl::Type* dstTy = getType();
    if (!dstTy) return v;
    const glsl::Type* srcTy = Operand->getType();

    // INVARIANT (Step a relies on this): a matrix is never an implicit-cast
    // target. GLSL has no non-identity implicit matrix conversion (fixed shape,
    // all float), so coerce()/coerceOperand() never wrap one — see coerce(). If
    // this fires, a new coercion path is minting a matrix cast and this function
    // needs a real matrix branch (a matrix target currently falls through to the
    // scalar path, where lowerScalarType() returns nullptr ⇒ pass-through).
    assert(!dstTy->isMatrix() && "implicit cast to a matrix violates invariant");

    // Vector target (Step 5a): a scalar operand widens to the element type then
    // splats; a vector operand widens elementwise. Materializes what the legacy
    // makeTypesMatch did at the binary op, now at the operand.
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
        if (v->getType()->isIntOrIntVectorTy()) {
            if (v->getType()->isIntegerTy(1)) {
                logErrorAt(loc,"Negation not supported for boolean type");
                return nullptr;
            }
            return Builder->CreateNeg(v, "neg");
        }
        logErrorAt(loc,fmt::format("Negation not supported for type: {}", v->getType()->getStructName()));
        return nullptr;
    }

    if (Op == TokenKind::Not) {
        // Logical NOT is bool-only (sema rejects non-bool operands up front). On
        // i1, CreateNot flips the bit. The old `xor v, 1` fallback for wider ints
        // was a miscompile — `!5` gave 4 (truthy), not 0.
        if (v->getType()->isIntegerTy(1)) {
            return Builder->CreateNot(v, "not");
        }
        logErrorAt(loc,"Logical NOT (!) requires boolean type");
        return nullptr;
    }

    // bitwise complement — integer scalars/vectors only
    if (Op == TokenKind::BitwiseNot) {
        // Fold an integral float *constant* (number literals codegen as
        // float) to i32 so `~5` works; non-constant floats stay and error.
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

// ── Matrix binary-op lowering (codegen half of Step a) ─────────────────────
// Matrices are column-major `[cols x <rows x float>]` (ArrayType-of-vector) —
// the same layout tryCodegenMatrixConstructor builds and resolveTypeByName
// mints. Sema (inferMatrixBinary + checkOperandTypes) has already validated the
// dimensions, so these only LOWER: column c is `extractvalue M, c` (a
// <rows x float>); dotFloatVector does the row·column reductions.

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

// mat * scalar (and scalar * mat): scale each column by splat(s). Matrices are
// always float, so castScalarTo is a backstop for a scalar arriving as double.
static Value* matTimesScalar(Value* M, Value* s) {
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
        res = Builder->CreateInsertValue(
            res, Builder->CreateFMul(col, sV, "ms.scaled"), c, "ms.ins");
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

// Dispatch the five sema-validated matrix shapes. Anything else reaching here is
// a sema/codegen split — sema should have rejected it.
static Value* codegenMatrixBinary(TokenKind op, Value* L, Value* R,
                                  SourceLocation loc) {
    const bool lMat = L->getType()->isArrayTy();
    const bool rMat = R->getType()->isArrayTy();
    if (op == TokenKind::Multiply) {
        if (lMat && rMat)                       return matTimesMat(L, R);
        if (lMat && R->getType()->isVectorTy()) return matTimesVec(L, R);
        if (L->getType()->isVectorTy() && rMat) return vecTimesMat(L, R);
        if (lMat && !rMat)                      return matTimesScalar(L, R);
        if (rMat && !lMat)                      return matTimesScalar(R, L);
    }
    if (op == TokenKind::Plus || op == TokenKind::Minus)
        if (lMat && rMat)                       return matAddSub(op, L, R);
    logErrorAt(loc, "Internal: unsupported matrix operation reached codegen");
    return nullptr;
}

// Binary operator
Value* BinaryExprAST::codegen() {
    // Short-circuit && / || MUST be handled before either operand is evaluated.
    // The operands carry side effects (calls, i++) and the RHS may legally never
    // run; the arithmetic path below evaluates both operands eagerly (and the
    // old code then re-evaluated them a second time here), so this case owns the
    // whole function and returns early — it never falls through to that path.
    if (Op == TokenKind::And || Op == TokenKind::Or) {
        Function* F = Builder->GetInsertBlock()->getParent();

        Value* L = LHS->codegen();
        if (!L) return nullptr;

        BasicBlock* LHSBB = Builder->GetInsertBlock();

        // An operand expression can't legally terminate its block (break/return
        // are statements, not expressions), so this is an internal invariant —
        // diagnose rather than fail silently, which would crash codegen with no
        // clue why.
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

    // Matrix operands ([cols x <rows x float>], an ArrayType-of-vector) — the
    // type-match guard below has no ArrayType handling, so intercept here. Sema
    // (inferMatrixBinary + checkOperandTypes) already validated the dimensions,
    // so this only lowers. The element-is-vector guard keeps a stray non-matrix
    // array (which sema rejects anyway) out of the matrix path rather than
    // crashing a cast.
    auto isMatrixVal = [](Value* v) {
        auto* a = dyn_cast<ArrayType>(v->getType());
        return a && a->getElementType()->isVectorTy();
    };
    if (isMatrixVal(L) || isMatrixVal(R))
        return codegenMatrixBinary(Op, L, R, loc);

    // (The old ConstantFP→i32 fold for bitwise ops is gone: sema's
    // checkOperandTypes rejects float bitwise operands, and a typed integer
    // literal already codegens as a ConstantInt — so no float constant reaches a
    // bitwise op anymore.)

    // The legacy makeTypesMatch net is gone: sema now materializes matching
    // operands (coerceOperand at the binary site, scalar→vector splat, matrix and
    // builtin-result typing, stage-builtin binding), so a typed binary op arrives
    // with L and R already the same LLVM type. If they differ, an operand reached
    // codegen untyped — diagnose it rather than silently coercing (matrix pairs
    // were already handled by the ArrayType intercept above).
    if (L->getType() != R->getType()) {
        std::string ls, rs;
        llvm::raw_string_ostream(ls) << *L->getType();
        llvm::raw_string_ostream(rs) << *R->getType();
        logErrorAt(loc, fmt::format("operands of '{}' have incompatible types "
                                    "'{}' and '{}'", tokenKindName(Op), ls, rs));
        return nullptr;
    }

    Type* opType = L->getType();

    // Step 4: signedness for the operations that have a signed/unsigned split
    // (sdiv/udiv, srem/urem, ashr/lshr, the ordered icmp predicates). Read off
    // the analyzer-stamped operand types — an i32 is signless on its own. After
    // coercion both operands share a type, so either being unsigned settles it;
    // untyped operands fall back to signed (the historical behavior).
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

    // bitwise operators — integer scalars/vectors only. `>>` is logical (lshr)
    // for unsigned operands and arithmetic/sign-propagating (ashr) for signed,
    // chosen from the operand types (`uns`); LLVM i32 itself is signless.
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
                // §5.9: a shift's signedness follows the LHS ONLY (the RHS is
                // just a count). `uns` is the OR of both operands — correct for
                // arithmetic/comparison (sema coerces those to a common type),
                // but wrong here, so read the LHS directly: `int >> uint` must
                // stay arithmetic (ashr), else a negative value zero-fills.
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

    // Evaluate each arm in its own block, but DON'T branch to the merge block
    // yet: if the arms have different types, the unifying cast must be emitted
    // at the end of that arm's own block so the value dominates the edge feeding
    // the PHI. The old code unified types in MergeBB, which produced a cast that
    // didn't dominate its predecessor edge — an "instruction does not dominate
    // all uses" verifier error waiting on the first arm-type mismatch.
    Builder->SetInsertPoint(ThenBB);
    Value* thenVal = ThenExpr->codegen();
    if (!thenVal) return nullptr;
    BasicBlock* thenEnd = Builder->GetInsertBlock();

    Builder->SetInsertPoint(ElseBB);
    Value* elseVal = ElseExpr->codegen();
    if (!elseVal) return nullptr;
    BasicBlock* elseEnd = Builder->GetInsertBlock();

    // Reconcile differing arm types by widening the narrower arm *in its own
    // block*. Scalar int<float<double order; a vector/exotic mismatch is a type
    // error sema is expected to reject, and castScalarTo returns null for it so
    // we surface a located diagnostic instead of tripping the verifier.
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

// Member assignment - struct field or vector swizzle
llvm::Value* MemberAssignmentExprAST::codegen() {
    // MATRIX column: mat[col].xy = ...
    if (auto* matAccess = llvm::dyn_cast<MatrixAccessExprAST>(Object)) {
        MatrixColumnAssigner assigner(matAccess, Member, Init);
        return assigner.codegen();
    }

    // Find variable alloca (struct/vector)
    AllocaInst* alloca = nullptr;
    VariableExprAST* baseVar = nullptr;
    if (auto* varExpr = llvm::dyn_cast<VariableExprAST>(Object)) {
        auto it = NamedValues.find(varExpr->Name);
        if (it != NamedValues.end()) {
            alloca = it->second;
            baseVar = varExpr;
        }
    }
    
    if (!alloca) {
        logErrorAt(loc,"Assignment LHS must be variable (e.g., a.xy = ..., lights[0].prop1 = ...)");
        return nullptr;
    }
    
    Type* objTy = alloca->getAllocatedType();
    
    // STRUCT field: light.pos.x = ...
    if (objTy->isStructTy()) {
        StructFieldAssigner assigner(Object, Member, Init);
        return assigner.codegen();
    }

    // VECTOR swizzle: pos.xy = ...
    if (objTy->isVectorTy()) {
        VectorSwizzleAssigner assigner(Object, Member, Init);
        return assigner.codegen();
    }
    
    logErrorAt(loc,"Unsupported member assignment type");
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
        logErrorAt(loc,"Unknown function referenced: " + Callee);
        return nullptr;
    }

    if (CalleeF->arg_size() != Args.size()) {
        logErrorAt(loc,fmt::format("Incorrect number of arguments for function {}. Expected {}, got {}",
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
                logErrorAt(loc,fmt::format("Cannot cast argument {} to expected type for function {}", i, Callee));
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

    // EXISTING CODE for declaration with initialization - remains the same!
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

Value *MatrixAccessExprAST::codegen() {
    auto *lhsVar = llvm::dyn_cast<VariableExprAST>(Object);
    if (!lhsVar) {
        logErrorAt(loc,"Matrix/array access LHS must be a variable (e.g., M[i] or M[i][j])");
        return nullptr;
    }

    auto it = NamedValues.find(lhsVar->Name);
    if (it != NamedValues.end()) {
        AllocaInst *alloca = it->second;
        Type *baseTy       = alloca->getAllocatedType();

        if (!baseTy->isArrayTy()) {
            logErrorAt(loc,"Matrix/array access only supported on array types for local vars");
            return nullptr;
        }

        auto *arrTy  = cast<ArrayType>(baseTy);
        Type *elemTy = arrTy->getElementType();

        // first index
        if (!Index) {
            logErrorAt(loc,"Array/matrix access requires first index");
            return nullptr;
        }

        Value *iVal = Index->codegen();
        if (!iVal) return nullptr;
        iVal = toI32(iVal);
        if (!iVal) {
            logErrorAt(loc,"First index is not convertible to i32");
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
            logErrorAt(loc,"Second index is only valid for matrix (array-of-vector)");
            return nullptr;
        }

        auto *colTy  = cast<FixedVectorType>(elemTy);
        Value *col   = Builder->CreateLoad(colTy, elemPtr, lhsVar->Name + ".col");

        Value *jVal = Index2->codegen();
        if (!jVal) return nullptr;
        jVal = toI32(jVal);
        if (!jVal) {
            logErrorAt(this->loc,"Second index is not convertible to i32");
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
        if (!iVal) { logErrorAt(loc,"Storage buffer index not convertible to i32"); return nullptr; }
        // GEP into the buffer: getelementptr elemTy, ptr %p, i32 idx
        Value* elemPtr = Builder->CreateInBoundsGEP(
            info.elemTy, bufPtr, {iVal}, lhsVar->Name + ".elem.ptr");
        return Builder->CreateLoad(info.elemTy, elemPtr, lhsVar->Name + ".elem");
    }

    auto uit = UniformArrays.find(lhsVar->Name);
    if (uit == UniformArrays.end()) {
        logErrorAt(loc,fmt::format("Unknown variable '{}' in matrix/array access", lhsVar->Name));
        return nullptr;
    }

    auto *gv    = uit->second;
    Type *baseTy = gv->getValueType();

    if (!baseTy->isArrayTy()) {
        logErrorAt(loc,fmt::format("Uniform '{}' is not an array type", lhsVar->Name));
        return nullptr;
    }

    auto *arrTy  = cast<ArrayType>(baseTy);
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
    auto* lhsVar = llvm::dyn_cast<VariableExprAST>(Object);
    if (!lhsVar) {
        logErrorAt(loc,"Array assignment LHS must be a named variable");
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

    // Local array write: arr[i] = val
    auto it = NamedValues.find(lhsVar->Name);
    if (it == NamedValues.end()) {
        logErrorAt(loc,fmt::format("Unknown variable '{}' in array assignment", lhsVar->Name));
        return nullptr;
    }
    AllocaInst* alloca = it->second;
    Type* matTy = alloca->getAllocatedType();
    if (!matTy->isArrayTy()) {
        logErrorAt(loc,fmt::format("'{}' is not an array type", lhsVar->Name));
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
            logErrorAt(loc,fmt::format("Unknown return type: {}", RetType));
            return nullptr; 
        }
    }

    // make param types
    std::vector<Type*> paramTys;
    paramTys.reserve(Args.size());
    for (auto& [tyName, argName] : Args) {
        Type* ty = resolveTypeByName(tyName);
        if (!ty) { 
            logErrorAt(loc,fmt::format("Unknown param type: {}", tyName));
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
            logErrorAt(loc,fmt::format("Entry function verification failed for '{}'", Proto->Name));
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

// Register an opaque StructType under `Name`. Idempotent: if predeclare()
// is called twice (or once before codegen()), the second call is a no-op.
// Enables forward struct references — `struct A { B b; }` declared before
// `struct B { ... }`.
void StructDeclExprAST::predeclare() {
    using namespace llvm;
    if (NamedStructTypes.find(Name) != NamedStructTypes.end()) return;
    StructType* ST = StructType::create(*Context, Name);
    StructInfo info;
    info.Type = ST;
    // FieldNames stays empty until codegen() fills it.
    NamedStructTypes[Name] = std::move(info);
}

llvm::Value* StructDeclExprAST::codegen() {
    using namespace llvm;
    auto it = NamedStructTypes.find(Name);
    StructType* ST = nullptr;
    if (it != NamedStructTypes.end()) {
        // Predeclared: complete the opaque body. Reject only if a body was
        // already set (i.e. duplicate decl, not the predeclare placeholder).
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
        logErrorAt(loc,fmt::format( "Unknown type for uniform: {}", TypeName));
        return nullptr;
    }

    GlobalVariable* G = TheModule->getGlobalVariable(Name);
    if (!G) {
        auto *init = Constant::getNullValue(t);
        // LinkOnceODR so the same uniform declared in both VS and FS modules
        // merges cleanly when llvm-link joins them. ExternalLinkage would
        // error on duplicate definition; LinkOnceODR keeps one copy.
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

Value* UniformArrayDeclExprAST::codegen() {
    Type* elemType = resolveTypeByName(TypeName);
    if (!elemType) {
        logErrorAt(loc,fmt::format("Unknown type for uniform array: {}", TypeName));
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
            logErrorAt(loc,fmt::format("Uniform array re-declared with different type: {}", Name));
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
        logErrorAt(loc,fmt::format("Unknown element type for storage buffer: {}", ElemType));
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

// Postfix x++ / x--:
//   old = load x
//   new = old ± 1   (typed: int → i32, float → fp)
//   store new, x
//   return old
//
// The crucial bit is returning `old`, not `new`. The parser is responsible
// for placing this node in any context where the *value* of `x++` matters
// (`a[i++]`, `f(i++)`, `j = i++`). For an expression-statement `i++;` the
// returned value is simply discarded.
Value* PostfixIncrExprAST::codegen() {
    auto* var = llvm::dyn_cast<VariableExprAST>(Target);
    if (!var) {
        logErrorAt(loc, "Postfix ++/-- target must be a variable");
        return nullptr;
    }

    auto it = NamedValues.find(var->Name);
    if (it == NamedValues.end()) {
        logErrorAt(loc,
                   fmt::format("Undefined variable in postfix expression: {}",
                               var->Name));
        return nullptr;
    }
    AllocaInst* alloca = it->second;
    Type* ty = alloca->getAllocatedType();

    Value* oldVal = Builder->CreateLoad(ty, alloca, var->Name + ".old");

    Value* one = nullptr;
    if (ty->isIntegerTy()) {
        one = ConstantInt::get(ty, 1);
    } else if (ty->isFloatingPointTy()) {
        one = ConstantFP::get(ty, 1.0);
    } else {
        logErrorAt(loc,
                   "Postfix ++/-- only supported on int / float scalars");
        return nullptr;
    }

    Value* newVal = nullptr;
    if (ty->isIntegerTy()) {
        newVal = isDecrement ? Builder->CreateSub(oldVal, one, "dec")
                             : Builder->CreateAdd(oldVal, one, "inc");
    } else {
        newVal = isDecrement ? Builder->CreateFSub(oldVal, one, "dec")
                             : Builder->CreateFAdd(oldVal, one, "inc");
    }

    Builder->CreateStore(newVal, alloca);
    return oldVal;
}
