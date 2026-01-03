#ifndef COMPILER_GLSL_UTILS_H
#define COMPILER_GLSL_UTILS_H

#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/IRBuilder.h>
#include "../codegen_state/codegen_state.h"
#include "../error_utils.h"
#include <string>

using namespace llvm;

static inline int charToIndex(char c) {
    if (c == 'x' || c == 'r') return 0;
    if (c == 'y' || c == 'g') return 1;
    if (c == 'z' || c == 'b') return 2;
    if (c == 'w' || c == 'a') return 3;
    return -1;
}

// Convert value to i32
inline Value* toI32(Value *v) {
        if (!v) return nullptr;
        llvm::Type* type = v->getType();
        llvm::Type* i32 = Type::getInt32Ty(*Context);
        // Already i32
        if (type->isIntegerTy(32))
            return v;
        // Integers that can be safely converted (i8, i16)
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
    inline Value* castScalarTo(Value* v, Type* dst) {
        Type* src = v->getType();
        if (src == dst) return v;
        // type to boolean
        if (dst->isIntegerTy(1)) {
            if (src->isIntegerTy()) {
                return Builder->CreateICmpNE(v, ConstantInt::get(src, 0), "tobool");
            }
            if (src->isFloatTy() || src->isDoubleTy()) {
                Value* zero = src->isDoubleTy()
                    ? static_cast<Value*>(ConstantFP::get(Type::getDoubleTy(*Context), 0.0))
                    : static_cast<Value*>(ConstantFP::get(Type::getFloatTy(*Context), 0.0f));
                return Builder->CreateFCmpUNE(v, zero, "tobool");
            }
            return nullptr;
        }
        // float/double conversions
        if (src->isDoubleTy() && dst->isFloatTy())
            return Builder->CreateFPTrunc(v, dst, "fptrunc");
        if (src->isFloatTy() && dst->isDoubleTy())
            return Builder->CreateFPExt(v, dst, "fpext");

        // int to fp
        if (src->isIntegerTy() && (dst->isFloatTy() || dst->isDoubleTy())) {
            return Builder->CreateSIToFP(v, dst, "sitofp");
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
            if (sw < dw)  return Builder->CreateSExt(v, dst, "sext");
            return Builder->CreateTrunc(v, dst, "trunc");
        }
        return nullptr;
    }

    // Splat scalar to vector
    inline Value* splatScalarToVector(Value* scalar, FixedVectorType* vecTy) {
        return Builder->CreateVectorSplat(vecTy->getNumElements(), scalar, "splat");
    }

    // Construct vector from scalars
    inline Value* buildVectorFromScalars(ArrayRef<Value*> scalars, unsigned size) {
        // zero vector
        if (scalars.empty()) {
            auto* vecTy = FixedVectorType::get(Type::getFloatTy(*Context), size);
            return ConstantAggregateZero::get(vecTy);  // <4 x float> zero
        }
        // determine element type from first scalar and build vector
        llvm::Type* elementTy = scalars[0]->getType();
        auto* vecTy = FixedVectorType::get(elementTy, size);
        Value* res = UndefValue::get(vecTy);
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
        // for struct types
        if (auto it = NamedStructTypes.find(name.str()); it != NamedStructTypes.end()) {
            const auto& info = it->second;
            return info.FieldNames.empty() ? nullptr : info.Type;
        }
        // for scalar types
        if (name == "double") return Type::getDoubleTy(*Context);
        if (name == "float")  return Type::getFloatTy(*Context);
        if (name == "int")    return Type::getInt32Ty(*Context);
        if (name == "uint")   return Type::getInt32Ty(*Context);
        if (name == "bool")   return Type::getInt1Ty(*Context);
        // for vector types
        if (name.consume_front("vec")) {
            if (unsigned n; !name.getAsInteger(10, n) && (n >= 2 && n <= 4))
                return FixedVectorType::get(Type::getFloatTy(*Context), n);
            logError("Invalid vector size");
            return nullptr;
        }
        // for matrix types
        if (name.consume_front("mat")) {
            unsigned col = 0, row = 0;
            // for mat2, mat3, mat4
            if (name.size() == 1 && isdigit(name[0])) {
                col = row = name[0] - '0';
            }
            // for mat2x3, mat3x2, mat4x2, mat2x4, mat3x4, mat4x3
            else {
                auto pos = name.find('x');
                if (pos == StringRef::npos) return nullptr;
                if (name.substr(0, pos).getAsInteger(10, col)) return nullptr;
                if (name.substr(pos+1).getAsInteger(10, row)) return nullptr;
            }
            auto* colTy = FixedVectorType::get(Type::getFloatTy(*Context), row);
            return ArrayType::get(colTy, col);
        }
        return nullptr;
    }

    // Register struct type and its field names
    inline void registerStructType(const std::string &name, llvm::StructType* st, const std::vector<std::string>& fieldNames) {
        NamedStructTypes[name] = {st, fieldNames};
    }

    // make L and R have matching types
    inline bool makeTypesMatch(Value*& L, Value*& R) {
        Type* LTy = L->getType();
        Type* RTy = R->getType();

        if (LTy == RTy) return true;

        // Helper lambda to convert entire vector to new element type
        auto convertVectorElemType = [&](Value* vec, Type* targetElemTy) -> Value* {
            auto* vecTy = cast<FixedVectorType>(vec->getType());
            unsigned numElems = vecTy->getNumElements();
            auto* targetVecTy = FixedVectorType::get(targetElemTy, numElems);
            
            Value* result = UndefValue::get(targetVecTy);
            for (unsigned i = 0; i < numElems; ++i) {
                Value* elem = Builder->CreateExtractElement(vec, Builder->getInt32(i));
                Value* converted = castScalarTo(elem, targetElemTy);
                if (!converted) return nullptr;
                result = Builder->CreateInsertElement(result, converted, Builder->getInt32(i));
            }
            return result;
        };

        bool LIsVec = llvm::isa<llvm::FixedVectorType>(LTy);
        bool RIsVec = llvm::isa<llvm::FixedVectorType>(RTy);


        // Vector + Scalar - splat scalar to vector
        if (LIsVec && !RIsVec) {
            auto* vecTy = cast<FixedVectorType>(LTy);
            Type* elemTy = vecTy->getElementType();
            Value* scalar = castScalarTo(R, elemTy);
            if (!scalar) return false;
            R = splatScalarToVector(scalar, vecTy);
            return true;
        }

        // Scalar + Vector - splat scalar to vector
        if (!LIsVec && RIsVec) {
            auto* vecTy = cast<FixedVectorType>(RTy);
            Type* elemTy = vecTy->getElementType();
            Value* scalar = castScalarTo(L, elemTy);
            if (!scalar) return false;
            L = splatScalarToVector(scalar, vecTy);
            return true;
        }

        // Vector + Vector
        if (LIsVec && RIsVec) {
            auto* LVecTy = cast<FixedVectorType>(LTy);
            auto* RVecTy = cast<FixedVectorType>(RTy);
            
            // Must have same number of elements
            if (LVecTy->getNumElements() != RVecTy->getNumElements()) {
                logError("Vector size mismatch");
                return false;
            }

            Type* elemTyL = LVecTy->getElementType();
            Type* elemTyR = RVecTy->getElementType();
            
            // Already same element type
            if (elemTyL == elemTyR) {
                return true;
            }

            // Promote to common element type
            Type* targetElemTy = nullptr;
            if (elemTyL->isDoubleTy() || elemTyR->isDoubleTy()) {
                targetElemTy = Type::getDoubleTy(*Context);
            } else if (elemTyL->isFloatTy() || elemTyR->isFloatTy()) {
                targetElemTy = Type::getFloatTy(*Context);
            } else if (elemTyL->isIntegerTy() && elemTyR->isIntegerTy()) {
                // Promote integers to wider type
                unsigned widthL = elemTyL->getIntegerBitWidth();
                unsigned widthR = elemTyR->getIntegerBitWidth();
                unsigned maxWidth = std::max(widthL, widthR);
                targetElemTy = Type::getIntNTy(*Context, maxWidth);
            } else {
                return false; // Incompatible types
            }

            // Convert both vectors to target element type
            if (elemTyL != targetElemTy) {
                Value* converted = convertVectorElemType(L, targetElemTy);
                if (!converted) return false;
                L = converted;
            }
            if (elemTyR != targetElemTy) {
                Value* converted = convertVectorElemType(R, targetElemTy);
                if (!converted) return false;
                R = converted;
            }
            return true;
        }

        // Scalar + Scalar - promote to higher precision
        if (!LIsVec && !RIsVec) {
            Type* targetTy = nullptr;

            // Promotion hierarchy: double > float > int64 > int32 > bool
            if (LTy->isDoubleTy() || RTy->isDoubleTy()) {
                targetTy = Type::getDoubleTy(*Context);
            } else if (LTy->isFloatTy() || RTy->isFloatTy()) {
                targetTy = Type::getFloatTy(*Context);
            } else if (LTy->isIntegerTy(64) || RTy->isIntegerTy(64)) {
                targetTy = Type::getInt64Ty(*Context);
            } else if (LTy->isIntegerTy(32) || RTy->isIntegerTy(32)) {
                targetTy = Type::getInt32Ty(*Context);
            } else if (LTy->isIntegerTy() || RTy->isIntegerTy()) {
                // Handle other integer types
                unsigned widthL = LTy->isIntegerTy() ? LTy->getIntegerBitWidth() : 1;
                unsigned widthR = RTy->isIntegerTy() ? RTy->getIntegerBitWidth() : 1;
                unsigned maxWidth = std::max(widthL, widthR);
                targetTy = Type::getIntNTy(*Context, maxWidth);
            } else {
                targetTy = Type::getInt1Ty(*Context); // both bool
            }

            // Promote both scalars to target type
            if (L->getType() != targetTy) {
                Value* casted = castScalarTo(L, targetTy);
                if (!casted) return false;
                L = casted;
            }
            if (R->getType() != targetTy) {
                Value* casted = castScalarTo(R, targetTy);
                if (!casted) return false;
                R = casted;
            }
            return true;
        }

        return false;
    }

    inline Value* extractStructMember(Value* obj, StructType* st, const std::string& fieldName) {
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

    inline Value* extractVectorComponents(Value* vec, FixedVectorType* vecTy, const std::string& swizzle) {
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

        Value* undefVec = UndefValue::get(vecTy);
        return Builder->CreateShuffleVector(vec, undefVec, indices, "swizzle");
    }

#endif
