// emit_trampolines.h — shared pipeline trampoline emitter for irgen / irgen_riscv.
// Include exactly once per translation unit that needs emitPipelineTrampolines().

#pragma once

#include "../codegen_state/codegen_state.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

// Returns the number of float scalars in a type (e.g. <4 x float> → 4).
static unsigned typeFloatCount(Type* ty) {
    if (auto* vt = dyn_cast<FixedVectorType>(ty)) return vt->getNumElements();
    if (ty->isFloatTy() || ty->isDoubleTy() || ty->isIntegerTy()) return 1;
    return 0;
}

// Flatten one struct field into a flat float* array starting at floatOff.
static unsigned flattenField(Value* srcPtr, Type* fieldTy, unsigned fieldIdx,
                              StructType* stTy, Value* flatPtr,
                              unsigned floatOff, Type* f32Ty) {
    Value* fieldPtr = Builder->CreateStructGEP(stTy, srcPtr, fieldIdx, "fld");
    unsigned nElems = typeFloatCount(fieldTy);
    if (auto* vt = dyn_cast<FixedVectorType>(fieldTy)) {
        Value* vec = Builder->CreateLoad(vt, fieldPtr, "vec");
        for (unsigned i = 0; i < nElems; ++i) {
            Value* elem = Builder->CreateExtractElement(vec, Builder->getInt32(i));
            if (elem->getType() != f32Ty)
                elem = Builder->CreateBitCast(elem, f32Ty, "bc");
            Value* dst = Builder->CreateInBoundsGEP(f32Ty, flatPtr,
                                                     Builder->getInt32(floatOff + i), "dst");
            Builder->CreateStore(elem, dst);
        }
    } else {
        Value* val = Builder->CreateLoad(fieldTy, fieldPtr, "sc");
        if (val->getType() != f32Ty)
            val = Builder->CreateBitCast(val, f32Ty, "bc");
        Value* dst = Builder->CreateInBoundsGEP(f32Ty, flatPtr,
                                                 Builder->getInt32(floatOff), "dst");
        Builder->CreateStore(val, dst);
    }
    return floatOff + nElems;
}

// Load one struct field from a flat float* array (inverse of flattenField).
static Value* loadFieldFromFlat(Type* fieldTy, Value* flatPtr,
                                 unsigned floatOff, Type* f32Ty) {
    unsigned nElems = typeFloatCount(fieldTy);
    if (auto* vt = dyn_cast<FixedVectorType>(fieldTy)) {
        Value* vec = UndefValue::get(vt);
        for (unsigned i = 0; i < nElems; ++i) {
            Value* src = Builder->CreateInBoundsGEP(f32Ty, flatPtr,
                                                     Builder->getInt32(floatOff + i), "src");
            Value* elem = Builder->CreateLoad(f32Ty, src, "e");
            Type* elemTy = vt->getElementType();
            if (elem->getType() != elemTy)
                elem = Builder->CreateBitCast(elem, elemTy, "bc");
            vec = Builder->CreateInsertElement(vec, elem, Builder->getInt32(i));
        }
        return vec;
    } else {
        Value* src = Builder->CreateInBoundsGEP(f32Ty, flatPtr,
                                                 Builder->getInt32(floatOff), "src");
        return Builder->CreateLoad(fieldTy, src, "sv");
    }
}

// Emit vs_invoke / fs_invoke trampolines + layout constants.
// Must be called after all codegen and before module verification.
static void emitPipelineTrampolines() {
    auto* f32Ty  = Type::getFloatTy(*Context);
    auto* i32Ty  = Type::getInt32Ty(*Context);
    auto* ptrTy  = PointerType::getUnqual(*Context);
    auto* voidTy = Type::getVoidTy(*Context);

    Function* vsFunc = TheModule->getFunction("vs_main");
    Function* fsFunc = TheModule->getFunction("fs_main");

    // ── VS trampoline ──────────────────────────────────────────────────────────
    if (vsFunc) {
        StructType* outTy = StructType::getTypeByName(*Context, "VS_Output");
        if (outTy) {
            unsigned totalFloats = 0, varyingFloats = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i) {
                unsigned n = typeFloatCount(outTy->getElementType(i));
                totalFloats += n;
                if (i > 0) varyingFloats += n;
            }

            new GlobalVariable(*TheModule, i32Ty, true,
                               GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, totalFloats),
                               "vs_total_floats");
            new GlobalVariable(*TheModule, i32Ty, true,
                               GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, varyingFloats),
                               "vs_varying_floats");

            auto* invokeTy = FunctionType::get(voidTy, {i32Ty, i32Ty, ptrTy}, false);
            auto* invoke   = Function::Create(invokeTy, Function::ExternalLinkage,
                                              "vs_invoke", TheModule.get());
            auto args = invoke->arg_begin();
            Value* vid  = &*args++; vid->setName("vid");
            Value* iid  = &*args++; iid->setName("iid");
            Value* flat = &*args;   flat->setName("flat_out");

            auto* BB = BasicBlock::Create(*Context, "entry", invoke);
            Builder->SetInsertPoint(BB);

            Value* outStruct = Builder->CreateAlloca(outTy, nullptr, "vs_out");
            Builder->CreateCall(vsFunc, {vid, iid, outStruct});

            unsigned off = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i)
                off = flattenField(outStruct, outTy->getElementType(i), i, outTy, flat, off, f32Ty);
            Builder->CreateRetVoid();
        }
    }

    // ── FS trampoline ──────────────────────────────────────────────────────────
    if (fsFunc) {
        StructType* outTy = StructType::getTypeByName(*Context, "FS_Output");
        if (outTy) {
            unsigned outputFloats = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i)
                outputFloats += typeFloatCount(outTy->getElementType(i));

            new GlobalVariable(*TheModule, i32Ty, true,
                               GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, outputFloats),
                               "fs_output_floats");

            FunctionType* fsFT = fsFunc->getFunctionType();
            unsigned numFSParams = fsFT->getNumParams();

            auto* invokeTy = FunctionType::get(voidTy, {ptrTy, ptrTy, ptrTy}, false);
            auto* invoke   = Function::Create(invokeTy, Function::ExternalLinkage,
                                              "fs_invoke", TheModule.get());
            auto args2 = invoke->arg_begin();
            Value* fc    = &*args2++; fc->setName("fragcoord");
            Value* varys = &*args2++; varys->setName("varyings");
            Value* flat  = &*args2;   flat->setName("flat_out");

            auto* BB2 = BasicBlock::Create(*Context, "entry", invoke);
            Builder->SetInsertPoint(BB2);

            std::vector<Value*> callArgs;
            auto* vec4Ty = FixedVectorType::get(f32Ty, 4);
            callArgs.push_back(Builder->CreateLoad(vec4Ty, fc, "fragcoord_vec"));

            unsigned varyOff = 0;
            for (unsigned i = 1; i + 1 < numFSParams; ++i) {
                Type* paramTy = fsFT->getParamType(i);
                callArgs.push_back(loadFieldFromFlat(paramTy, varys, varyOff, f32Ty));
                varyOff += typeFloatCount(paramTy);
            }

            Value* outStruct = Builder->CreateAlloca(outTy, nullptr, "fs_out");
            callArgs.push_back(outStruct);
            Builder->CreateCall(fsFunc, callArgs);

            unsigned off = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i)
                off = flattenField(outStruct, outTy->getElementType(i), i, outTy, flat, off, f32Ty);
            Builder->CreateRetVoid();
        }
    }
}