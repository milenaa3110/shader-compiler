// emit_trampolines.h — Shared pipeline trampoline emitter for irgen / irgen_riscv.
// Exposes wrappers to interface structured GLSL shader stages with a flat host runtime.

#pragma once

#include "../codegen_state/codegen_state.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

// Number of 32-bit (f32) slots a type occupies in the flat ABI: float/int
// scalars take one, vector components one each, arrays recurse. Doubles live in
// the separate 64-bit region (see typeDoubleCount) and contribute 0 here.
static unsigned typeFloatCount(Type* ty) {
    if (auto* at = dyn_cast<ArrayType>(ty))
        return at->getNumElements() * typeFloatCount(at->getElementType());
    if (auto* vt = dyn_cast<FixedVectorType>(ty)) return vt->getNumElements();
    if (ty->isFloatTy() || ty->isIntegerTy()) return 1;
    return 0;
}

// Number of 64-bit (f64) slots a type occupies — only doubles, recursing
// through arrays. Mirrors typeFloatCount for the double region of the ABI.
static unsigned typeDoubleCount(Type* ty) {
    if (auto* at = dyn_cast<ArrayType>(ty))
        return at->getNumElements() * typeDoubleCount(at->getElementType());
    if (ty->isDoubleTy()) return 1;
    return 0;
}

// Coerce a scalar into a single f32 ABI slot. A double is narrowed with
// FPTrunc (a double->float *bitcast* is illegal — widths differ), while 32-bit
// scalars (int/uint/float) are stored bit-for-bit via bitcast.
static Value* scalarToFlat(Value* v, Type* f32Ty) {
    Type* vt = v->getType();
    if (vt == f32Ty) return v;
    if (vt->isDoubleTy()) return Builder->CreateFPTrunc(v, f32Ty, "narrow");
    return Builder->CreateBitCast(v, f32Ty, "bc");
}

// Inverse of scalarToFlat: read one f32 ABI slot and produce a `dstTy` scalar.
// A double is widened back with FPExt; 32-bit scalars are bit-reinterpreted.
static Value* scalarFromFlat(Type* dstTy, Value* src, Type* f32Ty) {
    Value* v = Builder->CreateLoad(f32Ty, src, "e");
    if (dstTy == f32Ty) return v;
    if (dstTy->isDoubleTy()) return Builder->CreateFPExt(v, dstTy, "widen");
    return Builder->CreateBitCast(v, dstTy, "bc");
}

// Recursively flatten the value of type `ty` at `ptr` into the flat f32 array,
// returning the next free slot. Handles scalars, vectors, and arrays.
static unsigned flattenValue(Value* ptr, Type* ty, Value* flatPtr, unsigned floatOff,
                             Type* f32Ty) {
    if (auto* at = dyn_cast<ArrayType>(ty)) {
        for (unsigned i = 0; i < at->getNumElements(); ++i) {
            Value* elemPtr = Builder->CreateInBoundsGEP(
                at, ptr, { Builder->getInt32(0), Builder->getInt32(i) }, "arr.elt");
            floatOff = flattenValue(elemPtr, at->getElementType(), flatPtr, floatOff, f32Ty);
        }
        return floatOff;
    }
    if (auto* vt = dyn_cast<FixedVectorType>(ty)) {
        Value* vec = Builder->CreateLoad(vt, ptr, "vec");
        for (unsigned i = 0; i < vt->getNumElements(); ++i) {
            Value* elem = Builder->CreateExtractElement(vec, Builder->getInt32(i));
            Value* dst =
                Builder->CreateInBoundsGEP(f32Ty, flatPtr, Builder->getInt32(floatOff + i), "dst");
            Builder->CreateStore(scalarToFlat(elem, f32Ty), dst);
        }
        return floatOff + vt->getNumElements();
    }
    Value* val = Builder->CreateLoad(ty, ptr, "sc");
    Value* dst = Builder->CreateInBoundsGEP(f32Ty, flatPtr, Builder->getInt32(floatOff), "dst");
    Builder->CreateStore(scalarToFlat(val, f32Ty), dst);
    return floatOff + 1;
}

// Inverse of flattenValue: reconstruct a value of type `ty` from the flat
// array, advancing floatOff past the slots consumed.
static Value* loadValue(Type* ty, Value* flatPtr, unsigned& floatOff, Type* f32Ty) {
    if (auto* at = dyn_cast<ArrayType>(ty)) {
        Value* arr = UndefValue::get(at);
        for (unsigned i = 0; i < at->getNumElements(); ++i) {
            Value* elem = loadValue(at->getElementType(), flatPtr, floatOff, f32Ty);
            arr = Builder->CreateInsertValue(arr, elem, { i });
        }
        return arr;
    }
    if (auto* vt = dyn_cast<FixedVectorType>(ty)) {
        Value* vec = UndefValue::get(vt);
        for (unsigned i = 0; i < vt->getNumElements(); ++i) {
            Value* src =
                Builder->CreateInBoundsGEP(f32Ty, flatPtr, Builder->getInt32(floatOff + i), "src");
            vec = Builder->CreateInsertElement(vec, scalarFromFlat(vt->getElementType(), src, f32Ty),
                                               Builder->getInt32(i));
        }
        floatOff += vt->getNumElements();
        return vec;
    }
    Value* src = Builder->CreateInBoundsGEP(f32Ty, flatPtr, Builder->getInt32(floatOff), "src");
    Value* v = scalarFromFlat(ty, src, f32Ty);
    ++floatOff;
    return v;
}

// Two-region flatten: 32-bit leaves go to the f32 region (`fPtr`/`fOff`)
static void flattenValueSplit(Value* ptr, Type* ty, Value* fPtr, unsigned& fOff, Value* dPtr,
                              unsigned& dOff, Type* f32Ty, Type* f64Ty) {
    if (auto* at = dyn_cast<ArrayType>(ty)) {
        for (unsigned i = 0; i < at->getNumElements(); ++i) {
            Value* elemPtr = Builder->CreateInBoundsGEP(
                at, ptr, { Builder->getInt32(0), Builder->getInt32(i) }, "arr.elt");
            flattenValueSplit(elemPtr, at->getElementType(), fPtr, fOff, dPtr, dOff, f32Ty, f64Ty);
        }
        return;
    }
    if (auto* vt = dyn_cast<FixedVectorType>(ty)) {
        Value* vec = Builder->CreateLoad(vt, ptr, "vec");
        for (unsigned i = 0; i < vt->getNumElements(); ++i) {
            Value* elem = Builder->CreateExtractElement(vec, Builder->getInt32(i));
            Value* dst = Builder->CreateInBoundsGEP(f32Ty, fPtr, Builder->getInt32(fOff), "fdst");
            Builder->CreateStore(scalarToFlat(elem, f32Ty), dst);
            ++fOff;
        }
        return;
    }
    Value* val = Builder->CreateLoad(ty, ptr, "sc");
    if (ty->isDoubleTy()) {
        Value* dst = Builder->CreateInBoundsGEP(f64Ty, dPtr, Builder->getInt32(dOff), "ddst");
        Builder->CreateStore(val, dst);  // full 64-bit, no narrowing
        ++dOff;
    } else {
        Value* dst = Builder->CreateInBoundsGEP(f32Ty, fPtr, Builder->getInt32(fOff), "fdst");
        Builder->CreateStore(scalarToFlat(val, f32Ty), dst);
        ++fOff;
    }
}

// Inverse of flattenValueSplit: reconstruct a `ty` value, pulling doubles from
// the f64 region and everything else from the f32 region.
static Value* loadValueSplit(Type* ty, Value* fPtr, unsigned& fOff, Value* dPtr, unsigned& dOff,
                             Type* f32Ty, Type* f64Ty) {
    if (auto* at = dyn_cast<ArrayType>(ty)) {
        Value* arr = UndefValue::get(at);
        for (unsigned i = 0; i < at->getNumElements(); ++i) {
            Value* elem = loadValueSplit(at->getElementType(), fPtr, fOff, dPtr, dOff, f32Ty, f64Ty);
            arr = Builder->CreateInsertValue(arr, elem, { i });
        }
        return arr;
    }
    if (auto* vt = dyn_cast<FixedVectorType>(ty)) {
        Value* vec = UndefValue::get(vt);
        for (unsigned i = 0; i < vt->getNumElements(); ++i) {
            Value* src = Builder->CreateInBoundsGEP(f32Ty, fPtr, Builder->getInt32(fOff), "fsrc");
            vec = Builder->CreateInsertElement(
                vec, scalarFromFlat(vt->getElementType(), src, f32Ty), Builder->getInt32(i));
            ++fOff;
        }
        return vec;
    }
    if (ty->isDoubleTy()) {
        Value* src = Builder->CreateInBoundsGEP(f64Ty, dPtr, Builder->getInt32(dOff), "dsrc");
        Value* v = Builder->CreateLoad(f64Ty, src, "dv");  // full 64-bit
        ++dOff;
        return v;
    }
    Value* src = Builder->CreateInBoundsGEP(f32Ty, fPtr, Builder->getInt32(fOff), "fsrc");
    Value* v = scalarFromFlat(ty, src, f32Ty);
    ++fOff;
    return v;
}

// Flatten one struct field into the flat f32 array starting at floatOff.
static unsigned flattenField(Value* srcPtr, Type* fieldTy, unsigned fieldIdx, StructType* stTy,
                             Value* flatPtr, unsigned floatOff, Type* f32Ty) {
    Value* fieldPtr = Builder->CreateStructGEP(stTy, srcPtr, fieldIdx, "fld");
    return flattenValue(fieldPtr, fieldTy, flatPtr, floatOff, f32Ty);
}

// Load one struct field from the flat f32 array (inverse of flattenField).
static Value* loadFieldFromFlat(Type* fieldTy, Value* flatPtr, unsigned floatOff, Type* f32Ty) {
    unsigned off = floatOff;
    return loadValue(fieldTy, flatPtr, off, f32Ty);
}

// Emits pipeline invocation trampolines (vs_invoke, fs_invoke, cs_invoke/dispatch)
// and exposes layout sizing data via global metadata constants for the host runtime
static void emitPipelineTrampolines() {
    auto* f32Ty = Type::getFloatTy(*Context);
    auto* f64Ty = Type::getDoubleTy(*Context);
    auto* i32Ty = Type::getInt32Ty(*Context);
    auto* ptrTy = PointerType::getUnqual(*Context);
    auto* voidTy = Type::getVoidTy(*Context);

    Function* vsFunc = TheModule->getFunction("vs_main");
    Function* fsFunc = TheModule->getFunction("fs_main");
    // Vertex Shader (VS) Trampoline
    if (vsFunc) {
        StructType* outTy = StructType::getTypeByName(*Context, "VS_Output");
        if (outTy) {
            unsigned totalFloats = 0, varyingFloats = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i) {
                unsigned n = typeFloatCount(outTy->getElementType(i));
                totalFloats += n;
                if (i > 0) varyingFloats += n;
            }

            new GlobalVariable(*TheModule, i32Ty, true, GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, totalFloats), "vs_total_floats");
            new GlobalVariable(*TheModule, i32Ty, true, GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, varyingFloats), "vs_varying_floats");

            // Compute input attribute element count (skipping vid and iid)
            FunctionType* vsFT = vsFunc->getFunctionType();
            unsigned numVSParams = vsFT->getNumParams();

            unsigned inputFloats = 0, inputDoubles = 0;
            for (unsigned i = 2; i + 1 < numVSParams; ++i) {
                inputFloats  += typeFloatCount(vsFT->getParamType(i));
                inputDoubles += typeDoubleCount(vsFT->getParamType(i));
            }

            new GlobalVariable(*TheModule, i32Ty, true, GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, inputFloats), "vs_input_floats");
            new GlobalVariable(*TheModule, i32Ty, true, GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, inputDoubles), "vs_input_doubles");

            // vs_invoke(i32 vid, i32 iid, ptr flat_in, ptr flat_in_d, ptr flat_out):
            // attributes arrive split across a 32-bit region (flat_in) and a
            // 64-bit double region (flat_in_d); outputs are double-free varyings.
            auto* invokeTy =
                FunctionType::get(voidTy, { i32Ty, i32Ty, ptrTy, ptrTy, ptrTy }, false);
            auto* invoke =
                Function::Create(invokeTy, Function::ExternalLinkage, "vs_invoke", TheModule.get());
            auto args = invoke->arg_begin();
            Value* vid = &*args++;
            vid->setName("vid");
            Value* iid = &*args++;
            iid->setName("iid");
            Value* flatIn = &*args++;
            flatIn->setName("flat_in");
            Value* flatInD = &*args++;
            flatInD->setName("flat_in_d");
            Value* flatOut = &*args;
            flatOut->setName("flat_out");

            auto* BB = BasicBlock::Create(*Context, "entry", invoke);
            Builder->SetInsertPoint(BB);

            Value* outStruct = Builder->CreateAlloca(outTy, nullptr, "vs_out");

            std::vector<Value*> callArgs = { vid, iid };
            unsigned inOffF = 0, inOffD = 0;
            for (unsigned i = 2; i + 1 < numVSParams; ++i) {
                Type* paramTy = vsFT->getParamType(i);
                callArgs.push_back(
                    loadValueSplit(paramTy, flatIn, inOffF, flatInD, inOffD, f32Ty, f64Ty));
            }
            callArgs.push_back(outStruct);
            Builder->CreateCall(vsFunc, callArgs);

            unsigned off = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i)
                off = flattenField(outStruct, outTy->getElementType(i), i, outTy, flatOut, off,
                                   f32Ty);
            Builder->CreateRetVoid();
        }
    }

    // Fragment Shader (FS) Trampoline
    if (fsFunc) {
        StructType* outTy = StructType::getTypeByName(*Context, "FS_Output");
        if (outTy) {
            unsigned outputFloats = 0, outputDoubles = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i) {
                outputFloats  += typeFloatCount(outTy->getElementType(i));
                outputDoubles += typeDoubleCount(outTy->getElementType(i));
            }

            new GlobalVariable(*TheModule, i32Ty, true, GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, outputFloats), "fs_output_floats");
            new GlobalVariable(*TheModule, i32Ty, true, GlobalValue::ExternalLinkage,
                               ConstantInt::get(i32Ty, outputDoubles), "fs_output_doubles");

            FunctionType* fsFT = fsFunc->getFunctionType();
            unsigned numFSParams = fsFT->getNumParams();

            // fs_invoke(ptr fragcoord, ptr varyings, ptr flat_out, ptr flat_out_d):
            // varyings are double-free (interpolated), outputs split across a
            // 32-bit region (flat_out) and a 64-bit double region (flat_out_d).
            auto* invokeTy = FunctionType::get(voidTy, { ptrTy, ptrTy, ptrTy, ptrTy }, false);
            auto* invoke =
                Function::Create(invokeTy, Function::ExternalLinkage, "fs_invoke", TheModule.get());
            auto args2 = invoke->arg_begin();
            Value* fc = &*args2++;
            fc->setName("fragcoord");
            Value* varys = &*args2++;
            varys->setName("varyings");
            Value* flat = &*args2++;
            flat->setName("flat_out");
            Value* flatD = &*args2;
            flatD->setName("flat_out_d");

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

            unsigned offF = 0, offD = 0;
            for (unsigned i = 0; i < outTy->getNumElements(); ++i) {
                Value* fieldPtr = Builder->CreateStructGEP(outTy, outStruct, i, "fld");
                flattenValueSplit(fieldPtr, outTy->getElementType(i), flat, offF, flatD, offD, f32Ty,
                                  f64Ty);
            }
            Builder->CreateRetVoid();
        }
    }

    // Compute Shader (CS) Trampolines
    Function* csFunc = TheModule->getFunction("cs_main");
    if (csFunc) {
        auto* uvec3Ty = FixedVectorType::get(i32Ty, 3);

        // cs_invoke(i32 gid_x, i32 gid_y, i32 gid_z) — single-invocation helper
        // (kept for debugging / flexibility; hot path uses cs_dispatch_row)
        {
            auto* invokeTy = FunctionType::get(voidTy, { i32Ty, i32Ty, i32Ty }, false);
            auto* invoke =
                Function::Create(invokeTy, Function::ExternalLinkage, "cs_invoke", TheModule.get());
            auto csArgs = invoke->arg_begin();
            Value* gx = &*csArgs++;
            gx->setName("gid_x");
            Value* gy = &*csArgs++;
            gy->setName("gid_y");
            Value* gz = &*csArgs;
            gz->setName("gid_z");

            auto* BB = BasicBlock::Create(*Context, "entry", invoke);
            Builder->SetInsertPoint(BB);

            Value* zero3 = Constant::getNullValue(uvec3Ty);
            Value* gid = Builder->CreateInsertElement(zero3, gx, Builder->getInt32(0));
            gid = Builder->CreateInsertElement(gid, gy, Builder->getInt32(1));
            gid = Builder->CreateInsertElement(gid, gz, Builder->getInt32(2));
            Builder->CreateCall(csFunc, { gid, zero3, zero3, zero3 });
            Builder->CreateRetVoid();
        }

        // Vectorized row loop: cs_dispatch_row(i32 y, i32 width)
        {
            auto* dispTy = FunctionType::get(voidTy, { i32Ty, i32Ty }, false);
            auto* disp = Function::Create(dispTy, Function::ExternalLinkage, "cs_dispatch_row",
                                          TheModule.get());
            auto dArgs = disp->arg_begin();
            Value* argY = &*dArgs++;
            argY->setName("y");
            Value* argW = &*dArgs;
            argW->setName("width");

            auto* entryBB = BasicBlock::Create(*Context, "entry", disp);
            auto* loopBB = BasicBlock::Create(*Context, "loop", disp);
            auto* exitBB = BasicBlock::Create(*Context, "exit", disp);

            Builder->SetInsertPoint(entryBB);
            Builder->CreateBr(loopBB);

            Builder->SetInsertPoint(loopBB);
            PHINode* xPhi = Builder->CreatePHI(i32Ty, 2, "x");
            xPhi->addIncoming(Builder->getInt32(0), entryBB);

            Value* zero3 = Constant::getNullValue(uvec3Ty);
            Value* gid = Builder->CreateInsertElement(zero3, xPhi, Builder->getInt32(0));
            gid = Builder->CreateInsertElement(gid, argY, Builder->getInt32(1));
            gid = Builder->CreateInsertElement(gid, Builder->getInt32(0), Builder->getInt32(2));
            Builder->CreateCall(csFunc, { gid, zero3, zero3, zero3 });

            Value* xNext = Builder->CreateAdd(xPhi, Builder->getInt32(1), "x.next");
            xPhi->addIncoming(xNext, loopBB);
            Builder->CreateCondBr(Builder->CreateICmpSLT(xNext, argW, "cond"), loopBB, exitBB);

            Builder->SetInsertPoint(exitBB);
            Builder->CreateRetVoid();
        }
    }
}