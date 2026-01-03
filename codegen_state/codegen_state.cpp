//
// Created by Milena on 11/7/2025.
//

#include "codegen_state.h"
#include "../helpers/utils.h"
#include <vector>
#include "../error_utils.h"
#include <fmt/core.h>
#include <unordered_set>
namespace llvm {
    class Value;
}

std::unique_ptr<llvm::LLVMContext> Context;
std::unique_ptr<llvm::Module> TheModule;
std::unique_ptr<llvm::IRBuilder<>> Builder;
std::map<std::string, llvm::AllocaInst*> NamedValues;
std::unordered_map<std::string, StructInfo> NamedStructTypes;
std::unordered_set<std::string> StructTypes;
std::unordered_map<std::string, std::vector<std::string>> StructDependencies;
std::vector<llvm::BasicBlock*> BreakStack;
std::map<std::string, llvm::GlobalVariable*> UniformArrays;

using namespace llvm;

llvm::Value* splatIfScalarTo(IRBuilder<>& B, Value* v, Type* dstTy) {
    if (!v) return nullptr;
    if (v->getType() == dstTy) return v;

    auto* vecTy = dyn_cast<FixedVectorType>(dstTy);
    // if dstTy is not vector, nothing to do
    if (!vecTy) return v;

    auto* elemTy = vecTy->getElementType();

    if (!v->getType()->isVectorTy()) {
        if (v->getType() != elemTy) {
            v = castScalarTo(v, elemTy);
            if (!v) return nullptr;
        }
        return splatScalarToVector(v, vecTy);
    }
    return v;
}

unsigned getVectorLengthOr1(Type* T) {
    if (auto* V = dyn_cast<FixedVectorType>(T)) return V->getNumElements();
    return 1;
}

Type* getFloatElemOrType(Type* T) {
    if (auto* V = dyn_cast<FixedVectorType>(T)) return V->getElementType();
    return T;
}

Value* dotFloatVector(IRBuilder<>& B, Value* a, Value* b) {
    auto* VT = cast<FixedVectorType>(a->getType());
    unsigned N = VT->getNumElements();
    Value* mul = B.CreateFMul(a, b);
    Value* acc = B.CreateExtractElement(mul, (uint64_t)0);
    for (unsigned i = 1; i < N; ++i)
        acc = B.CreateFAdd(acc, B.CreateExtractElement(mul, i));
    return acc;
}

static Value* createUnaryIntrinsic(IRBuilder<>& B, Intrinsic::ID iid, Value* x) {
#if LLVM_VERSION_MAJOR >= 16
    return B.CreateUnaryIntrinsic(iid, x);
#else
    Function* F = Intrinsic::getDeclaration(B.GetInsertBlock()->getModule(), iid, { x->getType() });
    return B.CreateCall(F, { x });
#endif
}

static Value* createBinaryIntrinsic(IRBuilder<>& B, Intrinsic::ID iid, Value* a, Value* b) {
#if LLVM_VERSION_MAJOR >= 16
    return B.CreateBinaryIntrinsic(iid, a, b);
#else
    Function* F = Intrinsic::getDeclaration(B.GetInsertBlock()->getModule(), iid, { a->getType() });
    return B.CreateCall(F, { a, b });
#endif
}

Value* codegenBuiltin(IRBuilder<>& B,
                            Module* M,
                            const std::string& name,
                            const std::vector<Value*>& args) {
    // sin, cos, floor, etc.
    auto unaryFloat = [&](Intrinsic::ID iid) -> Value* {
        if (args.size() != 1) {
            logError(fmt::format("builtin {} expects 1 arg", name));
            return nullptr;
        }
        Value* x = const_cast<Value*>(args[0]);
        Type* ty = x->getType();
        Type* elem = getFloatElemOrType(ty);

        if (!elem->isFloatTy() && !elem->isDoubleTy()) {
            // cast scalar to float; defer vector casting if needed
            if (!ty->isVectorTy()) {
                x = castScalarTo(x, Type::getFloatTy(B.getContext()));
            } else {
                logError(fmt::format("builtin {}: vector non-float not supported", name));
                return nullptr;
            }
        }
        return createUnaryIntrinsic(B, iid, x);
    };

    if (name == "sin")   return unaryFloat(Intrinsic::sin);
    if (name == "cos")   return unaryFloat(Intrinsic::cos);
    if (name == "floor") return unaryFloat(Intrinsic::floor);
    if (name == "fract") {
        if (args.size() != 1) return nullptr;
        auto *x = args[0];
        auto *floorX = B.CreateUnaryIntrinsic(Intrinsic::floor, x);
        return B.CreateFSub(x, floorX, "fract");
    }
    // mix(a, b, t)
    if (name == "mix") {
        if (args.size() != 3) {
            logError("builtin mix expects 3 args");
            return nullptr;
        }
        Value* a = const_cast<Value*>(args[0]);
        Value* b = const_cast<Value*>(args[1]);
        Value* t = const_cast<Value*>(args[2]);

        // Determine the widest type (scalar vs vector)
        Type* wideType = a->getType();
        if (b->getType()->isVectorTy()) wideType = b->getType();
        if (t->getType()->isVectorTy()) wideType = t->getType();

        // Splat scalars to match vector size if needed
        a = splatIfScalarTo(B, a, wideType);
        b = splatIfScalarTo(B, b, wideType);
        t = splatIfScalarTo(B, t, wideType);

        if (!a || !b || !t) return nullptr;

        // mix(a, b, t) = a * (1 - t) + b * t
        Type* elemTy = getFloatElemOrType(wideType);
        Value* one = ConstantFP::get(elemTy, 1.0);
        if (wideType->isVectorTy()) {
            one = splatIfScalarTo(B, one, wideType);
        }

        Value* oneMinusT = B.CreateFSub(one, t, "1_minus_t");
        Value* aPart = B.CreateFMul(a, oneMinusT, "a_part");
        Value* bPart = B.CreateFMul(b, t, "b_part");
        return B.CreateFAdd(aPart, bPart, "mix");
    }

    // === mod(x,y) ===
    if (name == "mod") {
        if (args.size() != 2) {
            logError("builtin mod expects 2 args");
            return nullptr;
        }
        Value* x = const_cast<Value*>(args[0]);
        Value* y = const_cast<Value*>(args[1]);

        // align shape to "wider"
        Type* T = x->getType()->isVectorTy() ? x->getType()
                 : (y->getType()->isVectorTy() ? y->getType() : x->getType());
        x = splatIfScalarTo(B, x, T);
        y = splatIfScalarTo(B, y, T);
        if (!x || !y) return nullptr;

        // int: % (SRem/URem) — assume signed here
        if (getFloatElemOrType(T)->isIntegerTy()) {
            return B.CreateSRem(x, y);
        }

        // float: x - y * floor(x/y)
        Value* div = B.CreateFDiv(x, y);
        Value* fl  = createUnaryIntrinsic(B, Intrinsic::floor, div);
        Value* mul = B.CreateFMul(y, fl);
        return B.CreateFSub(x, mul);
    }

    // === normalize(v) ===
    if (name == "normalize") {
        if (args.size() != 1) {
            logError("normalize expects 1 arg");
            return nullptr;
        }
        Value* v = const_cast<Value*>(args[0]);
        if (!v->getType()->isVectorTy()) {
            logError("normalize expects a float vector");
            return nullptr;
        }
        auto* VT = cast<FixedVectorType>(v->getType());
        auto* elem = VT->getElementType();
        if (!elem->isFloatTy() && !elem->isDoubleTy()) {
            logError("normalize expects float/double vector");
            return nullptr;
        }
        Value* len2 = dotFloatVector(B, v, v);
        Value* len  = createUnaryIntrinsic(B, Intrinsic::sqrt, len2);

        // max(len, eps) (protection from zero)
        Value* eps = ConstantFP::get(elem, 1e-8);
        Value* denom = createBinaryIntrinsic(B, Intrinsic::maxnum, len, eps);

        // broadcast denom to vector
        Value* denomVec = splatIfScalarTo(B, denom, v->getType());
        return B.CreateFDiv(v, denomVec);
    }
    // === dot(a, b) ===
    if (name == "dot") {
        if (args.size() != 2) {
            logError("dot expects 2 args");
            return nullptr;
        }
        Value* a = const_cast<Value*>(args[0]);
        Value* b = const_cast<Value*>(args[1]);
        // both must be float vectors
        if (!a->getType()->isVectorTy() || !b->getType()->isVectorTy()) {
            logError("dot expects two float vectors");
            return nullptr;
        }

        auto* aVT = cast<FixedVectorType>(a->getType());
        auto* bVT = cast<FixedVectorType>(b->getType());

        if (aVT != bVT) {
            logError("dot expects vectors of same type");
            return nullptr;
        }

        Type* elemTy = aVT->getElementType();
        if (!elemTy->isFloatTy() && !elemTy->isDoubleTy()) {
            logError("dot expects float/double vectors");
            return nullptr;
        }

        return dotFloatVector(B, a, b);
    }
    // max(a, b) or min(a, b)
    if (name == "max" || name == "min") {
        if (args.size() != 2) {
            logError(fmt::format("{} requires 2 arguments", name));
            return nullptr;
        }

        Value* a = args[0];
        Value* b = args[1];
        Type* ta = a->getType();
        Type* tb = b->getType();

        // Promote scalar to vector if needed
        if (ta->isVectorTy() && !tb->isVectorTy()) {
            b = splatIfScalarTo(B, b, ta);
            tb = ta;
        } else if (!ta->isVectorTy() && tb->isVectorTy()) {
            a = splatIfScalarTo(B, a, tb);
            ta = tb;
        }

        // Both must be float or float vectors
        if (!ta->isFloatTy() && !(ta->isVectorTy() && ta->getScalarType()->isFloatTy())) {
            logError(fmt::format("{} requires float arguments", name));
            return nullptr;
        }

        if (ta != tb) {
            logError(fmt::format("{} requires matching types", name));
            return nullptr;
        }
        // Use LLVM intrinsic
        Intrinsic::ID id = (name == "max") ? Intrinsic::maxnum : Intrinsic::minnum;
        return B.CreateBinaryIntrinsic(id, a, b);
    }
    // length(v)
    if (name == "length") {
        if (args.size() != 1) {
            logError("length expects 1 arg");
            return nullptr;
        }
        Value* v = const_cast<Value*>(args[0]);

        // If scalar: abs(v) for float/double
        if (!v->getType()->isVectorTy()) {
            Type* elem = v->getType();
            if (elem->isFloatTy() || elem->isDoubleTy()) {
                return createUnaryIntrinsic(B, Intrinsic::fabs, v);
            }
            logError("length on scalar int not supported");
            return nullptr;
        }

        // For vector: sqrt(dot(v,v))
        auto* VT = cast<FixedVectorType>(v->getType());
        auto* elem = VT->getElementType();
        if (!elem->isFloatTy() && !elem->isDoubleTy()) {
            logError("length expects float/double vector");
            return nullptr;
        }

        Value* len2 = dotFloatVector(B, v, v);
        return createUnaryIntrinsic(B, Intrinsic::sqrt, len2);
    }
    if (name == "sqrt") {
        if (args.size() != 1) return nullptr;
        Value* arg = args[0];
        Type* argTy = arg->getType();

        // scalar float/double
        if (argTy->isFloatTy() || argTy->isDoubleTy()) {
            Type* retTy = argTy;
            Function* sqrtFn = Intrinsic::getDeclaration(M, Intrinsic::sqrt, {retTy});
            return B.CreateCall(sqrtFn, {arg}, "sqrt");
        }

        // vector float/double
        if (auto* VT = dyn_cast<FixedVectorType>(argTy)) {
            Type* retTy = VT;
            Function* sqrtFn = Intrinsic::getDeclaration(M, Intrinsic::sqrt, {retTy});
            return B.CreateCall(sqrtFn, {arg}, "sqrt");
        }

        return nullptr;
    }

    // === clamp(x, minVal, maxVal) ===
    if (name == "clamp") {
        if (args.size() != 3) {
            logError("clamp requires 3 arguments");
            return nullptr;
        }

        Value* x = args[0];
        Value* minVal = args[1];
        Value* maxVal = args[2];

        Type* tx = x->getType();
        Type* tmin = minVal->getType();
        Type* tmax = maxVal->getType();

        // Promote scalar to vector if needed
        Type* wideType = tx;
        if (tmin->isVectorTy() && !tx->isVectorTy()) {
            x = splatIfScalarTo(B, x, tmin);
            wideType = tmin;
        }
        if (tmax->isVectorTy() && !wideType->isVectorTy()) {
            x = splatIfScalarTo(B, x, tmax);
            minVal = splatIfScalarTo(B, minVal, tmax);
            wideType = tmax;
        }

        // Ensure all args have matching types
        if (tmin->isVectorTy() && tmin != wideType) {
            minVal = splatIfScalarTo(B, minVal, wideType);
        }
        if (tmax->isVectorTy() && tmax != wideType) {
            maxVal = splatIfScalarTo(B, maxVal, wideType);
        }

        // clamp(x, minVal, maxVal) = max(minVal, min(x, maxVal))
        Value* minXMax = B.CreateBinaryIntrinsic(Intrinsic::minnum, x, maxVal);
        return B.CreateBinaryIntrinsic(Intrinsic::maxnum, minVal, minXMax);
    }
    return nullptr;
}
