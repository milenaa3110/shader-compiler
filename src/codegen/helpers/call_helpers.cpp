// helpers/call_helpers.cpp
#include "call_helpers.h"
#include "../../frontend/ast/ast.h"
#include "../codegen_state/codegen_state.h"
#include "utils.h"
#include "../../common/error_utils_fmt.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

using namespace llvm;

static bool parseMatDims(const std::string& callee, unsigned& cols, unsigned& rows) {
    // Accept: mat2, mat3, mat4, mat2x3, mat3x4, mat4x2 ...
    if (callee.rfind("mat", 0) != 0) return false;
    if (callee.size() < 4) return false;

    auto isDigit = [](char ch) { return ch >= '0' && ch <= '9'; };

    // matN
    if (callee.find('x') == std::string::npos) {
        if (callee.size() != 4) return false;
        if (!isDigit(callee[3])) return false;
        cols = rows = static_cast<unsigned>(callee[3] - '0');
        return (cols >= 2 && cols <= 4);
    }

    // matNxM
    size_t x = callee.find('x');
    if (x == std::string::npos) return false;
    if (callee.size() != x + 2) return false;
    if (x != 4) return false;

    if (!isDigit(callee[3]) || !isDigit(callee[x + 1])) return false;

    cols = static_cast<unsigned>(callee[3] - '0');
    rows = static_cast<unsigned>(callee[x + 1] - '0');
    return (cols >= 2 && cols <= 4 && rows >= 2 && rows <= 4);
}

llvm::Value* tryCodegenMatrixConstructor(const CallExprAST* call) {
    unsigned cols = 0, rows = 0;
    if (!parseMatDims(call->Callee, cols, rows)) return nullptr;

    Type* elemTy = Type::getFloatTy(*Context);
    auto* colTy  = FixedVectorType::get(elemTy, rows);
    auto* matTy  = ArrayType::get(colTy, cols);

    // matN() / matNxM() -> identity (min(cols,rows)) on diagonal
    if (call->Args.empty()) {
        Value* mat = ConstantAggregateZero::get(matTy);
        for (unsigned i = 0; i < std::min(cols, rows); ++i) {
            Value* col = Builder->CreateExtractValue(mat, i, "col");
            col = Builder->CreateInsertElement(
                col,
                ConstantFP::get(elemTy, 1.0),
                Builder->getInt32(i),
                "diagins");
            mat = Builder->CreateInsertValue(mat, col, i, "colins");
        }
        return mat;
    }

    // matN(s) -> diagonal = s, rest 0
    if (call->Args.size() == 1) {
        Value* s = call->Args[0]->codegen();
        if (!s) return nullptr;
        if (s->getType() != elemTy) {
            s = castScalarTo(s, elemTy);
            if (!s) return nullptr;
        }

        Value* mat = ConstantAggregateZero::get(matTy);
        for (unsigned i = 0; i < std::min(cols, rows); ++i) {
            Value* col = Builder->CreateExtractValue(mat, i, "col");
            col = Builder->CreateInsertElement(col, s, Builder->getInt32(i), "diagins");
            mat = Builder->CreateInsertValue(mat, col, i, "colins");
        }
        return mat;
    }

    // matNxM(col0, col1, ... col{cols-1}) each col is vec{rows} of float
    if (call->Args.size() == cols) {
        Value* mat = UndefValue::get(matTy);
        for (unsigned c = 0; c < cols; ++c) {
            Value* v = call->Args[c]->codegen();
            if (!v) return nullptr;

            if (!v->getType()->isVectorTy() || v->getType() != colTy) {
                logErrorFmtAt(call->loc,"Matrix constructor {}: expects column {} to be vec{} of float",
                           call->Callee, c, rows);
                return nullptr;
            }
            mat = Builder->CreateInsertValue(mat, v, c, "colins");
        }
        return mat;
    }

    // matNxM(C*R scalars) column-major
    if (call->Args.size() == cols * rows) {
        Value* mat = UndefValue::get(matTy);
        unsigned k = 0;

        for (unsigned c = 0; c < cols; ++c) {
            Value* col = UndefValue::get(colTy);
            for (unsigned r = 0; r < rows; ++r, ++k) {
                Value* a = call->Args[k]->codegen();
                if (!a) return nullptr;
                if (a->getType() != elemTy) {
                    a = castScalarTo(a, elemTy);
                    if (!a) return nullptr;
                }
                col = Builder->CreateInsertElement(col, a, Builder->getInt32(r), "colelm");
            }
            mat = Builder->CreateInsertValue(mat, col, c, "colins");
        }
        return mat;
    }

    logErrorFmtAt(call->loc,"Matrix constructor: unsupported argument pattern for {}", call->Callee);
    return nullptr;
}

llvm::Value* tryCodegenVectorConstructor(const CallExprAST* call) {
    const std::string& C = call->Callee;
    // vecN → float elements; ivecN / uvecN → i32 elements. N ∈ {2,3,4}. (int and
    // uint share the signless i32; the conversion of each component is what
    // carries signedness, via castScalarTo below.)
    llvm::Type* elemTy = nullptr;
    if (C == "vec2" || C == "vec3" || C == "vec4")
        elemTy = llvm::Type::getFloatTy(*Context);
    else if (C == "ivec2" || C == "ivec3" || C == "ivec4" ||
             C == "uvec2" || C == "uvec3" || C == "uvec4")
        elemTy = llvm::Type::getInt32Ty(*Context);
    else
        return nullptr;

    const unsigned N = static_cast<unsigned>(C.back() - '0');
    auto* vecTy = llvm::FixedVectorType::get(elemTy, N);

    if (call->Args.empty()) {
        logErrorFmtAt(call->loc,"Constructor {} expects at least 1 argument", C);
        return nullptr;
    }

    // Codegen args once
    std::vector<llvm::Value*> argVals;
    argVals.reserve(call->Args.size());
    for (auto& A : call->Args) {
        llvm::Value* v = A->codegen();
        if (!v) return nullptr;
        argVals.push_back(v);
    }

    // GLSL-like splat: vecN(s) => (s,s,...)
    if (argVals.size() == 1 && !argVals[0]->getType()->isVectorTy()) {
        llvm::Value* s = argVals[0];
        if (s->getType() != elemTy) {
            s = castScalarTo(s, elemTy);
            if (!s) return nullptr;
        }
        return splatScalarToVector(s, vecTy);
    }

    std::vector<llvm::Value*> comps;
    comps.reserve(N);

    auto pushComp = [&](llvm::Value* e) -> bool {
        if (e->getType() != elemTy) {
            e = castScalarTo(e, elemTy);
            if (!e) return false;
        }
        comps.push_back(e);
        return true;
    };

    for (size_t ai = 0; ai < argVals.size(); ++ai) {
        llvm::Value* v = argVals[ai];

        if (v->getType()->isVectorTy()) {
            auto* vVecTy = llvm::dyn_cast<llvm::FixedVectorType>(v->getType());
            if (!vVecTy) {
                logErrorFmtAt(call->loc,"Constructor {}: only fixed-size vectors supported", C);
                return nullptr;
            }

            unsigned m = vVecTy->getNumElements();
            for (unsigned i = 0; i < m; ++i) {
                if (comps.size() == N) {
                    // Truncate extra components inside this vector argument (GLSL-like narrowing)
                    break;
                }
                llvm::Value* e = Builder->CreateExtractElement(v, Builder->getInt32(i), "ctor_comp");
                if (!pushComp(e)) return nullptr;
            }
        } else {
            if (comps.size() == N) {
                logErrorFmtAt(call->loc,"Constructor {}: too many components", C);
                return nullptr;
            }
            if (!pushComp(v)) return nullptr;
        }

        // If we've filled N but still have remaining arguments, that's an error.
        if (comps.size() == N && ai + 1 < argVals.size()) {
            logErrorFmtAt(call->loc,"Constructor {}: too many components", C);
            return nullptr;
        }
    }

    // Fill missing components: default 0, but for a 4-vector missing w => 1.
    // The constant follows the element type (ConstantInt for ivec/uvec).
    if (comps.size() < N) {
        auto elemConst = [&](double v) -> llvm::Value* {
            return elemTy->isIntegerTy()
                ? static_cast<llvm::Value*>(
                      llvm::ConstantInt::get(elemTy, static_cast<uint64_t>(v)))
                : static_cast<llvm::Value*>(llvm::ConstantFP::get(elemTy, v));
        };
        while (comps.size() < N) comps.push_back(elemConst(0.0));
        if (N == 4) comps[3] = elemConst(1.0);  // missing w => 1
    }

    return buildVectorFromScalars(comps, N);
}

llvm::Value* tryCodegenScalarConstructor(const CallExprAST* call) {
    const std::string& C = call->Callee;
    if (C != "float" && C != "double" && C != "int" && C != "uint" && C != "bool") return nullptr;

    if (call->Args.size() != 1) {
        logErrorFmtAt(call->loc,"Constructor {} expects exactly 1 argument", C);
        return nullptr;
    }

    Value* v = call->Args[0]->codegen();
    if (!v) return nullptr;

    Type* dst = resolveTypeByName(C);
    if (!dst) {
        logErrorFmtAt(call->loc,"Unknown scalar constructor: {}", C);
        return nullptr;
    }

    Value* cv = castScalarTo(v, dst);
    if (!cv) {
        logErrorFmtAt(call->loc,"Cannot cast to {}", C);
        return nullptr;
    }
    return cv;
}

llvm::Value* tryCodegenStructConstructor(const CallExprAST* call) {
    Type* ty = resolveTypeByName(call->Callee);
    if (!ty) return nullptr;

    auto* st = dyn_cast<StructType>(ty);
    if (!st) return nullptr;

    const unsigned numFields = st->getNumElements();

    if (call->Args.empty()) {
        return ConstantAggregateZero::get(st);
    }

    if (call->Args.size() != numFields) {
        logErrorFmtAt(call->loc,"Struct constructor {} expects {} arguments, got {}",
                   call->Callee, numFields, call->Args.size());
        return nullptr;
    }

    Value* result = UndefValue::get(st);

    for (unsigned i = 0; i < numFields; ++i) {
        Value* argVal = call->Args[i]->codegen();
        if (!argVal) return nullptr;

        Type* fieldTy = st->getElementType(i);

        if (argVal->getType() != fieldTy) {
            if (fieldTy->isVectorTy() && !argVal->getType()->isVectorTy()) {
                auto* vecTy = dyn_cast<FixedVectorType>(fieldTy);
                if (!vecTy) {
                    logErrorFmtAt(call->loc,"Struct constructor {}: only fixed-size vectors supported for field {}",
                               call->Callee, i);
                    return nullptr;
                }
                Type* elemTy = vecTy->getElementType();
                Value* scalar = castScalarTo(argVal, elemTy);
                if (!scalar) return nullptr;

                argVal = splatScalarToVector(scalar, vecTy);
                if (!argVal) return nullptr;
            } else {
                argVal = castScalarTo(argVal, fieldTy);
                if (!argVal) return nullptr;
            }
        }

        result = Builder->CreateInsertValue(result, argVal, {i}, "fieldins");
    }

    return result;
}