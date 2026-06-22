// sincos_opt.cpp — LLVM pass plugin: combine llvm.sin.f32(X) + llvm.cos.f32(X)
// pairs into a single sincosf(X, &s, &c) libcall.
//
// Background: irgen emits llvm.sin.f32 / llvm.cos.f32 intrinsics. LLVM's
// built-in sincos-combining only fires on direct sinf/cosf libcalls, so it
// never triggers on intrinsics. This pass runs after opt -O3 (which unifies
// identical subexpressions via GVN), at which point sin and cos of the same
// expression share the same SSA Value* argument — making pairs easy to find.
//
// Build:  see Makefile target sincos_opt.so
// Usage:  opt-18 -load-pass-plugin=./sincos_opt.so \
//                -passes='sincos-opt,mem2reg,instcombine' -S in.ll -o out.ll

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

// ── Pass ────────────────────────────────────────────────────────────────────

struct SinCosOptPass : PassInfoMixin<SinCosOptPass> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
        // Assign a sequential index to every instruction so we can order them.
        DenseMap<Instruction *, unsigned> order;
        unsigned n = 0;
        for (auto &BB : F)
            for (auto &I : BB)
                order[&I] = n++;

        // Collect the first sin / cos intrinsic call for each f32 argument.
        DenseMap<Value *, IntrinsicInst *> sinOf, cosOf;
        for (auto &BB : F)
            for (auto &I : BB)
                if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
                    if (!II->getType()->isFloatTy()) continue;
                    Value *arg = II->getArgOperand(0);
                    auto id   = II->getIntrinsicID();
                    if      (id == Intrinsic::sin && !sinOf.count(arg)) sinOf[arg] = II;
                    else if (id == Intrinsic::cos && !cosOf.count(arg)) cosOf[arg] = II;
                }

        // Declare  void sincosf(float, float*, float*)  once per module.
        auto &Ctx   = F.getContext();
        Module  *M  = F.getParent();
        Type *F32   = Type::getFloatTy(Ctx);
        Type *Void  = Type::getVoidTy(Ctx);
        Type *F32P  = PointerType::getUnqual(F32);
        FunctionCallee Sincosf = M->getOrInsertFunction(
            "sincosf",
            FunctionType::get(Void, {F32, F32P, F32P}, false));

        bool changed = false;
        SmallVector<Instruction *, 8> toErase;

        for (auto &[arg, sinII] : sinOf) {
            auto it = cosOf.find(arg);
            if (it == cosOf.end()) continue;
            IntrinsicInst *cosII = it->second;

            // Only handle same-basic-block pairs (cross-block dominance is
            // more complex; those cases are rare in straight-line shader code).
            if (sinII->getParent() != cosII->getParent()) continue;

            // Insert alloca at function entry (always dominates everything).
            IRBuilder<> entryB(&F.getEntryBlock().front());
            AllocaInst *SP = entryB.CreateAlloca(F32, nullptr, "sc_s");
            AllocaInst *CP = entryB.CreateAlloca(F32, nullptr, "sc_c");

            // Place the sincosf call + loads before whichever intrinsic
            // comes first; that guarantees the results dominate all uses.
            Instruction *first = (order[sinII] < order[cosII])
                                     ? static_cast<Instruction *>(sinII)
                                     : static_cast<Instruction *>(cosII);
            IRBuilder<> B(first);
            B.CreateCall(Sincosf, {arg, SP, CP});
            Value *sv = B.CreateLoad(F32, SP, "s");
            Value *cv = B.CreateLoad(F32, CP, "c");

            sinII->replaceAllUsesWith(sv);
            cosII->replaceAllUsesWith(cv);
            toErase.push_back(sinII);
            toErase.push_back(cosII);
            changed = true;
        }

        for (auto *I : toErase)
            I->eraseFromParent();

        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

// ── Plugin registration ──────────────────────────────────────────────────────

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION, "sincos-opt", "v0.1",
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "sincos-opt") {
                        FPM.addPass(SinCosOptPass{});
                        return true;
                    }
                    return false;
                });
        }
    };
}
