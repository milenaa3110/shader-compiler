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

// Matrix spellings come from builtin_types.def, the same table the lexer,
// parser, sema and resolveTypeByName use, so a constructor call and a
// declaration of the same type can never disagree about which spellings exist.
static bool parseMatDims(const std::string& callee, unsigned& cols, unsigned& rows) {
#define BTYPE_MATRIX(Tok, Spelling, Cols, Rows) \
    if (callee == Spelling) { cols = Cols; rows = Rows; return true; }
#include "../../frontend/ast/builtin_types.def"
    return false;
}

// Components a value contributes when flattened into a constructor argument
// list: matrices count every element, vectors their lanes, scalars one.
static unsigned valueComponentCount(llvm::Value* v) {
    if (auto shape = matrixShapeOf(v->getType())) return shape->cols * shape->rows;
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(v->getType()))
        return vt->getNumElements();
    return 1;
}

// Append a value's scalar components to `out`, converting each to `elemTy`.
// Shared by the vector and matrix constructors so the GLSL flattening rules
// exist in exactly one place. Stops once `want` components have been collected
// (GLSL narrows an over-long trailing argument rather than erroring).
static bool accumulateComponents(llvm::Value* v, llvm::Type* elemTy, unsigned want,
                                 std::vector<llvm::Value*>& out,
                                 const CallExprAST* call) {
    auto push = [&](llvm::Value* e) -> bool {
        if (e->getType() != elemTy) {
            e = castScalarTo(e, elemTy);
            if (!e) {
                logErrorFmtAt(call->loc, "Constructor {}: cannot convert an argument component",
                              call->Callee);
                return false;
            }
        }
        out.push_back(e);
        return true;
    };

    if (auto shape = matrixShapeOf(v->getType())) {
        llvm::Value* colsVal = matrixColumns(v);
        for (unsigned c = 0; c < shape->cols && out.size() < want; ++c) {
            llvm::Value* col = Builder->CreateExtractValue(colsVal, c, "ctor.mcol");
            for (unsigned r = 0; r < shape->rows && out.size() < want; ++r)
                if (!push(Builder->CreateExtractElement(col, Builder->getInt32(r), "ctor.melem")))
                    return false;
        }
        return true;
    }
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(v->getType())) {
        for (unsigned i = 0; i < vt->getNumElements() && out.size() < want; ++i)
            if (!push(Builder->CreateExtractElement(v, Builder->getInt32(i), "ctor_comp")))
                return false;
        return true;
    }
    if (out.size() < want) return push(v);
    return true;
}

CtorResult tryCodegenMatrixConstructor(const CallExprAST* call) {
    unsigned cols = 0, rows = 0;
    if (!parseMatDims(call->Callee, cols, rows)) return CtorResult::notMine();

    Type* elemTy = Type::getFloatTy(*Context);
    auto* colTy  = FixedVectorType::get(elemTy, rows);
    // The inner column array; the %glsl.matCxR wrapper goes on at each return.
    auto* matTy  = ArrayType::get(colTy, cols);
    const unsigned diag = std::min(cols, rows);

    // Build a diagonal matrix from a single scalar (1.0 gives the identity).
    auto diagonal = [&](Value* s) -> Value* {
        Value* mat = ConstantAggregateZero::get(matTy);
        for (unsigned i = 0; i < diag; ++i) {
            Value* col = Builder->CreateExtractValue(mat, i, "col");
            col = Builder->CreateInsertElement(col, s, Builder->getInt32(i), "diagins");
            mat = Builder->CreateInsertValue(mat, col, i, "colins");
        }
        return matrixFromColumns(mat, cols, rows);
    };

    // matN() / matNxM() -> identity
    if (call->Args.empty()) return CtorResult::ok(diagonal(ConstantFP::get(elemTy, 1.0)));

    // Codegen every argument once, up front, so the dispatch below can look at
    // their *types*. Choosing a form by argument count alone is ambiguous: for
    // mat2, cols == 2 and cols*rows == 4, so `mat2(a, b)` with two scalars used
    // to be forced down the column path and rejected.
    std::vector<Value*> argVals;
    argVals.reserve(call->Args.size());
    for (auto& A : call->Args) {
        Value* v = A->codegen();
        if (!v) return CtorResult::failed();
        argVals.push_back(v);
    }

    if (argVals.size() == 1) {
        Value* a = argVals[0];
        // matC2xR2(matCxR): copy the overlapping block, fill the rest from the
        // identity. This is the basis of every normal-matrix idiom (mat3(MVP)).
        if (auto src = matrixShapeOf(a->getType())) {
            Value* srcCols = matrixColumns(a);
            Value* mat = ConstantAggregateZero::get(matTy);
            for (unsigned c = 0; c < cols; ++c) {
                Value* col = Builder->CreateExtractValue(mat, c, "col");
                for (unsigned r = 0; r < rows; ++r) {
                    Value* e;
                    if (c < src->cols && r < src->rows) {
                        Value* sc = Builder->CreateExtractValue(srcCols, c, "src.col");
                        e = Builder->CreateExtractElement(sc, Builder->getInt32(r), "src.elem");
                    } else if (c == r) {
                        e = ConstantFP::get(elemTy, 1.0);   // identity outside the copied block
                    } else {
                        continue;                            // already zero
                    }
                    col = Builder->CreateInsertElement(col, e, Builder->getInt32(r), "nar.ins");
                }
                mat = Builder->CreateInsertValue(mat, col, c, "colins");
            }
            return CtorResult::ok(matrixFromColumns(mat, cols, rows));
        }
        // matN(s) -> diagonal = s
        if (!a->getType()->isVectorTy()) {
            Value* s = a;
            if (s->getType() != elemTy) {
                s = castScalarTo(s, elemTy);
                if (!s) {
                    logErrorFmtAt(call->loc, "Constructor {}: argument is not convertible to float",
                                  call->Callee);
                    return CtorResult::failed();
                }
            }
            return CtorResult::ok(diagonal(s));
        }
        logErrorFmtAt(call->loc, "Constructor {}: a single argument must be a scalar or a matrix",
                      call->Callee);
        return CtorResult::failed();
    }

    // One vector per column: matCxR(vecR, vecR, ...).
    bool allColumns = argVals.size() == cols;
    for (Value* v : argVals)
        if (v->getType() != colTy) allColumns = false;
    if (allColumns) {
        Value* mat = UndefValue::get(matTy);
        for (unsigned c = 0; c < cols; ++c)
            mat = Builder->CreateInsertValue(mat, argVals[c], c, "colins");
        return CtorResult::ok(matrixFromColumns(mat, cols, rows));
    }

    // Otherwise flatten everything column-major and require exactly C*R
    // components. Sharing accumulateComponents with the vector constructor is
    // what makes mat2(vec2(1,2), 3.0, 4.0) work without a second copy of the
    // GLSL flattening rules.
    const unsigned want = cols * rows;
    std::vector<Value*> comps;
    comps.reserve(want);
    unsigned supplied = 0;
    for (Value* v : argVals) {
        supplied += valueComponentCount(v);
        if (!accumulateComponents(v, elemTy, want, comps, call))
            return CtorResult::failed();
    }
    if (supplied != want) {
        logErrorFmtAt(call->loc,
                      "Constructor {}: expected {} columns of vec{} or {} components, got {}",
                      call->Callee, cols, rows, want, supplied);
        return CtorResult::failed();
    }

    Value* mat = UndefValue::get(matTy);
    unsigned k = 0;
    for (unsigned c = 0; c < cols; ++c) {
        Value* col = UndefValue::get(colTy);
        for (unsigned r = 0; r < rows; ++r, ++k)
            col = Builder->CreateInsertElement(col, comps[k], Builder->getInt32(r), "colelm");
        mat = Builder->CreateInsertValue(mat, col, c, "colins");
    }
    return CtorResult::ok(matrixFromColumns(mat, cols, rows));
}

CtorResult tryCodegenVectorConstructor(const CallExprAST* call) {
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
        return CtorResult::notMine();

    const unsigned N = static_cast<unsigned>(C.back() - '0');
    auto* vecTy = llvm::FixedVectorType::get(elemTy, N);

    if (call->Args.empty()) {
        logErrorFmtAt(call->loc,"Constructor {} expects at least 1 argument", C);
        return CtorResult::failed();
    }

    // Codegen args once
    std::vector<llvm::Value*> argVals;
    argVals.reserve(call->Args.size());
    for (auto& A : call->Args) {
        llvm::Value* v = A->codegen();
        if (!v) return CtorResult::failed();
        argVals.push_back(v);
    }

    // GLSL-like splat: vecN(s) => (s,s,...)
    if (argVals.size() == 1 && !argVals[0]->getType()->isVectorTy() &&
        !isMatrixTy(argVals[0]->getType())) {
        llvm::Value* s = argVals[0];
        if (s->getType() != elemTy) {
            s = castScalarTo(s, elemTy);
            if (!s) return CtorResult::failed();
        }
        return CtorResult::ok(splatScalarToVector(s, vecTy));
    }

    // Flatten each argument into scalar components. accumulateComponents is
    // shared with the matrix constructor so the GLSL rules live in one place;
    // it also covers a matrix argument.
    std::vector<llvm::Value*> comps;
    comps.reserve(N);
    unsigned supplied = 0;
    for (size_t ai = 0; ai < argVals.size(); ++ai) {
        if (comps.size() == N) {
            logErrorFmtAt(call->loc,"Constructor {}: too many components", C);
            return CtorResult::failed();
        }
        supplied += valueComponentCount(argVals[ai]);
        if (!accumulateComponents(argVals[ai], elemTy, N, comps, call))
            return CtorResult::failed();
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

    return CtorResult::ok(buildVectorFromScalars(comps, N));
}

CtorResult tryCodegenScalarConstructor(const CallExprAST* call) {
    const std::string& C = call->Callee;
    if (C != "float" && C != "double" && C != "int" && C != "uint" && C != "bool")
        return CtorResult::notMine();

    if (call->Args.size() != 1) {
        logErrorFmtAt(call->loc,"Constructor {} expects exactly 1 argument", C);
        return CtorResult::failed();
    }

    Value* v = call->Args[0]->codegen();
    if (!v) return CtorResult::failed();

    Type* dst = resolveTypeByName(C);
    if (!dst) {
        logErrorFmtAt(call->loc,"Unknown scalar constructor: {}", C);
        return CtorResult::failed();
    }

    // GLSL takes the first component of a composite: float(vec3) is v.x, and
    // float(mat3) is m[0][0]. Confirmed against glslangValidator.
    if (auto shape = matrixShapeOf(v->getType())) {
        (void)shape;
        Value* col = Builder->CreateExtractValue(matrixColumns(v), 0, "sc.col");
        v = Builder->CreateExtractElement(col, Builder->getInt32(0), "sc.elem");
    } else if (v->getType()->isVectorTy()) {
        v = Builder->CreateExtractElement(v, Builder->getInt32(0), "sc.elem");
    }

    Value* cv = castScalarTo(v, dst);
    if (!cv) {
        logErrorFmtAt(call->loc,"Cannot cast to {}", C);
        return CtorResult::failed();
    }
    return CtorResult::ok(cv);
}

CtorResult tryCodegenStructConstructor(const CallExprAST* call) {
    Type* ty = resolveTypeByName(call->Callee);
    if (!ty) return CtorResult::notMine();

    auto* st = dyn_cast<StructType>(ty);
    // A matrix is a named struct (%glsl.matCxR) too, so it would otherwise be
    // claimed here. It has its own constructor with GLSL's own rules.
    if (!st || isMatrixTy(st)) return CtorResult::notMine();

    const unsigned numFields = st->getNumElements();

    if (call->Args.empty()) {
        return CtorResult::ok(ConstantAggregateZero::get(st));
    }

    if (call->Args.size() != numFields) {
        logErrorFmtAt(call->loc,"Struct constructor {} expects {} arguments, got {}",
                   call->Callee, numFields, call->Args.size());
        return CtorResult::failed();
    }

    Value* result = UndefValue::get(st);

    for (unsigned i = 0; i < numFields; ++i) {
        Value* argVal = call->Args[i]->codegen();
        if (!argVal) return CtorResult::failed();

        Type* fieldTy = st->getElementType(i);

        if (argVal->getType() != fieldTy) {
            if (fieldTy->isVectorTy() && !argVal->getType()->isVectorTy()) {
                auto* vecTy = dyn_cast<FixedVectorType>(fieldTy);
                if (!vecTy) {
                    logErrorFmtAt(call->loc,"Struct constructor {}: only fixed-size vectors supported for field {}",
                               call->Callee, i);
                    return CtorResult::failed();
                }
                Type* elemTy = vecTy->getElementType();
                Value* scalar = castScalarTo(argVal, elemTy);
                if (!scalar) return CtorResult::failed();

                argVal = splatScalarToVector(scalar, vecTy);
                if (!argVal) return CtorResult::failed();
            } else {
                argVal = castScalarTo(argVal, fieldTy);
                if (!argVal) return CtorResult::failed();
            }
        }

        result = Builder->CreateInsertValue(result, argVal, {i}, "fieldins");
    }

    return CtorResult::ok(result);
}