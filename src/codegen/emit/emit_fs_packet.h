// emit_fs_packet.h — Route B: SPMD-on-SIMD packetizer for fragment and vertex shaders.
#pragma once

#include "../codegen_state/codegen_state.h"
#include "../../frontend/ast/ast.h"
#include "../../frontend/ast/type.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fspacket {

using namespace llvm;

// Fixed packet width matching host runtime vector width
static constexpr unsigned kW = 4;

// Vectorized representation of a GLSL value (SoA layout: K separate <W x T> variables)
struct PacketValue {
    std::vector<Value*> comps;
    const glsl::Type* ty = nullptr;
    bool valid() const { return ty != nullptr && !comps.empty(); }
};

class PacketEmitter {
 public:
    // Main entry point: converts scalar AST into a vectorized LLVM function
    bool run(const std::vector<ExprAST*>& program);

 private:
    struct Slot {
        unsigned off;
        unsigned ncomp;
        const glsl::Type* ty;
    };
    std::map<std::string, Slot> varyings_;  // FS inputs / VS outputs
    std::map<std::string, Slot> outputs_;   // FS outputs

    ShaderStage stage_ = ShaderStage::Fragment;
    Value* varySoa_ = nullptr;  // Pointer to input SoA region
    Value* fragSoa_ = nullptr;  // Fragment coordinates (gl_FragCoord)
    Value* outSoa_ = nullptr;   // Pointer to output SoA region
    Value* vidBase_ = nullptr;  // VS gl_VertexID base for lane 0
    Value* iid_ = nullptr;      // VS gl_InstanceID

    // Allocation trackers for mutable data promoted via mem2reg
    struct LocalVar {
        std::vector<AllocaInst*> slots;
        const glsl::Type* ty = nullptr;
    };
    std::map<std::string, LocalVar> localVars_;
    std::map<std::string, std::vector<AllocaInst*>> outAlloca_;
    BasicBlock* entryBB_ = nullptr;
    bool bailed_ = false;

    // Masking machinery for predicated execution (if-conversion / SPMD-on-SIMD)
    Value* mask_ = nullptr;       // Structural mask (<W x i1>)
    Value* entryMask_ = nullptr;  // Identity mask (all-true)
    Value* live_ = nullptr;       // Liveness mask modified by discard
    Value* liveSoa_ = nullptr;    // Pointer to live_out mask

    // Active tracking masks for loops to handle divergent break/continue
    AllocaInst* la_ = nullptr;  // Lanes active in future loop iterations
    AllocaInst* em_ = nullptr;  // Lanes executing the current loop statement

    Type* f32_ = nullptr;
    Type* i32_ = nullptr;
    Type* i1_ = nullptr;

    // Computes intersecting active lanes (structural mask & control-flow mask)
    Value* effMask() {
        if (!em_) return mask_;
        return Builder->CreateAnd(mask_, Builder->CreateLoad(vty(i1_), em_, "em"), "eff");
    }

    // Blends updates under the current mask: select(mask, new, old)
    Value* blend(Value* newv, Value* oldv) {
        Value* m = effMask();
        return (m == entryMask_) ? newv : Builder->CreateSelect(m, newv, oldv, "msel");
    }

    // Creates an alloca in the entry block to satisfy mem2reg constraints
    AllocaInst* allocaEntry(Type* ty, const char* nm) {
        IRBuilder<> tmp(entryBB_, entryBB_->begin());
        return tmp.CreateAlloca(ty, nullptr, nm);
    }

    // Core type and vectorization layout helpers
    Type* velem(const glsl::Type* t);
    VectorType* vty(Type* elem) { return FixedVectorType::get(elem, kW); }
    Value* splat(Value* scalar) { return Builder->CreateVectorSplat(kW, scalar); }
    unsigned compCount(const glsl::Type* t);
    void bail() { bailed_ = true; }

    Value* loadComp(Value* soa, unsigned slot);
    void storeComp(Value* soa, unsigned slot, Value* v);

    // Expression vectorization passes
    PacketValue emit(ExprAST* e);
    PacketValue emitNumber(NumberExprAST* n);
    PacketValue emitVariable(VariableExprAST* v);
    PacketValue emitBinary(BinaryExprAST* b);
    PacketValue emitUnary(UnaryExprAST* u);
    PacketValue emitMember(MemberAccessExprAST* m);
    PacketValue emitTernary(TernaryExprAST* t);
    PacketValue emitCtor(CallExprAST* c);
    PacketValue emitTexture(CallExprAST* c);
    PacketValue emitBuiltin(CallExprAST* c);

    // Structural generation passes
    void emitStmt(ExprAST* s);
    void emitLoop(ExprAST* cond, ExprAST* body, ExprAST* incr);
};

inline Type* PacketEmitter::velem(const glsl::Type* t) {
    const glsl::Type* e = t->isVector() ? t->elementType() : t;
    if (e->isFloat() || e->isDouble()) return f32_;
    if (e->isBool()) return i1_;
    return i32_;
}

inline unsigned PacketEmitter::compCount(const glsl::Type* t) {
    if (!t) return 0;
    if (t->isScalar()) return 1;
    if (t->isVector()) return t->vectorSize();
    return 0;
}

inline Value* PacketEmitter::loadComp(Value* soa, unsigned slot) {
    Value* p = Builder->CreateInBoundsGEP(vty(f32_), soa, Builder->getInt32(slot), "vp");
    return Builder->CreateLoad(vty(f32_), p, "vload");
}

inline void PacketEmitter::storeComp(Value* soa, unsigned slot, Value* v) {
    Value* p = Builder->CreateInBoundsGEP(vty(f32_), soa, Builder->getInt32(slot), "op");
    Builder->CreateStore(v, p);
}

inline int swizzleIndex(char c) {
    switch (c) {
        case 'x':
        case 'r':
        case 's':
            return 0;
        case 'y':
        case 'g':
        case 't':
            return 1;
        case 'z':
        case 'b':
        case 'p':
            return 2;
        case 'w':
        case 'a':
        case 'q':
            return 3;
        default:
            return -1;
    }
}

inline PacketValue PacketEmitter::emitNumber(NumberExprAST* n) {
    const glsl::Type* t = n->getType();
    if (!t || !t->isScalar()) {
        bail();
        return {};
    }
    Value* s;
    if (t->isFloat())
        s = ConstantFP::get(f32_, n->Val);
    else if (t->isBool())
        s = ConstantInt::get(i1_, n->Val != 0.0);
    else
        s = ConstantInt::get(i32_, (int64_t)n->Val);
    return { { splat(s) }, t };
}

inline PacketValue PacketEmitter::emitVariable(VariableExprAST* v) {
    if (auto it = localVars_.find(v->Name); it != localVars_.end()) {
        LocalVar& lv = it->second;
        Type* et = vty(velem(lv.ty));
        PacketValue pv;
        pv.ty = lv.ty;
        for (AllocaInst* a : lv.slots)
            pv.comps.push_back(Builder->CreateLoad(et, a, "lload"));
        return pv;
    }
    if (auto it = varyings_.find(v->Name); it != varyings_.end()) {
        const Slot& s = it->second;
        PacketValue pv;
        pv.ty = v->getType();
        for (unsigned c = 0; c < s.ncomp; ++c)
            pv.comps.push_back(loadComp(varySoa_, s.off + c));
        return pv;
    }
    if (stage_ == ShaderStage::Fragment && v->Name == "gl_FragCoord") {
        PacketValue pv;
        pv.ty = v->getType();
        for (unsigned c = 0; c < 4; ++c)
            pv.comps.push_back(loadComp(fragSoa_, c));
        return pv;
    }
    if (stage_ == ShaderStage::Vertex && v->Name == "gl_VertexID") {
        std::vector<uint32_t> idx(kW);
        for (unsigned l = 0; l < kW; ++l)
            idx[l] = l;
        Value* iota = ConstantDataVector::get(*Context, idx);
        PacketValue pv;
        pv.ty = v->getType();
        pv.comps.push_back(Builder->CreateAdd(splat(vidBase_), iota, "vid"));
        return pv;
    }
    if (stage_ == ShaderStage::Vertex && v->Name == "gl_InstanceID") {
        return { { splat(iid_) }, v->getType() };
    }
    if (auto it = outAlloca_.find(v->Name); it != outAlloca_.end()) {
        PacketValue pv;
        pv.ty = v->getType();
        for (AllocaInst* slot : it->second)
            pv.comps.push_back(Builder->CreateLoad(vty(f32_), slot, "oload"));
        return pv;
    }
    if (TheModule) {
        if (auto* G = TheModule->getGlobalVariable(v->Name)) {
            const glsl::Type* t = v->getType();
            Value* loaded = Builder->CreateLoad(G->getValueType(), G, v->Name);
            PacketValue pv;
            pv.ty = t;
            unsigned nc = compCount(t);
            if (nc == 0) {
                bail();
                return {};
            }
            if (nc == 1) {
                pv.comps.push_back(splat(loaded));
            } else {
                for (unsigned c = 0; c < nc; ++c)
                    pv.comps.push_back(
                        splat(Builder->CreateExtractElement(loaded, Builder->getInt32(c))));
            }
            return pv;
        }
    }
    bail();
    return {};
}

inline PacketValue PacketEmitter::emitUnary(UnaryExprAST* u) {
    if (u->Op != TokenKind::Minus) {
        bail();
        return {};
    }
    PacketValue o = emit(u->Operand);
    if (bailed_ || !o.valid()) {
        bail();
        return {};
    }
    PacketValue r;
    r.ty = o.ty;
    for (Value* c : o.comps)
        r.comps.push_back(Builder->CreateFNeg(c, "vneg"));
    return r;
}

inline PacketValue PacketEmitter::emitBinary(BinaryExprAST* b) {
    PacketValue l = emit(b->LHS);
    PacketValue r = emit(b->RHS);
    if (bailed_ || !l.valid() || !r.valid()) {
        bail();
        return {};
    }

    auto broadcast = [](PacketValue& a, unsigned n) {
        if (a.comps.size() == 1 && n > 1) a.comps.assign(n, a.comps[0]);
    };
    unsigned n = std::max(l.comps.size(), r.comps.size());
    broadcast(l, n);
    broadcast(r, n);
    if (l.comps.size() != r.comps.size()) {
        bail();
        return {};
    }

    const glsl::Type* et = l.ty->isVector() ? l.ty->elementType() : l.ty;
    bool isFloat = et->isFloat() || et->isDouble();

    PacketValue out;
    auto cmp = [&](CmpInst::Predicate fp, CmpInst::Predicate ip) {
        for (unsigned c = 0; c < n; ++c)
            out.comps.push_back(isFloat ? Builder->CreateFCmp(fp, l.comps[c], r.comps[c], "vcmp")
                                        : Builder->CreateICmp(ip, l.comps[c], r.comps[c], "vcmp"));
        out.ty = b->getType();
    };
    auto arith = [&](Instruction::BinaryOps fop, Instruction::BinaryOps iop) {
        for (unsigned c = 0; c < n; ++c)
            out.comps.push_back(
                Builder->CreateBinOp(isFloat ? fop : iop, l.comps[c], r.comps[c], "vbin"));
        out.ty = (l.ty->isVector() ? l.ty : r.ty);
    };
    switch (b->Op) {
        case TokenKind::Plus:
            arith(Instruction::FAdd, Instruction::Add);
            break;
        case TokenKind::Minus:
            arith(Instruction::FSub, Instruction::Sub);
            break;
        case TokenKind::Multiply:
            arith(Instruction::FMul, Instruction::Mul);
            break;
        case TokenKind::Divide:
            arith(Instruction::FDiv, Instruction::SDiv);
            break;
        case TokenKind::Less:
            cmp(CmpInst::FCMP_OLT, CmpInst::ICMP_SLT);
            break;
        case TokenKind::LessEqual:
            cmp(CmpInst::FCMP_OLE, CmpInst::ICMP_SLE);
            break;
        case TokenKind::Greater:
            cmp(CmpInst::FCMP_OGT, CmpInst::ICMP_SGT);
            break;
        case TokenKind::GreaterEqual:
            cmp(CmpInst::FCMP_OGE, CmpInst::ICMP_SGE);
            break;
        case TokenKind::Equal:
            cmp(CmpInst::FCMP_OEQ, CmpInst::ICMP_EQ);
            break;
        case TokenKind::NotEqual:
            cmp(CmpInst::FCMP_ONE, CmpInst::ICMP_NE);
            break;
        default:
            bail();
            return {};
    }
    return out;
}

inline PacketValue PacketEmitter::emitMember(MemberAccessExprAST* m) {
    PacketValue o = emit(m->Object);
    if (bailed_ || !o.valid()) {
        bail();
        return {};
    }
    PacketValue r;
    for (char c : m->Member) {
        int idx = swizzleIndex(c);
        if (idx < 0 || (unsigned)idx >= o.comps.size()) {
            bail();
            return {};
        }
        r.comps.push_back(o.comps[idx]);
    }
    r.ty = m->getType();
    return r;
}

inline PacketValue PacketEmitter::emitTernary(TernaryExprAST* t) {
    PacketValue cond = emit(t->Cond);
    PacketValue a = emit(t->ThenExpr);
    PacketValue e = emit(t->ElseExpr);
    if (bailed_ || !cond.valid() || !a.valid() || !e.valid()) {
        bail();
        return {};
    }
    if (cond.comps.size() != 1 || a.comps.size() != e.comps.size()) {
        bail();
        return {};
    }
    Value* mask = cond.comps[0];
    PacketValue r;
    r.ty = a.ty;
    for (unsigned c = 0; c < a.comps.size(); ++c)
        r.comps.push_back(Builder->CreateSelect(mask, a.comps[c], e.comps[c], "vsel"));
    return r;
}

inline PacketValue PacketEmitter::emitCtor(CallExprAST* c) {
    const std::string& C = c->Callee;
    unsigned want = (C == "float")  ? 1
                    : (C == "vec2") ? 2
                    : (C == "vec3") ? 3
                    : (C == "vec4") ? 4
                                    : 0;
    if (want == 0) {
        bail();
        return {};
    }
    std::vector<Value*> comps;
    for (ExprAST* arg : c->Args) {
        PacketValue a = emit(arg);
        if (bailed_ || !a.valid()) {
            bail();
            return {};
        }
        const glsl::Type* ae = a.ty->isVector() ? a.ty->elementType() : a.ty;
        bool toFloat = ae && (ae->isInt() || ae->isUint());
        for (Value* cc : a.comps)
            comps.push_back(toFloat ? Builder->CreateSIToFP(cc, vty(f32_), "ctorf") : cc);
    }
    if (comps.size() == 1 && want > 1) comps.assign(want, comps[0]);
    if (comps.size() != want) {
        bail();
        return {};
    }
    PacketValue r;
    r.comps = std::move(comps);
    r.ty = c->getType();
    return r;
}

inline PacketValue PacketEmitter::emitBuiltin(CallExprAST* c) {
    const std::string& F = c->Callee;
    std::vector<PacketValue> a;
    for (ExprAST* arg : c->Args) {
        PacketValue p = emit(arg);
        if (bailed_ || !p.valid()) {
            bail();
            return {};
        }
        a.push_back(p);
    }
    auto bcast = [](PacketValue& p, unsigned n) {
        if (p.comps.size() == 1 && n > 1) p.comps.assign(n, p.comps[0]);
    };
    auto isF = [&](const PacketValue& p) {
        const glsl::Type* et = p.ty->isVector() ? p.ty->elementType() : p.ty;
        return et->isFloat() || et->isDouble();
    };
    auto unary = [&](Intrinsic::ID id) -> PacketValue {
        PacketValue r;
        r.ty = a[0].ty;
        for (Value* cc : a[0].comps)
            r.comps.push_back(Builder->CreateUnaryIntrinsic(id, cc));
        return r;
    };
    auto binop = [&](auto make) -> PacketValue {
        unsigned n = std::max(a[0].comps.size(), a[1].comps.size());
        bcast(a[0], n);
        bcast(a[1], n);
        if (a[0].comps.size() != a[1].comps.size()) {
            bail();
            return {};
        }
        PacketValue r;
        r.ty = (a[0].ty->isVector() ? a[0].ty : a[1].ty);
        for (unsigned i = 0; i < n; ++i)
            r.comps.push_back(make(a[0].comps[i], a[1].comps[i]));
        return r;
    };
    auto dot = [&](const PacketValue& x, const PacketValue& y) -> Value* {
        Value* acc = nullptr;
        for (unsigned i = 0; i < x.comps.size(); ++i) {
            Value* p = Builder->CreateFMul(x.comps[i], y.comps[i]);
            acc = acc ? Builder->CreateFAdd(acc, p) : p;
        }
        return acc;
    };

    if (a.empty() || !isF(a[0])) {
        bail();
        return {};
    }

    if (F == "sin" && a.size() == 1) return unary(Intrinsic::sin);
    if (F == "cos" && a.size() == 1) return unary(Intrinsic::cos);
    if (F == "sqrt" && a.size() == 1) return unary(Intrinsic::sqrt);
    if (F == "floor" && a.size() == 1) return unary(Intrinsic::floor);
    if (F == "exp" && a.size() == 1) return unary(Intrinsic::exp);
    if (F == "log" && a.size() == 1) return unary(Intrinsic::log);
    if (F == "abs" && a.size() == 1) return unary(Intrinsic::fabs);
    if (F == "fract" && a.size() == 1) {
        PacketValue r;
        r.ty = a[0].ty;
        for (Value* cc : a[0].comps)
            r.comps.push_back(Builder->CreateFSub(
                cc, Builder->CreateUnaryIntrinsic(Intrinsic::floor, cc), "fract"));
        return r;
    }
    if (F == "tan" && a.size() == 1) {
        PacketValue r;
        r.ty = a[0].ty;
        for (Value* cc : a[0].comps)
            r.comps.push_back(Builder->CreateFDiv(Builder->CreateUnaryIntrinsic(Intrinsic::sin, cc),
                                                  Builder->CreateUnaryIntrinsic(Intrinsic::cos, cc),
                                                  "tan"));
        return r;
    }
    if (F == "min" && a.size() == 2)
        return binop([&](Value* x, Value* y) {
            return Builder->CreateBinaryIntrinsic(Intrinsic::minnum, x, y);
        });
    if (F == "max" && a.size() == 2)
        return binop([&](Value* x, Value* y) {
            return Builder->CreateBinaryIntrinsic(Intrinsic::maxnum, x, y);
        });
    if (F == "pow" && a.size() == 2)
        return binop([&](Value* x, Value* y) {
            return Builder->CreateBinaryIntrinsic(Intrinsic::pow, x, y);
        });
    if (F == "mod" && a.size() == 2)
        return binop([&](Value* x, Value* y) {
            Value* fl = Builder->CreateUnaryIntrinsic(Intrinsic::floor, Builder->CreateFDiv(x, y));
            return Builder->CreateFSub(x, Builder->CreateFMul(y, fl), "mod");
        });
    if (F == "clamp" && a.size() == 3) {
        unsigned n = a[0].comps.size();
        bcast(a[1], n);
        bcast(a[2], n);
        if (a[1].comps.size() != n || a[2].comps.size() != n) {
            bail();
            return {};
        }
        PacketValue r;
        r.ty = a[0].ty;
        for (unsigned i = 0; i < n; ++i) {
            Value* mx =
                Builder->CreateBinaryIntrinsic(Intrinsic::maxnum, a[0].comps[i], a[1].comps[i]);
            r.comps.push_back(Builder->CreateBinaryIntrinsic(Intrinsic::minnum, mx, a[2].comps[i]));
        }
        return r;
    }
    if (F == "mix" && a.size() == 3) {
        unsigned n = std::max(a[0].comps.size(), a[1].comps.size());
        bcast(a[0], n);
        bcast(a[1], n);
        bcast(a[2], n);
        if (a[0].comps.size() != n || a[1].comps.size() != n || a[2].comps.size() != n) {
            bail();
            return {};
        }
        PacketValue r;
        r.ty = (a[0].ty->isVector() ? a[0].ty : a[1].ty);
        for (unsigned i = 0; i < n; ++i) {
            Value* d = Builder->CreateFSub(a[1].comps[i], a[0].comps[i]);
            r.comps.push_back(
                Builder->CreateFAdd(a[0].comps[i], Builder->CreateFMul(d, a[2].comps[i]), "mix"));
        }
        return r;
    }
    if (F == "dot" && a.size() == 2 && a[0].comps.size() == a[1].comps.size()) {
        PacketValue r;
        r.ty = c->getType();
        r.comps.push_back(dot(a[0], a[1]));
        return r;
    }
    if (F == "length" && a.size() == 1) {
        PacketValue r;
        r.ty = c->getType();
        r.comps.push_back(Builder->CreateUnaryIntrinsic(Intrinsic::sqrt, dot(a[0], a[0])));
        return r;
    }
    if (F == "normalize" && a.size() == 1) {
        Value* len = Builder->CreateUnaryIntrinsic(Intrinsic::sqrt, dot(a[0], a[0]));
        PacketValue r;
        r.ty = a[0].ty;
        for (Value* cc : a[0].comps)
            r.comps.push_back(Builder->CreateFDiv(cc, len, "norm"));
        return r;
    }
    if (F == "step" && a.size() == 2) {
        unsigned n = a[1].comps.size();
        bcast(a[0], n);
        if (a[0].comps.size() != n) {
            bail();
            return {};
        }
        Value *one = ConstantFP::get(vty(f32_), 1.0), *zero = ConstantFP::get(vty(f32_), 0.0);
        PacketValue r;
        r.ty = a[1].ty;
        for (unsigned i = 0; i < n; ++i) {
            Value* ge = Builder->CreateFCmpOGE(a[1].comps[i], a[0].comps[i], "stepge");
            r.comps.push_back(Builder->CreateSelect(ge, one, zero, "step"));
        }
        return r;
    }
    if (F == "smoothstep" && a.size() == 3) {
        unsigned n = a[2].comps.size();
        bcast(a[0], n);
        bcast(a[1], n);
        if (a[0].comps.size() != n || a[1].comps.size() != n) {
            bail();
            return {};
        }
        Value *z = ConstantFP::get(vty(f32_), 0.0), *one = ConstantFP::get(vty(f32_), 1.0);
        Value *three = ConstantFP::get(vty(f32_), 3.0), *two = ConstantFP::get(vty(f32_), 2.0);
        PacketValue r;
        r.ty = a[2].ty;
        for (unsigned i = 0; i < n; ++i) {
            Value* num = Builder->CreateFSub(a[2].comps[i], a[0].comps[i]);
            Value* den = Builder->CreateFSub(a[1].comps[i], a[0].comps[i]);
            Value* t = Builder->CreateFDiv(num, den);
            t = Builder->CreateBinaryIntrinsic(Intrinsic::maxnum, t, z);
            t = Builder->CreateBinaryIntrinsic(Intrinsic::minnum, t, one);
            Value* p = Builder->CreateFSub(three, Builder->CreateFMul(two, t));
            r.comps.push_back(Builder->CreateFMul(Builder->CreateFMul(t, t), p, "smoothstep"));
        }
        return r;
    }
    bail();
    return {};
}

inline PacketValue PacketEmitter::emitTexture(CallExprAST* c) {
    if (c->Args.size() < 2) {
        bail();
        return {};
    }
    PacketValue uv = emit(c->Args[1]);
    if (bailed_ || !uv.valid() || uv.comps.size() != 2) {
        bail();
        return {};
    }

    auto* ptrTy = PointerType::getUnqual(*Context);
    auto* voidTy = Type::getVoidTy(*Context);
    Function* fn = TheModule->getFunction("__tex2d_sample");
    if (!fn) {
        auto* FT = FunctionType::get(voidTy, { ptrTy, f32_, f32_, ptrTy }, false);
        fn = Function::Create(FT, Function::ExternalLinkage, "__tex2d_sample", TheModule.get());
    }
    auto* vec4Ty = FixedVectorType::get(f32_, 4);
    AllocaInst* tmp = allocaEntry(vec4Ty, "tex.lane");
    Value* nullSampler = ConstantPointerNull::get(ptrTy);

    PacketValue r;
    r.ty = c->getType();
    for (unsigned k = 0; k < 4; ++k)
        r.comps.push_back(UndefValue::get(vty(f32_)));
    for (unsigned l = 0; l < kW; ++l) {
        Value* u = Builder->CreateExtractElement(uv.comps[0], Builder->getInt32(l), "u");
        Value* v = Builder->CreateExtractElement(uv.comps[1], Builder->getInt32(l), "v");
        Builder->CreateCall(fn, { nullSampler, u, v, tmp });
        Value* rgba = Builder->CreateLoad(vec4Ty, tmp, "rgba");
        for (unsigned k = 0; k < 4; ++k)
            r.comps[k] = Builder->CreateInsertElement(
                r.comps[k], Builder->CreateExtractElement(rgba, Builder->getInt32(k)),
                Builder->getInt32(l));
    }
    return r;
}

inline PacketValue PacketEmitter::emit(ExprAST* e) {
    if (!e || bailed_) {
        bail();
        return {};
    }
    using K = ExprAST::Kind;
    switch (e->getKind()) {
        case K::Number:
            return emitNumber(llvm::cast<NumberExprAST>(e));
        case K::Boolean: {
            auto* be = llvm::cast<BooleanExprAST>(e);
            return { { splat(ConstantInt::get(i1_, be->Val)) }, e->getType() };
        }
        case K::Variable:
            return emitVariable(llvm::cast<VariableExprAST>(e));
        case K::Unary:
            return emitUnary(llvm::cast<UnaryExprAST>(e));
        case K::Binary:
            return emitBinary(llvm::cast<BinaryExprAST>(e));
        case K::MemberAccess:
            return emitMember(llvm::cast<MemberAccessExprAST>(e));
        case K::Ternary:
            return emitTernary(llvm::cast<TernaryExprAST>(e));
        case K::Call: {
            auto* c = llvm::cast<CallExprAST>(e);
            const std::string& F = c->Callee;
            if (F == "texture") return emitTexture(c);
            if (F == "float" || F == "vec2" || F == "vec3" || F == "vec4") return emitCtor(c);
            return emitBuiltin(c);  // math builtins; bails if unknown
        }
        case K::ImplicitCast: {
            // Widening that doesn't change packet shape: pass through
            // float<-float; int->float per component.
            auto* ic = llvm::cast<ImplicitCastExprAST>(e);
            PacketValue in = emit(ic->Operand);
            if (bailed_ || !in.valid()) {
                bail();
                return {};
            }
            const glsl::Type* dt = ic->getType();
            const glsl::Type* de = dt->isVector() ? dt->elementType() : dt;
            const glsl::Type* se = in.ty->isVector() ? in.ty->elementType() : in.ty;
            if (se->isFloat() && de->isFloat()) {
                in.ty = dt;
                return in;
            }
            if ((se->isInt() || se->isUint()) && de->isFloat()) {
                PacketValue r;
                r.ty = dt;
                for (Value* cc : in.comps)
                    r.comps.push_back(Builder->CreateSIToFP(cc, vty(f32_), "vcvt"));
                return r;
            }
            bail();
            return {};
        }
        default:
            bail();
            return {};
    }
}

inline void PacketEmitter::emitStmt(ExprAST* s) {
    if (!s || bailed_) {
        bail();
        return;
    }
    using K = ExprAST::Kind;
    switch (s->getKind()) {
        case K::Block:
            for (ExprAST* st : llvm::cast<BlockExprAST>(s)->Statements)
                emitStmt(st);
            return;
        case K::Assignment: {
            auto* a = llvm::cast<AssignmentExprAST>(s);
            PacketValue v = emit(a->Init);
            if (bailed_ || !v.valid()) {
                bail();
                return;
            }
            if (auto it = outAlloca_.find(a->VarName); it != outAlloca_.end()) {
                std::vector<AllocaInst*>& acc = it->second;
                if (v.comps.size() != acc.size()) {
                    bail();
                    return;
                }
                Type* et = vty(f32_);
                for (unsigned c = 0; c < acc.size(); ++c)
                    Builder->CreateStore(blend(v.comps[c], Builder->CreateLoad(et, acc[c], "old")),
                                         acc[c]);
                return;
            }
            LocalVar& lv = localVars_[a->VarName];
            if (lv.slots.empty()) {
                lv.ty = v.ty;
                for (unsigned c = 0; c < v.comps.size(); ++c)
                    lv.slots.push_back(allocaEntry(vty(velem(v.ty)), "loc"));
            }
            if (lv.slots.size() != v.comps.size()) {
                bail();
                return;
            }
            Type* et = vty(velem(lv.ty));
            for (unsigned c = 0; c < v.comps.size(); ++c)
                Builder->CreateStore(blend(v.comps[c], Builder->CreateLoad(et, lv.slots[c], "old")),
                                     lv.slots[c]);
            return;
        }
        case K::MemberAssignment: {
            auto* ma = llvm::cast<MemberAssignmentExprAST>(s);
            auto* obj = llvm::dyn_cast<VariableExprAST>(ma->Object);
            if (!obj) {
                bail();
                return;
            }
            auto it = outAlloca_.find(obj->Name);
            if (it == outAlloca_.end()) {
                bail();
                return;
            }
            PacketValue v = emit(ma->Init);
            if (bailed_ || !v.valid() || v.comps.size() != ma->Member.size()) {
                bail();
                return;
            }
            std::vector<AllocaInst*>& acc = it->second;
            Type* et = vty(f32_);
            for (unsigned k = 0; k < ma->Member.size(); ++k) {
                int idx = swizzleIndex(ma->Member[k]);
                if (idx < 0 || (unsigned)idx >= acc.size()) {
                    bail();
                    return;
                }
                Builder->CreateStore(blend(v.comps[k], Builder->CreateLoad(et, acc[idx], "old")),
                                     acc[idx]);
            }
            return;
        }
        case K::If: {
            auto* iff = llvm::cast<IfExprAST>(s);
            PacketValue cond = emit(iff->Condition);
            if (bailed_ || !cond.valid() || cond.comps.size() != 1) {
                bail();
                return;
            }
            Value* condMask = cond.comps[0];
            Value* saved = mask_;
            mask_ = Builder->CreateAnd(saved, condMask, "then.mask");
            emitStmt(iff->ThenExpr);
            if (iff->ElseExpr) {
                mask_ =
                    Builder->CreateAnd(saved, Builder->CreateNot(condMask, "ncond"), "else.mask");
                emitStmt(iff->ElseExpr);
            }
            mask_ = saved;
            return;
        }
        case K::Discard: {
            live_ = Builder->CreateAnd(live_, Builder->CreateNot(effMask(), "ndisc"), "live");
            return;
        }
        case K::For: {
            auto* f = llvm::cast<ForExprAST>(s);
            emitStmt(f->Init);
            emitLoop(f->Condition, f->Body, f->Increment);
            return;
        }
        case K::While: {
            auto* w = llvm::cast<WhileExprAST>(s);
            emitLoop(w->Condition, w->Body, nullptr);
            return;
        }
        case K::Break: {
            if (!la_ || !em_) {
                bail();
                return;
            }
            Value* e = effMask();
            Value* ne = Builder->CreateNot(e, "nbrk");
            Builder->CreateStore(
                Builder->CreateAnd(Builder->CreateLoad(vty(i1_), em_, "em"), ne, "em.brk"), em_);
            Builder->CreateStore(
                Builder->CreateAnd(Builder->CreateLoad(vty(i1_), la_, "la"), ne, "la.brk"), la_);
            return;
        }
        case K::Continue: {
            if (!em_) {
                bail();
                return;
            }
            Value* ne = Builder->CreateNot(effMask(), "ncont");
            Builder->CreateStore(
                Builder->CreateAnd(Builder->CreateLoad(vty(i1_), em_, "em"), ne, "em.cont"), em_);
            return;
        }
        default:
            bail();
            return;
    }
}

inline void PacketEmitter::emitLoop(ExprAST* cond, ExprAST* body, ExprAST* incr) {
    if (bailed_) return;
    Value* enterMask = mask_;
    AllocaInst* la = allocaEntry(vty(i1_), "loop.active");
    AllocaInst* em = allocaEntry(vty(i1_), "loop.iter");
    Builder->CreateStore(enterMask, la);
    AllocaInst *savedLa = la_, *savedEm = em_;
    la_ = la;
    em_ = em;

    Function* F = entryBB_->getParent();
    auto* hdr = BasicBlock::Create(*Context, "loop.header", F);
    auto* bodyBB = BasicBlock::Create(*Context, "loop.body", F);
    auto* exitBB = BasicBlock::Create(*Context, "loop.exit", F);
    Builder->CreateBr(hdr);

    Builder->SetInsertPoint(hdr);
    Value* active = Builder->CreateLoad(vty(i1_), la, "active");
    mask_ = active;
    em_ = nullptr;
    PacketValue c = emit(cond);
    em_ = em;
    if (bailed_ || !c.valid() || c.comps.size() != 1) {
        la_ = savedLa;
        em_ = savedEm;
        bail();
        return;
    }
    Value* na = Builder->CreateAnd(active, c.comps[0], "active.cond");
    Builder->CreateStore(na, la);
    Builder->CreateCondBr(Builder->CreateOrReduce(na), bodyBB, exitBB);

    Builder->SetInsertPoint(bodyBB);
    Builder->CreateStore(Builder->CreateLoad(vty(i1_), la, "iter0"), em);
    mask_ = Builder->CreateLoad(vty(i1_), la, "body.base");
    emitStmt(body);
    if (incr) {
        em_ = nullptr;
        mask_ = Builder->CreateLoad(vty(i1_), la, "incr.mask");
        emitStmt(incr);
        em_ = em;
    }
    if (!Builder->GetInsertBlock()->getTerminator()) Builder->CreateBr(hdr);

    Builder->SetInsertPoint(exitBB);
    la_ = savedLa;
    em_ = savedEm;
    mask_ = enterMask;
}

inline bool PacketEmitter::run(const std::vector<ExprAST*>& program) {
    if (!Context || !Builder || !TheModule) return false;
    f32_ = Type::getFloatTy(*Context);
    i32_ = Type::getInt32Ty(*Context);
    i1_ = Type::getInt1Ty(*Context);

    FunctionAST* entry = nullptr;
    for (ExprAST* n : program) {
        if (auto* fn = llvm::dyn_cast_or_null<FunctionAST>(n))
            if (fn->Attrs.isEntry && fn->Attrs.stage &&
                (*fn->Attrs.stage == ShaderStage::Fragment ||
                 *fn->Attrs.stage == ShaderStage::Vertex)) {
                entry = fn;
                break;
            }
    }
    if (!entry || !entry->Body) return false;
    stage_ = *entry->Attrs.stage;
    const bool isVS = (stage_ == ShaderStage::Vertex);

    auto compOf = [&](const std::string& tn) -> unsigned {
        if (tn == "float") return 1;
        if (tn == "vec2") return 2;
        if (tn == "vec3") return 3;
        if (tn == "vec4") return 4;
        return 0;
    };

    unsigned vOff = 0, oOff = 0;
    if (isVS) {
        outputs_["gl_Position"] = { 0, 4, nullptr };
        oOff = 4;
    }
    for (ExprAST* n : program) {
        auto* sv = llvm::dyn_cast_or_null<StageVarDeclAST>(n);
        if (!sv) continue;
        unsigned nc = compOf(sv->TypeName);
        if (nc == 0) return false;
        if (sv->isInput) {
            varyings_[sv->Name] = { vOff, nc, nullptr };
            vOff += nc;
        } else {
            outputs_[sv->Name] = { oOff, nc, nullptr };
            oOff += nc;
        }
    }

    auto* ptrTy = PointerType::getUnqual(*Context);
    auto* voidTy = Type::getVoidTy(*Context);
    const char* fname = isVS ? "vs_packet" : "fs_packet";
    if (TheModule->getFunction(fname)) return false;
    auto* fnTy = isVS ? FunctionType::get(voidTy, { ptrTy, i32_, i32_, ptrTy }, false)
                      : FunctionType::get(voidTy, { ptrTy, ptrTy, ptrTy, ptrTy }, false);
    auto* F = Function::Create(fnTy, Function::ExternalLinkage, fname, TheModule.get());
    auto ai = F->arg_begin();
    if (isVS) {
        varySoa_ = &*ai++;
        varySoa_->setName("in_soa");
        vidBase_ = &*ai++;
        vidBase_->setName("vid_base");
        iid_ = &*ai++;
        iid_->setName("iid");
        outSoa_ = &*ai;
        outSoa_->setName("out_soa");
    } else {
        varySoa_ = &*ai++;
        varySoa_->setName("vary_soa");
        fragSoa_ = &*ai++;
        fragSoa_->setName("frag_soa");
        outSoa_ = &*ai++;
        outSoa_->setName("out_soa");
        liveSoa_ = &*ai;
        liveSoa_->setName("live_out");
    }

    auto* saved = Builder->GetInsertBlock();
    auto* BB = BasicBlock::Create(*Context, "entry", F);
    Builder->SetInsertPoint(BB);
    entryBB_ = BB;

    entryMask_ = Constant::getAllOnesValue(vty(i1_));
    mask_ = entryMask_;
    live_ = entryMask_;
    for (auto& [name, sl] : outputs_) {
        auto& slots = outAlloca_[name];
        for (unsigned c = 0; c < sl.ncomp; ++c) {
            AllocaInst* a = allocaEntry(vty(f32_), "out");
            Builder->CreateStore(ConstantAggregateZero::get(vty(f32_)), a);
            slots.push_back(a);
        }
    }

    emitStmt(entry->Body);

    if (!bailed_) {
        for (auto& [name, sl] : outputs_)
            for (unsigned c = 0; c < sl.ncomp; ++c)
                storeComp(outSoa_, sl.off + c,
                          Builder->CreateLoad(vty(f32_), outAlloca_[name][c], "outf"));
        if (!isVS) {
            Value* liveI32 = Builder->CreateSExt(live_, vty(i32_), "live.i32");
            Builder->CreateStore(liveI32, liveSoa_);
        }
    }

    Builder->CreateRetVoid();
    if (saved) Builder->SetInsertPoint(saved);

    if (bailed_) {
        F->eraseFromParent();
        return false;
    }
    return true;
}

}  // namespace fspacket