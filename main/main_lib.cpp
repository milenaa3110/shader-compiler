//
// Created by Milena on 11/12/2025.
//

#include "../parser/parser.h"
#include "../ast/ast.h"
#include "../codegen_state/codegen_state.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>

extern int CurTok;
int getNextToken();

using namespace llvm;

// Initialize LLVM
static void InitializeModule() {
    Context = std::make_unique<llvm::LLVMContext>();
    TheModule  = std::make_unique<llvm::Module>("shader_module", *Context);
    Builder    = std::make_unique<llvm::IRBuilder<>>(*Context);
}

static bool addShadeWrapper() {
    // find shade function
    auto *F = TheModule->getFunction("shade");
    auto *f32   = Type::getFloatTy(*Context);
    auto *f32v2 = FixedVectorType::get(f32, 2);
    auto *f32v4 = FixedVectorType::get(f32, 4);

    if (!F) { std::cerr << "shade() not found\n"; return false; }
    auto *FT = F->getFunctionType();
    if (FT->getReturnType() != f32v4 || FT->getNumParams() != 1 || FT->getParamType(0) != f32v2) {
        std::cerr << "shade signature must be vec4(vec2)\n"; return false;
    }

    // void shade_wrapper(float u, float v, float* out)
    auto *f32ptr= PointerType::getUnqual(f32);
    auto *WrapTy = FunctionType::get(Type::getVoidTy(*Context), {f32, f32, f32ptr}, false);
    auto *Wrap   = Function::Create(WrapTy, Function::ExternalLinkage, "shade_wrapper", TheModule.get());

    // Dodaj C calling convention i ABI atribute
    Wrap->setCallingConv(llvm::CallingConv::C);
    Wrap->addFnAttr(llvm::Attribute::NoUnwind);

    auto *BB     = BasicBlock::Create(*Context, "entry", Wrap);
    Builder->SetInsertPoint(BB);

    auto AI = Wrap->arg_begin();
    Value* U   = &*AI++; U->setName("u");
    Value* V   = &*AI++; V->setName("v");
    Value* OUT = &*AI++; OUT->setName("out");

    // make vec2(u,v)
    Value* uv = UndefValue::get(f32v2);
    uv = Builder->CreateInsertElement(uv, U, Builder->getInt32(0));
    uv = Builder->CreateInsertElement(uv, V, Builder->getInt32(1));

    // call shade(vec2(u,v))
    Value* C = Builder->CreateCall(F, {uv}, "color");

    // store result into out[0..3]
    for (int i = 0; i < 4; ++i) {
        auto *c   = Builder->CreateExtractElement(C, Builder->getInt32(i));
        auto *ptr = Builder->CreateInBoundsGEP(f32, OUT, Builder->getInt32(i));
        Builder->CreateStore(c, ptr);
    }
    Builder->CreateRetVoid();
    return true;
}

#include "emit_trampolines.h"


int main() {
    InitializeModule();
    NamedValues.clear();

    // start parsing
    getNextToken();
    auto nodes = ParseProgram();
    if (nodes.empty()) { std::cerr << "Parse failed or program is empty.\n"; return 1; }

    // codegen for all nodes in the program top-level
    for (auto &n : nodes) {
        if (n && !n->codegen()) { std::cerr << "Codegen failed.\n"; return 1; }
    }

    // Only add the shade_wrapper shim when a legacy shade(vec2)->vec4 function exists.
    // Stage-entry shaders (@entry @stage(...)) don't have this function and don't need it.
    bool hasShade = TheModule->getFunction("shade") != nullptr;
    bool hasStageEntry = false;
    for (auto& F : *TheModule) {
        if (F.hasMetadata("shader.stage")) { hasStageEntry = true; break; }
    }
    if (hasShade) {
        if (!addShadeWrapper()) return 1;
    } else if (!hasStageEntry) {
        std::cerr << "shade() not found\n"; return 1;
    }

    // Emit pipeline trampolines (vs_invoke / fs_invoke) for stage-entry shaders
    if (hasStageEntry) {
        emitPipelineTrampolines();
    }

    if (llvm::verifyModule(*TheModule, &llvm::errs())) {
        std::cerr << "Invalid LLVM module.\n"; return 1;
    }

    // output LLVM IR to file
    std::error_code EC;
    llvm::raw_fd_ostream OS("module.ll", EC, llvm::sys::fs::OF_Text);
    if (EC) { std::cerr << "Cannot open module.ll: " << EC.message() << "\n"; return 1; }
    TheModule->print(OS, nullptr);
    std::cout << "Wrote module.ll\n";
    return 0;
}