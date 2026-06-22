#ifndef COMPILER_GLSL_UTILS_H
#define COMPILER_GLSL_UTILS_H

#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include "../codegen_state/codegen_state.h"
#include "../../common/error_utils_fmt.h"
#include <string>

// NOTE: no `using namespace llvm;` — this is a header, and a using-directive
// here leaks llvm:: into every translation unit that includes it (and would
// collide with glsl::Type vs llvm::Type). llvm names are qualified; `Builder`,
// `Context`, `NamedStructTypes`, `logError`, and the helper functions are
// project globals, not llvm.

static inline int charToIndex(char c) {
    if (c == 'x' || c == 'r') return 0;
    if (c == 'y' || c == 'g') return 1;
    if (c == 'z' || c == 'b') return 2;
    if (c == 'w' || c == 'a') return 3;
    return -1;
}

// Convert value to i32
inline llvm::Value* toI32(llvm::Value* v) {
        if (!v) return nullptr;
        llvm::Type* type = v->getType();
        llvm::Type* i32 = llvm::Type::getInt32Ty(*Context);
        // Already i32
        if (type->isIntegerTy(32))
            return v;
        // bool (i1) is a 0/1 value — widen by ZERO-extension. sext would make
        // `true` 0xFFFFFFFF (= -1), so e.g. arr[someBool] would index at -1.
        if (type->isIntegerTy(1))
            return Builder->CreateZExt(v, i32, "idx_zext");
        // Narrower signed integers (i8, i16) — sign-extend.
        if (type->isIntegerTy() && type->getIntegerBitWidth() < 32)
            return Builder->CreateSExt(v, i32, "idx_sext");
        // Integers that are too big - reject
        if (type->isIntegerTy() && type->getIntegerBitWidth() > 32) {
            logError("Index integer too wide (only <=32bit allowed)");
            return nullptr;
        }
        // Float to i32
        if (type->isFloatingPointTy())
            return Builder->CreateFPToSI(v, i32, "idx_fptosi");
        logError("Index must be convertible to signed int (i32)");
        return nullptr;
    }

    // Cast scalar value to destination type
    inline llvm::Value* castScalarTo(llvm::Value* v, llvm::Type* dst) {
        llvm::Type* src = v->getType();
        if (src == dst) return v;
        // type to boolean
        if (dst->isIntegerTy(1)) {
            if (src->isIntegerTy()) {
                return Builder->CreateICmpNE(v, llvm::ConstantInt::get(src, 0), "tobool");
            }
            if (src->isFloatTy() || src->isDoubleTy()) {
                llvm::Value* zero = src->isDoubleTy()
                    ? static_cast<llvm::Value*>(llvm::ConstantFP::get(llvm::Type::getDoubleTy(*Context), 0.0))
                    : static_cast<llvm::Value*>(llvm::ConstantFP::get(llvm::Type::getFloatTy(*Context), 0.0f));
                return Builder->CreateFCmpUNE(v, zero, "tobool");
            }
            return nullptr;
        }
        // float/double conversions
        if (src->isDoubleTy() && dst->isFloatTy())
            return Builder->CreateFPTrunc(v, dst, "fptrunc");
        if (src->isFloatTy() && dst->isDoubleTy())
            return Builder->CreateFPExt(v, dst, "fpext");

        // int to fp. bool (i1) is an unsigned 0/1 value → uitofp (sitofp would
        // give -1.0 for true; GLSL §5.4.1: float(true) == 1.0). NOTE: signed-only
        // for the int/uint split is still a Step-5 gap — only bool is fixed here.
        if (src->isIntegerTy() && (dst->isFloatTy() || dst->isDoubleTy())) {
            return src->isIntegerTy(1) ? Builder->CreateUIToFP(v, dst, "uitofp")
                                       : Builder->CreateSIToFP(v, dst, "sitofp");
        }
        // fp to int
        if ((src->isFloatTy() || src->isDoubleTy()) && dst->isIntegerTy()) {
            return Builder->CreateFPToSI(v, dst, "fptosi");
        }
        // int to int
        if (src->isIntegerTy() && dst->isIntegerTy()) {
            unsigned sw = src->getIntegerBitWidth();
            unsigned dw = dst->getIntegerBitWidth();
            if (sw == dw) return v;
            // bool (i1) widens by zero-extension so true→1, not -1.
            if (sw < dw)
                return src->isIntegerTy(1) ? Builder->CreateZExt(v, dst, "zext")
                                           : Builder->CreateSExt(v, dst, "sext");
            return Builder->CreateTrunc(v, dst, "trunc");
        }
        return nullptr;
    }

    // Splat scalar to vector
    inline llvm::Value* splatScalarToVector(llvm::Value* scalar, llvm::FixedVectorType* vecTy) {
        return Builder->CreateVectorSplat(vecTy->getNumElements(), scalar, "splat");
    }

    // Construct vector from scalars
    inline llvm::Value* buildVectorFromScalars(llvm::ArrayRef<llvm::Value*> scalars, unsigned size) {
        // zero vector
        if (scalars.empty()) {
            auto* vecTy = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*Context), size);
            return llvm::ConstantAggregateZero::get(vecTy);  // <4 x float> zero
        }
        // determine element type from first scalar and build vector
        llvm::Type* elementTy = scalars[0]->getType();
        auto* vecTy = llvm::FixedVectorType::get(elementTy, size);
        llvm::Value* res = llvm::UndefValue::get(vecTy);
        for (unsigned i = 0; i < scalars.size(); ++i) {
            if (scalars[i]->getType() != elementTy) {
                logError("Vector elements must have same type");
                return nullptr;
            }
            res = Builder->CreateInsertElement(res, scalars[i],
                                            Builder->getInt32(i), "ins");
        }
        return res;
    }

    inline llvm::Type* resolveTypeByName(llvm::StringRef name) {
        // For struct types. An opaque pre-declaration (FieldNames still
        // empty) is a *valid* reference at struct-decl time: LLVM allows
        // opaque struct types in nested field lists, and the body gets
        // set when that struct's StructDeclExprAST::codegen() runs.
        // Sema rejects truly unknown names before we get here.
        if (auto it = NamedStructTypes.find(name.str()); it != NamedStructTypes.end()) {
            return it->second.Type;
        }
        // Every value-bearing builtin from builtin_types.def — the same source
        // the lexer/parser/sema use, so the lists can't drift. (Replaces the old
        // hand-listed scalars + algorithmically-parsed vec/mat.) Matrices are
        // column-major [Cols x <Rows x float>]; samplers are opaque pointers.
#define BTYPE_SCALAR(Tok, Spelling, GlslKind, LlvmGetter) \
        if (name == Spelling) return llvm::Type::LlvmGetter(*Context);
#define BTYPE_VECTOR(Tok, Spelling, GlslElem, LlvmElem, N) \
        if (name == Spelling) \
            return llvm::FixedVectorType::get(llvm::Type::LlvmElem(*Context), N);
#define BTYPE_MATRIX(Tok, Spelling, Cols, Rows) \
        if (name == Spelling) \
            return llvm::ArrayType::get( \
                llvm::FixedVectorType::get(llvm::Type::getFloatTy(*Context), Rows), Cols);
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) \
        if (name == Spelling) return llvm::PointerType::getUnqual(*Context);
#include "../../frontend/ast/builtin_types.def"
        return nullptr;
    }

    // Register struct type and its field names
    inline void registerStructType(const std::string &name, llvm::StructType* st, const std::vector<std::string>& fieldNames) {
        NamedStructTypes[name] = {st, fieldNames};
    }

    inline llvm::Value* extractStructMember(llvm::Value* obj, llvm::StructType* st, const std::string& fieldName) {
        // get struct type name
        std::string stName = st->hasName() ? st->getName().str() : "";
        if (stName.empty()) {
            logError("Cannot access member of unnamed struct");
            return nullptr;
        }

        auto it = NamedStructTypes.find(stName);
        if (it == NamedStructTypes.end()) {
            logError(fmt::format("Struct field names not registered for {}", stName));
            return nullptr;
        }

        const auto& fields = it->second.FieldNames;
        auto fit = std::find(fields.begin(), fields.end(), fieldName);
        if (fit == fields.end()) {
            logError(fmt::format("Struct {} has no field '{}'", stName, fieldName));
            return nullptr;
        }

        unsigned idx = std::distance(fields.begin(), fit);
        return Builder->CreateExtractValue(obj, idx, ("memb." + fieldName).c_str());
    }

    inline llvm::Value* extractVectorComponents(llvm::Value* vec, llvm::FixedVectorType* vecTy, const std::string& swizzle) {
        unsigned numElems = vecTy->getNumElements();
        std::vector<int> indices;
        indices.reserve(swizzle.size());

        for (char c : swizzle) {
            int idx = charToIndex(c);
            if (idx < 0 || idx >= (int)numElems) {
                logError(fmt::format("Invalid swizzle component: {}", c));
                return nullptr;
            }
            indices.push_back(idx);
        }

        if (indices.size() == 1) {
            return Builder->CreateExtractElement(vec, Builder->getInt32(indices[0]), "comp");
        }

        llvm::Value* undefVec = llvm::UndefValue::get(vecTy);
        return Builder->CreateShuffleVector(vec, undefVec, indices, "swizzle");
    }

#endif
