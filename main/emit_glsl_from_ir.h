// emit_glsl_from_ir.h — Translate LLVM IR Module to GLSL 450 source text.
//
// Designed for pre-optimisation IR produced by ast.cpp codegen. Recognises:
//   - allocas        → local variable declarations
//   - globals        → uniform / push_constant block
//   - extractelement → swizzle (.x .y .z .w)
//   - insertelement  → vec constructor
//   - LLVM intrinsics→ GLSL builtins (sin, cos, sqrt, exp, log, pow, ...)
//   - codegen builtins (clamp, step, mix, etc.) → GLSL calls
//   - for.cond/for.body/for.inc/for.end → for loop
//   - then/else/ifend → if/else
//
// Only handles the fs_main / vs_main / cs_main functions (skips trampolines).

#pragma once

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/CFG.h>

#include <string>
#include <map>
#include <vector>
#include <set>
#include <sstream>
#include <cassert>
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ────���─────────────────────��────────────────────────��─────────────────────────

static std::string glslTypeName(llvm::Type* ty) {
    if (ty->isFloatTy()) return "float";
    if (ty->isIntegerTy(32)) return "int";
    if (ty->isIntegerTy(1))  return "bool";
    if (auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(ty)) {
        unsigned n = vt->getNumElements();
        if (vt->getElementType()->isFloatTy()) {
            if (n == 2) return "vec2";
            if (n == 3) return "vec3";
            if (n == 4) return "vec4";
        }
        if (vt->getElementType()->isIntegerTy(32)) {
            if (n == 2) return "ivec2";
            if (n == 3) return "ivec3";
            if (n == 4) return "ivec4";
        }
    }
    if (ty->isVoidTy()) return "void";
    return "float"; // fallback
}

static const char* swizzle(unsigned idx) {
    static const char* s[] = {"x","y","z","w"};
    return (idx < 4) ? s[idx] : "x";
}

static std::string fmtFloat(double v) {
    if (v == 0.0) return "0.0";
    if (v == 1.0) return "1.0";
    if (v == -1.0) return "-1.0";
    if (v == (long long)v && std::abs(v) < 1e14) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld.0", (long long)v);
        return buf;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%.8g", v);
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
        s += ".0";
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// IR → GLSL emitter
// ───��─────────────────��───────────────────────────────────────────────────────

struct IRToGLSL {
    llvm::Module& M;
    std::string glsl;                              // accumulated output
    std::map<llvm::Value*, std::string> names;     // value → GLSL name
    std::set<std::string> declaredVars;            // already emitted declarations
    int tmpCounter = 0;
    int indentLevel = 1;

    // Shader interface extracted from the IR
    struct StageVar { std::string type; std::string name; bool isInput; int location; };
    std::vector<StageVar> stageVars;
    std::vector<std::pair<std::string,std::string>> uniforms; // type, name

    explicit IRToGLSL(llvm::Module& mod) : M(mod) {}

    std::string indent() { return std::string(indentLevel * 4, ' '); }

    std::string tmpName() { return "_t" + std::to_string(tmpCounter++); }

    // Get the GLSL expression for a value
    std::string val(llvm::Value* v) {
        if (auto it = names.find(v); it != names.end())
            return it->second;

        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v))
            return std::to_string(ci->getSExtValue());

        if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(v)) {
            double d = cf->getValueAPF().convertToDouble();
            return fmtFloat(d);
        }

        if (auto* cv = llvm::dyn_cast<llvm::ConstantVector>(v)) {
            unsigned n = cv->getType()->getNumElements();
            std::string s = glslTypeName(cv->getType()) + "(";
            for (unsigned i = 0; i < n; i++) {
                if (i) s += ", ";
                s += val(cv->getAggregateElement(i));
            }
            return s + ")";
        }

        if (auto* caz = llvm::dyn_cast<llvm::ConstantAggregateZero>(v)) {
            return glslTypeName(caz->getType()) + "(0.0)";
        }

        if (llvm::isa<llvm::UndefValue>(v))
            return glslTypeName(v->getType()) + "(0.0)";

        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(v))
            return gv->getName().str();

        // Unnamed — assign a temporary
        std::string nm = tmpName();
        names[v] = nm;
        return nm;
    }

    // ── Intrinsic / call mapping ──────────────────────────────────────────────

    std::string mapIntrinsic(llvm::Intrinsic::ID id) {
        switch (id) {
            case llvm::Intrinsic::sin:     return "sin";
            case llvm::Intrinsic::cos:     return "cos";
            case llvm::Intrinsic::sqrt:    return "sqrt";
            case llvm::Intrinsic::exp:     return "exp";
            case llvm::Intrinsic::exp2:    return "exp2";
            case llvm::Intrinsic::log:     return "log";
            case llvm::Intrinsic::log2:    return "log2";
            case llvm::Intrinsic::pow:     return "pow";
            case llvm::Intrinsic::fabs:    return "abs";
            case llvm::Intrinsic::floor:   return "floor";
            case llvm::Intrinsic::ceil:    return "ceil";
            case llvm::Intrinsic::round:   return "round";
            case llvm::Intrinsic::trunc:   return "trunc";
            case llvm::Intrinsic::maxnum:  return "max";
            case llvm::Intrinsic::minnum:  return "min";
            case llvm::Intrinsic::fma:     return "fma";
            case llvm::Intrinsic::copysign:return "sign";
            default: return "";
        }
    }

    // Map codegen-state helper function names to GLSL builtins
    std::string mapCallName(llvm::StringRef name) {
        // Direct GLSL builtins from codegen_state
        if (name == "clamp_f" || name == "clampf")   return "clamp";
        if (name == "step_f" || name == "stepf")      return "step";
        if (name == "smoothstep_f" || name == "smoothstepf") return "smoothstep";
        if (name == "mix_f" || name == "mixf")        return "mix";
        if (name == "sign_f" || name == "signf")      return "sign";
        if (name == "fract_f" || name == "fractf")    return "fract";
        if (name == "mod_f" || name == "modf_glsl")   return "mod";
        if (name == "distance_f")                     return "distance";
        if (name == "dot_f")                          return "dot";
        if (name == "length_f")                       return "length";
        if (name == "normalize_f")                    return "normalize";
        if (name == "cross_f")                        return "cross";
        if (name == "reflect_f")                      return "reflect";
        if (name == "sincosf")                        return ""; // handled specially
        if (name == "__frag_discard")                  return "discard";
        if (name == "__barrier")                       return "barrier";
        return "";
    }

    // ── Emit one instruction ──────────────────────────────────────────────────

    void emitInst(llvm::Instruction& I) {
        // Skip alloca — we declare vars when first stored
        if (llvm::isa<llvm::AllocaInst>(I)) {
            // Pre-name the alloca with its LLVM name
            if (I.hasName()) {
                names[&I] = I.getName().str();
            }
            return;
        }

        // Store → assignment
        if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            std::string dst = val(SI->getPointerOperand());
            std::string src = val(SI->getValueOperand());

            // Skip stores to _out (trampoline output struct)
            if (dst == "_out" || dst.find("_out") == 0) return;

            // Skip stores of function params to their shadow allocas
            // (pattern: store %param, ptr %param1 — used by the codegen to shadow args)
            if (llvm::isa<llvm::Argument>(SI->getValueOperand())) {
                // Alias the alloca to the param name for later loads
                std::string paramName = src;
                names[SI->getPointerOperand()] = paramName;
                return;
            }

            // Declare variable on first use
            llvm::Type* vty = SI->getValueOperand()->getType();
            if (declaredVars.find(dst) == declaredVars.end()) {
                // Skip re-declaration of parameters and FragColor (declared in header)
                if (dst != "FragColor" && dst != "gl_Position" &&
                    dst != "gl_FragCoord" && dst != "vUV" && dst != "vColor") {
                    glsl += indent() + glslTypeName(vty) + " " + dst + " = " + src + ";\n";
                    declaredVars.insert(dst);
                    return;
                }
            }
            glsl += indent() + dst + " = " + src + ";\n";
            return;
        }

        // Load → just alias the name
        if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            std::string ptr = val(LI->getPointerOperand());
            names[&I] = ptr;
            return;
        }

        // GEP — skip (handled by load/store context)
        if (llvm::isa<llvm::GetElementPtrInst>(I)) {
            names[&I] = val(I.getOperand(0));
            return;
        }

        // Binary float ops
        if (auto* BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
            std::string a = val(BO->getOperand(0));
            std::string b = val(BO->getOperand(1));
            std::string op;
            switch (BO->getOpcode()) {
                case llvm::Instruction::FAdd: op = " + "; break;
                case llvm::Instruction::FSub: op = " - "; break;
                case llvm::Instruction::FMul: op = " * "; break;
                case llvm::Instruction::FDiv: op = " / "; break;
                case llvm::Instruction::FRem: op = " % "; break; // mod
                case llvm::Instruction::Add:  op = " + "; break;
                case llvm::Instruction::Sub:  op = " - "; break;
                case llvm::Instruction::Mul:  op = " * "; break;
                case llvm::Instruction::SDiv: op = " / "; break;
                case llvm::Instruction::And:  op = " & "; break;
                case llvm::Instruction::Or:   op = " | "; break;
                default: op = " + "; break;
            }
            std::string nm = I.hasName() ? I.getName().str() : tmpName();
            names[&I] = nm;
            // Check if result is immediately stored to an alloca
            if (I.hasOneUse()) {
                if (auto* st = llvm::dyn_cast<llvm::StoreInst>(*I.user_begin())) {
                    // Will be handled by the store
                    names[&I] = "(" + a + op + b + ")";
                    return;
                }
            }
            names[&I] = "(" + a + op + b + ")";
            return;
        }

        // FNeg
        if (auto* UN = llvm::dyn_cast<llvm::UnaryOperator>(&I)) {
            if (UN->getOpcode() == llvm::Instruction::FNeg) {
                names[&I] = "(-" + val(UN->getOperand(0)) + ")";
                return;
            }
        }

        // FCmp
        if (auto* FC = llvm::dyn_cast<llvm::FCmpInst>(&I)) {
            std::string a = val(FC->getOperand(0));
            std::string b = val(FC->getOperand(1));
            std::string op;
            switch (FC->getPredicate()) {
                case llvm::CmpInst::FCMP_OLT: op = " < ";  break;
                case llvm::CmpInst::FCMP_OLE: op = " <= "; break;
                case llvm::CmpInst::FCMP_OGT: op = " > ";  break;
                case llvm::CmpInst::FCMP_OGE: op = " >= "; break;
                case llvm::CmpInst::FCMP_OEQ: op = " == "; break;
                case llvm::CmpInst::FCMP_ONE: op = " != "; break;
                default: op = " == "; break;
            }
            names[&I] = "(" + a + op + b + ")";
            return;
        }

        // ICmp
        if (auto* IC = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
            std::string a = val(IC->getOperand(0));
            std::string b = val(IC->getOperand(1));
            std::string op;
            switch (IC->getPredicate()) {
                case llvm::CmpInst::ICMP_EQ:  op = " == "; break;
                case llvm::CmpInst::ICMP_NE:  op = " != "; break;
                case llvm::CmpInst::ICMP_SLT: op = " < ";  break;
                case llvm::CmpInst::ICMP_SLE: op = " <= "; break;
                case llvm::CmpInst::ICMP_SGT: op = " > ";  break;
                case llvm::CmpInst::ICMP_SGE: op = " >= "; break;
                default: op = " == "; break;
            }
            names[&I] = "(" + a + op + b + ")";
            return;
        }

        // Select (ternary)
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(&I)) {
            std::string c = val(sel->getCondition());
            std::string t = val(sel->getTrueValue());
            std::string f = val(sel->getFalseValue());
            names[&I] = "(" + c + " ? " + t + " : " + f + ")";
            return;
        }

        // ExtractElement → swizzle
        if (auto* EE = llvm::dyn_cast<llvm::ExtractElementInst>(&I)) {
            std::string vec = val(EE->getVectorOperand());
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(EE->getIndexOperand())) {
                names[&I] = vec + "." + swizzle(ci->getZExtValue());
            } else {
                names[&I] = vec + "[" + val(EE->getIndexOperand()) + "]";
            }
            return;
        }

        // InsertElement → reconstruct vector
        if (auto* IE = llvm::dyn_cast<llvm::InsertElementInst>(&I)) {
            // Track: if building a full vec from undef/zero, emit constructor
            // Otherwise emit component assignment
            std::string vec = val(IE->getOperand(0));
            std::string elem = val(IE->getOperand(1));
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(IE->getOperand(2));

            // Check if this is a chain of inserts building a vec
            if (ci && llvm::isa<llvm::UndefValue>(IE->getOperand(0))) {
                // First insert into undef — start building
                names[&I] = elem; // partial, will be completed by next insert
                return;
            }
            if (ci && llvm::isa<llvm::ConstantAggregateZero>(IE->getOperand(0))) {
                // Insert into zero vector
                unsigned idx = ci->getZExtValue();
                unsigned n = llvm::cast<llvm::FixedVectorType>(IE->getType())->getNumElements();
                std::string tn = glslTypeName(IE->getType());
                if (idx == 0) {
                    names[&I] = elem;
                } else {
                    // Build constructor with zeros filled in
                    names[&I] = tn + "(" + vec + ", " + elem + ")";
                }
                return;
            }

            // Chain of inserts — collect all into a constructor
            // Walk back the insert chain
            std::vector<std::string> elems;
            unsigned n = llvm::cast<llvm::FixedVectorType>(IE->getType())->getNumElements();
            elems.resize(n, "0.0");

            llvm::Value* cur = &I;
            while (auto* ie = llvm::dyn_cast<llvm::InsertElementInst>(cur)) {
                if (auto* idx = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2))) {
                    elems[idx->getZExtValue()] = val(ie->getOperand(1));
                }
                cur = ie->getOperand(0);
            }

            std::string tn = glslTypeName(IE->getType());
            std::string s = tn + "(";
            for (unsigned i = 0; i < n; i++) {
                if (i) s += ", ";
                s += elems[i];
            }
            s += ")";
            names[&I] = s;
            return;
        }

        // SIToFP / FPToSI — float(x) / int(x)
        if (auto* cv = llvm::dyn_cast<llvm::SIToFPInst>(&I)) {
            names[&I] = "float(" + val(cv->getOperand(0)) + ")";
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::FPToSIInst>(&I)) {
            names[&I] = "int(" + val(cv->getOperand(0)) + ")";
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::UIToFPInst>(&I)) {
            names[&I] = "float(" + val(cv->getOperand(0)) + ")";
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::FPExtInst>(&I)) {
            names[&I] = val(cv->getOperand(0));
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::FPTruncInst>(&I)) {
            names[&I] = val(cv->getOperand(0));
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::BitCastInst>(&I)) {
            names[&I] = val(cv->getOperand(0));
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::ZExtInst>(&I)) {
            names[&I] = val(cv->getOperand(0));
            return;
        }
        if (auto* cv = llvm::dyn_cast<llvm::SExtInst>(&I)) {
            names[&I] = val(cv->getOperand(0));
            return;
        }

        // Call — intrinsic or user function
        if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
            llvm::Function* callee = CI->getCalledFunction();
            if (!callee) return;

            // LLVM math intrinsic
            if (callee->isIntrinsic()) {
                std::string fn = mapIntrinsic(callee->getIntrinsicID());
                if (!fn.empty()) {
                    std::string args;
                    for (unsigned i = 0; i < CI->arg_size(); i++) {
                        if (i) args += ", ";
                        args += val(CI->getArgOperand(i));
                    }
                    names[&I] = fn + "(" + args + ")";
                    return;
                }
            }

            // Codegen-state builtin
            std::string mapped = mapCallName(callee->getName());
            if (mapped == "discard") {
                glsl += indent() + "discard;\n";
                return;
            }
            if (mapped == "barrier") {
                glsl += indent() + "barrier();\n";
                return;
            }
            if (!mapped.empty()) {
                std::string args;
                for (unsigned i = 0; i < CI->arg_size(); i++) {
                    if (i) args += ", ";
                    args += val(CI->getArgOperand(i));
                }
                names[&I] = mapped + "(" + args + ")";
                return;
            }

            // User-defined function
            std::string fn = callee->getName().str();
            std::string args;
            for (unsigned i = 0; i < CI->arg_size(); i++) {
                // Skip the _out pointer param
                if (CI->getArgOperand(i)->getType()->isPointerTy()) continue;
                if (!args.empty()) args += ", ";
                args += val(CI->getArgOperand(i));
            }
            if (CI->getType()->isVoidTy()) {
                glsl += indent() + fn + "(" + args + ");\n";
            } else {
                names[&I] = fn + "(" + args + ")";
            }
            return;
        }

        // PHI — handle logical merge (&&/||) and general cases
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
            // Detect short-circuit && pattern:
            //   phi [false, %blockA], [%cmp, %blockB] → (cmpA && cmpB)
            if (phi->getNumIncomingValues() == 2) {
                auto* c0 = llvm::dyn_cast<llvm::ConstantInt>(phi->getIncomingValue(0));
                auto* c1 = llvm::dyn_cast<llvm::ConstantInt>(phi->getIncomingValue(1));
                if (c0 && c0->isZero() && !c1) {
                    // && pattern: false from short-circuit, real value from rhs
                    // Find the condition that branched to logical.rhs
                    llvm::BasicBlock* lhsBB = phi->getIncomingBlock(0);
                    auto* lhsBr = llvm::dyn_cast<llvm::BranchInst>(lhsBB->getTerminator());
                    if (lhsBr && lhsBr->isConditional()) {
                        std::string lhs = val(lhsBr->getCondition());
                        std::string rhs = val(phi->getIncomingValue(1));
                        names[&I] = "(" + lhs + " && " + rhs + ")";
                        return;
                    }
                }
                if (c1 && c1->isOne() && !c0) {
                    // || pattern: true from short-circuit, real value from rhs
                    llvm::BasicBlock* lhsBB = phi->getIncomingBlock(1);
                    auto* lhsBr = llvm::dyn_cast<llvm::BranchInst>(lhsBB->getTerminator());
                    if (lhsBr && lhsBr->isConditional()) {
                        std::string lhs = val(lhsBr->getCondition());
                        std::string rhs = val(phi->getIncomingValue(0));
                        names[&I] = "(" + lhs + " || " + rhs + ")";
                        return;
                    }
                }
            }
            names[&I] = val(phi->getIncomingValue(0));
            return;
        }

        // Branch — handled by structured control flow emitter
        if (llvm::isa<llvm::BranchInst>(I)) return;
        // Return
        if (llvm::isa<llvm::ReturnInst>(I)) return;
    }

    // ── Structured control flow ───────────────────────────────────────────────

    void emitBlock(llvm::BasicBlock* BB, std::set<llvm::BasicBlock*>& visited) {
        if (!BB || visited.count(BB)) return;
        visited.insert(BB);

        std::string name = BB->getName().str();

        // For loop pattern: for.cond → for.body → for.inc → for.cond, with for.end exit
        if (name.find("for.cond") != std::string::npos) {
            emitForLoop(BB, visited);
            return;
        }

        // Logical short-circuit blocks — process instructions silently (just name values)
        // then follow to the merge block
        if (name.find("logical.rhs") != std::string::npos) {
            for (auto& I : *BB) {
                if (&I == BB->getTerminator()) break;
                emitInst(I);
            }
            auto* br = llvm::dyn_cast<llvm::BranchInst>(BB->getTerminator());
            if (br && !br->isConditional()) {
                emitBlock(br->getSuccessor(0), visited);
            }
            return;
        }
        if (name.find("logical.merge") != std::string::npos) {
            // Process PHI and other instructions, then follow successor
            for (auto& I : *BB) {
                emitInst(I);
            }
            auto* br = llvm::dyn_cast<llvm::BranchInst>(BB->getTerminator());
            if (br && br->isConditional()) {
                std::string cond = val(br->getCondition());
                llvm::BasicBlock* thenBB = br->getSuccessor(0);
                llvm::BasicBlock* elseBB = br->getSuccessor(1);
                std::string thenName = thenBB->getName().str();
                std::string elseName = elseBB->getName().str();

                if (thenName.find("then") != std::string::npos) {
                    glsl += indent() + "if (" + cond + ") {\n";
                    indentLevel++;
                    emitBlock(thenBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                    emitBlock(elseBB, visited);
                } else if (elseName.find("ifend") != std::string::npos ||
                           elseName.find("if.end") != std::string::npos) {
                    glsl += indent() + "if (" + cond + ") {\n";
                    indentLevel++;
                    emitBlock(thenBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                    emitBlock(elseBB, visited);
                } else {
                    glsl += indent() + "if (" + cond + ") {\n";
                    indentLevel++;
                    emitBlock(thenBB, visited);
                    indentLevel--;
                    glsl += indent() + "} else {\n";
                    indentLevel++;
                    emitBlock(elseBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                }
            } else if (br && !br->isConditional()) {
                emitBlock(br->getSuccessor(0), visited);
            }
            return;
        }

        // Emit all instructions in the block
        for (auto& I : *BB) {
            emitInst(I);
        }

        // Handle terminator
        auto* term = BB->getTerminator();
        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(term)) {
            if (br->isConditional()) {
                std::string cond = val(br->getCondition());
                llvm::BasicBlock* thenBB = br->getSuccessor(0);
                llvm::BasicBlock* elseBB = br->getSuccessor(1);

                std::string thenName = thenBB->getName().str();
                std::string elseName = elseBB->getName().str();

                // Logical short-circuit: branch to logical.rhs / logical.merge
                // Don't emit if/else — let the logical blocks handle it
                if (thenName.find("logical.rhs") != std::string::npos ||
                    elseName.find("logical.rhs") != std::string::npos) {
                    // Process the logical.rhs block (names values)
                    emitBlock(thenBB, visited);
                    // Then process logical.merge (emits the combined if)
                    emitBlock(elseBB, visited);
                }
                // if/then/ifend pattern
                else if (elseName.find("ifend") != std::string::npos ||
                    elseName.find("if.end") != std::string::npos) {
                    glsl += indent() + "if (" + cond + ") {\n";
                    indentLevel++;
                    emitBlock(thenBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                    emitBlock(elseBB, visited);
                } else if (thenName.find("ifend") != std::string::npos ||
                           thenName.find("if.end") != std::string::npos) {
                    glsl += indent() + "if (!(" + cond + ")) {\n";
                    indentLevel++;
                    emitBlock(elseBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                    emitBlock(thenBB, visited);
                } else if (thenName.find("then") != std::string::npos) {
                    glsl += indent() + "if (" + cond + ") {\n";
                    indentLevel++;
                    emitBlock(thenBB, visited);
                    indentLevel--;
                    if (elseName.find("else") != std::string::npos) {
                        glsl += indent() + "} else {\n";
                        indentLevel++;
                        emitBlock(elseBB, visited);
                        indentLevel--;
                    }
                    glsl += indent() + "}\n";
                    // Find and emit the merge block (ifend)
                    for (auto* succ : llvm::successors(thenBB)) {
                        if (succ->getName().str().find("ifend") != std::string::npos ||
                            succ->getName().str().find("if.end") != std::string::npos) {
                            emitBlock(succ, visited);
                            break;
                        }
                    }
                } else {
                    // Generic conditional — emit as if/else
                    glsl += indent() + "if (" + cond + ") {\n";
                    indentLevel++;
                    emitBlock(thenBB, visited);
                    indentLevel--;
                    glsl += indent() + "} else {\n";
                    indentLevel++;
                    emitBlock(elseBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                }
            } else {
                // Unconditional branch
                llvm::BasicBlock* target = br->getSuccessor(0);
                std::string tName = target->getName().str();
                // Don't follow back-edges (already visited) or branches to for.inc
                if (tName.find("for.inc") == std::string::npos) {
                    emitBlock(target, visited);
                }
            }
        }
    }

    void emitForLoop(llvm::BasicBlock* condBB, std::set<llvm::BasicBlock*>& visited) {
        // Pattern: for.cond has conditional branch to for.body / for.end
        auto* condBr = llvm::dyn_cast<llvm::BranchInst>(condBB->getTerminator());
        if (!condBr || !condBr->isConditional()) {
            // Not the expected pattern — emit as regular block
            for (auto& I : *condBB) emitInst(I);
            return;
        }

        // Emit condition instructions (loads, comparisons) first,
        // so that the condition value gets a proper name
        for (auto& I : *condBB) {
            if (&I == condBB->getTerminator()) break;
            emitInst(I);
        }

        // Now get the condition expression (after instructions are named)
        std::string condExpr = val(condBr->getCondition());

        // Determine for.body and for.end
        llvm::BasicBlock* bodyBB = condBr->getSuccessor(0);
        llvm::BasicBlock* endBB  = condBr->getSuccessor(1);

        // The condition branch may be inverted
        if (bodyBB->getName().str().find("for.end") != std::string::npos) {
            std::swap(bodyBB, endBB);
            condExpr = "!(" + condExpr + ")";
        }

        // Find the loop variable: look for a store in the for.inc block
        // We emit as a while loop since the init is already done before for.cond
        glsl += indent() + "while (" + condExpr + ") {\n";
        indentLevel++;

        // Emit body
        visited.insert(bodyBB);
        for (auto& I : *bodyBB) emitInst(I);

        // Follow body's successor(s) recursively until we reach for.inc
        auto* bodyTerm = bodyBB->getTerminator();
        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(bodyTerm)) {
            if (br->isConditional()) {
                // Body has conditional — if/else inside the loop
                std::string cond2 = val(br->getCondition());
                llvm::BasicBlock* tBB = br->getSuccessor(0);
                llvm::BasicBlock* fBB = br->getSuccessor(1);

                std::string tName = tBB->getName().str();
                std::string fName = fBB->getName().str();

                // If one branch goes to for.inc, emit as simple if
                bool tIsInc = tName.find("for.inc") != std::string::npos;
                bool fIsInc = fName.find("for.inc") != std::string::npos;
                bool tIsEnd = tName.find("ifend") != std::string::npos;
                bool fIsEnd = fName.find("ifend") != std::string::npos;

                if (fIsInc || fIsEnd) {
                    glsl += indent() + "if (" + cond2 + ") {\n";
                    indentLevel++;
                    emitBlock(tBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                    if (fIsEnd) emitBlock(fBB, visited);
                } else if (tIsInc || tIsEnd) {
                    glsl += indent() + "if (!(" + cond2 + ")) {\n";
                    indentLevel++;
                    emitBlock(fBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                    if (tIsEnd) emitBlock(tBB, visited);
                } else {
                    glsl += indent() + "if (" + cond2 + ") {\n";
                    indentLevel++;
                    emitBlock(tBB, visited);
                    indentLevel--;
                    glsl += indent() + "} else {\n";
                    indentLevel++;
                    emitBlock(fBB, visited);
                    indentLevel--;
                    glsl += indent() + "}\n";
                }
            } else {
                llvm::BasicBlock* next = br->getSuccessor(0);
                std::string nName = next->getName().str();
                if (nName.find("for.inc") == std::string::npos &&
                    nName.find("for.cond") == std::string::npos) {
                    emitBlock(next, visited);
                }
            }
        }

        // Emit for.inc block contents (the loop increment)
        for (auto& BB : *condBB->getParent()) {
            if (BB.getName().str().find("for.inc") != std::string::npos &&
                !visited.count(&BB)) {
                visited.insert(&BB);
                for (auto& I : BB) {
                    if (&I == BB.getTerminator()) break;
                    emitInst(I);
                }
                break;
            }
        }

        indentLevel--;
        glsl += indent() + "}\n";

        // Continue with for.end
        emitBlock(endBB, visited);
    }

    // ── Main entry: emit entire module as GLSL ��───────────────────────────────

    std::string emit() {
        glsl.clear();
        glsl += "#version 450\n\n";

        // Find the shader entry function
        llvm::Function* entryFn = nullptr;
        std::string shaderStage;
        for (auto& F : M) {
            if (auto* md = F.getMetadata("shader.stage")) {
                if (auto* mds = llvm::dyn_cast<llvm::MDString>(md->getOperand(0))) {
                    shaderStage = mds->getString().str();
                    entryFn = &F;
                    break;
                }
            }
        }
        if (!entryFn) return "// ERROR: no shader entry found\n";

        // ── Collect uniforms from globals ───���──────────────────────────────────
        std::vector<std::pair<std::string,std::string>> pcUniforms;
        for (auto& G : M.globals()) {
            std::string gn = G.getName().str();
            if (gn.find("_output_floats") != std::string::npos) continue;
            if (gn.find("_varying_floats") != std::string::npos) continue;
            if (gn.find("_total_floats") != std::string::npos) continue;
            if (G.isConstant()) continue;

            llvm::Type* ty = G.getValueType();
            std::string tn = glslTypeName(ty);
            pcUniforms.push_back({tn, gn});
        }

        // Push constant block
        if (!pcUniforms.empty()) {
            glsl += "layout(push_constant) uniform PC {\n";
            for (auto& [t, n] : pcUniforms)
                glsl += "    " + t + " " + n + ";\n";
            glsl += "} pc;\n";
            for (auto& [t, n] : pcUniforms)
                glsl += "#define " + n + " pc." + n + "\n";
            glsl += "\n";
        }

        // ── Stage in/out from function params ──────────────────────────────────
        if (shaderStage == "fragment") {
            // Fragment: params are (vec4 gl_FragCoord, <varyings...>, ptr _out)
            int loc = 0;
            for (unsigned i = 0; i < entryFn->arg_size(); i++) {
                llvm::Argument& arg = *entryFn->getArg(i);
                if (arg.getType()->isPointerTy()) continue; // skip _out
                std::string aname = arg.hasName() ? arg.getName().str() : ("arg" + std::to_string(i));

                if (aname == "gl_FragCoord") continue; // built-in, not declared

                glsl += "layout(location = " + std::to_string(loc++) +
                        ") in " + glslTypeName(arg.getType()) + " " + aname + ";\n";
                names[&arg] = aname;
            }

            // Find output from FS_Output struct
            glsl += "layout(location = 0) out vec4 FragColor;\n\n";
        } else if (shaderStage == "vertex") {
            // Vertex outputs: look at the output struct
            // For now, find stage output vars from function body stores
            int loc = 0;
            // Check if there's a vColor output (common pattern)
            for (auto& BB : *entryFn) {
                for (auto& I : BB) {
                    if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        if (SI->getPointerOperand()->hasName()) {
                            std::string n = SI->getPointerOperand()->getName().str();
                            if (n == "vColor" || n == "vUV") {
                                if (declaredVars.find("out_" + n) == declaredVars.end()) {
                                    llvm::Type* ty = SI->getValueOperand()->getType();
                                    glsl += "layout(location = " + std::to_string(loc++) +
                                            ") out " + glslTypeName(ty) + " " + n + ";\n";
                                    declaredVars.insert("out_" + n);
                                }
                            }
                        }
                    }
                }
            }
            glsl += "\n";
        }

        // ── Emit main function ───��─────────────────────────────────────────────
        glsl += "void main() {\n";

        // Pre-name the function parameters
        for (unsigned i = 0; i < entryFn->arg_size(); i++) {
            llvm::Argument& arg = *entryFn->getArg(i);
            if (arg.getType()->isPointerTy()) continue;
            std::string aname = arg.hasName() ? arg.getName().str() : ("arg" + std::to_string(i));
            names[&arg] = aname;
        }

        // Pre-declare all alloca'd variables at function scope to avoid
        // scoping issues when a variable is first written inside a loop
        // but used again after the loop.
        for (auto& I : entryFn->getEntryBlock()) {
            if (auto* AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                std::string vn = AI->hasName() ? AI->getName().str() : tmpName();
                // Skip shadow allocas for params, _out, and FragColor (declared in header)
                // These have names like gl_FragCoord1, vUV2, _out3
                bool isParamShadow =
                    vn.find("gl_FragCoord") == 0 || vn.find("gl_Position") == 0 ||
                    vn.find("_out") == 0;
                bool isStageVar =
                    vn == "FragColor" || vn == "vUV" || vn == "vColor";
                if (isParamShadow) {
                    // Map back to the clean name for loads
                    std::string cleanName = vn;
                    // Strip LLVM suffix digits from shadow names
                    while (!cleanName.empty() && std::isdigit(cleanName.back())) cleanName.pop_back();
                    names[AI] = cleanName;
                    declaredVars.insert(cleanName);
                    continue;
                }
                if (isStageVar) {
                    names[AI] = vn;
                    declaredVars.insert(vn);
                    continue;
                }
                if (AI->getAllocatedType()->isPointerTy()) {
                    names[AI] = vn;
                    continue;
                }
                std::string tn = glslTypeName(AI->getAllocatedType());
                glsl += indent() + tn + " " + vn + ";\n";
                names[AI] = vn;
                declaredVars.insert(vn);
            }
        }

        // Walk the basic blocks with structured control flow
        std::set<llvm::BasicBlock*> visited;
        emitBlock(&entryFn->getEntryBlock(), visited);

        // Emit final output store
        if (shaderStage == "fragment") {
            // FragColor is written via store to alloca, already emitted
        }

        glsl += "}\n";
        return glsl;
    }
};

/// Translate the LLVM Module to a GLSL 450 source string.
inline std::string emitGLSLFromIR(llvm::Module& M) {
    IRToGLSL emitter(M);
    return emitter.emit();
}
