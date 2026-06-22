// emit_spirv_from_ir.h — Walk LLVM IR and emit a SPIR-V binary directly.
//
// No glslang, no llvm-spirv, no LLVM SPIRV target. Hand-written 1:1 lowering
// of the IR our codegen produces (ast.cpp + emit_trampolines.h) to SPIR-V
// opcodes via the official Khronos SPIR-V Headers.
//
// Pipeline replaces:  IR -> emit_glsl_from_ir -> glslangValidator -> .spv
// With:               IR -> emit_spirv_from_ir -> .spv
//
// Coverage targets every fragment / vertex / compute shader the project ships.

#pragma once

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../parser/parser.h"  // for ShaderStage

namespace spv_emit {

using Word = uint32_t;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

// ─────────────────────────────────────────────────────────────────────────────
// Word-stream helpers
// ─────────────────────────────────────────────────────────────────────────────
inline void appendOp(std::vector<Word>& dst, SpvOp op,
                     std::initializer_list<Word> ops) {
  Word wordCount = 1u + (Word)ops.size();
  dst.push_back((wordCount << 16) | (Word)op);
  for (Word w : ops) dst.push_back(w);
}

inline void appendOpV(std::vector<Word>& dst, SpvOp op,
                      const std::vector<Word>& ops) {
  Word wordCount = 1u + (Word)ops.size();
  dst.push_back((wordCount << 16) | (Word)op);
  for (Word w : ops) dst.push_back(w);
}

// Pack a UTF-8 string into 32-bit little-endian words, NUL-terminated, padded.
inline void packStringInto(std::vector<Word>& dst, llvm::StringRef s) {
  size_t n = s.size();
  size_t totalBytes = n + 1;
  size_t totalWords = (totalBytes + 3) / 4;
  for (size_t w = 0; w < totalWords; ++w) {
    Word v = 0;
    for (size_t b = 0; b < 4; ++b) {
      size_t i = w * 4 + b;
      unsigned char ch = (i < n) ? (unsigned char)s[i] : 0;
      v |= (Word)ch << (b * 8);
    }
    dst.push_back(v);
  }
}

inline std::vector<Word> packString(llvm::StringRef s) {
  std::vector<Word> out;
  packStringInto(out, s);
  return out;
}

// OpName / OpString / OpEntryPoint etc. encode trailing string as packed words.
inline void appendOpStr(std::vector<Word>& dst, SpvOp op,
                        const std::vector<Word>& prefix, llvm::StringRef str) {
  std::vector<Word> packed = packString(str);
  Word wordCount = 1u + (Word)prefix.size() + (Word)packed.size();
  dst.push_back((wordCount << 16) | (Word)op);
  for (Word w : prefix) dst.push_back(w);
  for (Word w : packed) dst.push_back(w);
}

// ─────────────────────────────────────────────────────────────────────────────
// Translator
// ─────────────────────────────────────────────────────────────────────────────
class IRToSPIRV {
 public:
  IRToSPIRV(llvm::Module& mod, ShaderStage st) : M(mod), stage(st) {}

  // Emit and return the binary as a byte vector.
  std::vector<uint8_t> emit();

 private:
  llvm::Module& M;
  ShaderStage stage;

  // ── ID management ─────────────────────────────────────────────────────────
  Word nextID = 1;
  Word allocID() { return nextID++; }

  llvm::DenseMap<llvm::Value*, Word> valueIDs;  // LLVM value -> SPIR-V id
  llvm::DenseMap<llvm::Type*, Word> typeIDs;    // base types
  llvm::DenseMap<uint64_t, Word>
      ptrTypeCache;  // (sc << 32) | typeID -> ptr type id
  llvm::DenseMap<uint64_t, Word> intConstCache;         // (typeID << 32) | bits
  llvm::DenseMap<uint64_t, Word> fpConstCache;          // (typeID << 32) | bits
  llvm::DenseMap<llvm::Constant*, Word> aggConstCache;  // composite constants

  // ── Sections (output ordering matters in SPIR-V) ─────────────────────────
  std::vector<Word> capabilities;
  std::vector<Word> extInstImports;
  std::vector<Word> memoryModel;
  std::vector<Word> entryPoint;
  std::vector<Word> executionMode;
  std::vector<Word> debugSource;
  std::vector<Word> debugNames;
  std::vector<Word> annotations;   // OpDecorate / OpMemberDecorate
  std::vector<Word> typesGlobals;  // OpType*, OpConstant*, OpVariable (global)
  std::vector<Word> functions;     // OpFunction ... OpFunctionEnd

  // ── Cached well-known IDs ─────────────────────────────────────────────────
  Word glslExtID = 0;
  Word voidTypeID = 0;
  Word boolTypeID = 0;
  Word floatTypeID = 0;
  Word int32TypeID = 0;
  Word uint32TypeID = 0;

  // ── Push-constant block ──────────────────────────────────────────────────
  Word pushConstStructTypeID = 0;
  Word pushConstVarID = 0;
  Word pushConstStructPtrID = 0;  // ptr to struct (PushConstant)
  llvm::DenseMap<llvm::GlobalVariable*, uint32_t>
      uniformMemberIdx;  // global -> member index

  // ── SSBO descriptors (compute storage buffers) ───────────────────────────
  struct SSBOInfo {
    Word varID = 0;          // OpVariable id (Uniform storage class)
    Word elemPtrTypeID = 0;  // OpTypePointer Uniform <elem> (for OpAccessChain)
    uint32_t binding = 0;
  };
  llvm::DenseMap<llvm::GlobalVariable*, SSBOInfo> ssboInfo;
  // Tracks `%p = load ptr, ptr @ssbo_global` results so that GEPs through %p
  // know which SSBO they're indexing into.
  llvm::DenseMap<llvm::Value*, llvm::GlobalVariable*> valueToSSBO;

  // ── Combined image-sampler descriptors (sampler2D uniforms) ──────────────
  // sampler2Ds appear in IR as opaque-pointer externals (`@tex = global ptr`)
  // whose loads are consumed by `__tex2d_sample(ptr, float, float)` calls.
  struct SamplerInfo {
    Word varID = 0;             // OpVariable id (UniformConstant storage class)
    Word sampledImageTypeID = 0;
    uint32_t binding = 0;
  };
  llvm::DenseMap<llvm::GlobalVariable*, SamplerInfo> samplerInfo;
  // Tracks `%p = load ptr, ptr @sampler_global` so that subsequent
  // `__tex2d_sample(%p, …)` calls know which OpVariable to use.
  llvm::DenseMap<llvm::Value*, llvm::GlobalVariable*> valueToSampler;

  // ── Stage IO ─────────────────────────────────────────────────────────────
  // Map function-arg/alloca → input/output OpVariable id.
  llvm::DenseMap<llvm::Argument*, Word> argInputVarID;
  llvm::DenseMap<llvm::AllocaInst*, Word>
      allocaToOutputVar;  // FragColor/gl_Position
  llvm::DenseMap<llvm::AllocaInst*, Word>
      allocaToInputVar;                   // shadow allocas for inputs
  std::vector<Word> entryPointInterface;  // Input/Output var ids

  // ── Trampoline / dead value tracking ──────────────────────────────────────
  std::set<llvm::Value*> deadValues;  // values from `_out` chain — skip stores

  // ── The entry function and its info ──────────────────────────────────────
  llvm::Function* entryFn = nullptr;
  Word entryFnID = 0;

  // ── CFG / structurization ─────────────────────────────────────────────────
  // header BB -> merge BB id (for OpSelectionMerge / OpLoopMerge)
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> selectionMerge;
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> loopMerge;
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> loopContinue;

  // ─────────────────────────────────────────────────────────────────────────
  // Type / constant builders
  // ─────────────────────────────────────────────────────────────────────────
  Word typeFor(llvm::Type* t);
  Word ptrTypeFor(llvm::Type* pointee, SpvStorageClass sc);
  Word funcTypeFor(llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params);
  Word structTypeFor(llvm::ArrayRef<Word> memberTypeIDs);

  Word constInt(llvm::Type* ty, int64_t v);
  Word constUint(uint32_t v) {
    return constInt(llvm::Type::getInt32Ty(M.getContext()), (int64_t)v);
  }
  Word constFP(llvm::Type* ty, double v);
  Word constBool(bool v);
  Word constComposite(llvm::Type* ty, llvm::ArrayRef<Word> elems);
  Word constNull(llvm::Type* ty);
  Word lowerConstant(llvm::Constant* c);

  Word valueOf(llvm::Value* v);

  // ─────────────────────────────────────────────────────────────────────────
  // Module setup
  // ─────────────────────────────────────────────────────────────────────────
  void emitCapabilitiesAndModel();
  void analyzeUniforms();  // build push-constant block
  void analyzeSSBOs();     // emit StorageBuffer descriptors
  void analyzeSamplers();  // emit combined image-sampler descriptors
  void emitStageIO();      // gl_FragCoord, vUV, FragColor, ...
  void emitFunction(llvm::Function& F);

  // ─────────────────────────────────────────────────────────────────────────
  // Function emission
  // ─────────────────────────────────────────────────────────────────────────
  void analyzeCFG(llvm::Function& F);
  void emitBlockBody(llvm::BasicBlock& BB, std::vector<Word>& out);
  void emitInstruction(llvm::Instruction& I, std::vector<Word>& out);
  void emitTerminator(llvm::BasicBlock& BB, std::vector<Word>& out);

  // ─────────────────────────────────────────────────────────────────────────
  // Helpers
  // ─────────────────────────────────────────────────────────────────────────
  bool isExtInstFunction(llvm::Function* F, GLSLstd450& outOp);
  bool isTrampolineFunction(llvm::Function& F);
  bool isUniformGlobal(llvm::GlobalVariable& G);
  bool isSSBOGlobal(llvm::GlobalVariable& G);
  bool isSamplerGlobal(llvm::GlobalVariable& G);
  bool isPointerAlloca(llvm::AllocaInst* AI);

  // After IR codegen the stage entry function is named
  // "fs_main"/"vs_main"/"cs_main"
  llvm::Function* findStageEntry();

  // Names used by the codegen for stage I/O (heuristic, but stable)
  static bool isFragColorAlloca(llvm::AllocaInst* AI);
  static bool isOutputAlloca(llvm::AllocaInst* AI, std::string& outName);
  static bool isShadowAllocaForArg(llvm::AllocaInst* AI,
                                   llvm::Argument*& outArg, llvm::Function& F);

  SpvBuiltIn builtinForName(llvm::StringRef name);

  // Splat / vector helpers
  Word splatToVector(Word scalarID, llvm::Type* dstVecType,
                     std::vector<Word>& out);

  // SPIR-V unsigned ops (OpUMod, OpUDiv, OpULessThan, ...) require their
  // operands and result to use a Signedness=0 integer type. Our type system
  // emits all i32 as signed; this helper lazily allocates the matching
  // unsigned i32 so we can OpBitcast through it.
  Word uintTypeFor();
};

// ─────────────────────────────────────────────────────────────────────────────
// Implementation
// ─────────────────────────────────────────────────────────────────────────────
inline Word IRToSPIRV::typeFor(llvm::Type* t) {
  auto it = typeIDs.find(t);
  if (it != typeIDs.end()) return it->second;

  Word id = 0;
  if (t->isVoidTy()) {
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeVoid, {id});
    voidTypeID = id;
  } else if (t->isFloatTy()) {
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeFloat, {id, 32});
    floatTypeID = id;
  } else if (t->isDoubleTy()) {
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeFloat, {id, 64});
  } else if (t->isIntegerTy(1)) {
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeBool, {id});
    boolTypeID = id;
  } else if (t->isIntegerTy(32)) {
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeInt, {id, 32, 1});  // signed
    int32TypeID = id;
  } else if (t->isIntegerTy()) {
    unsigned bw = t->getIntegerBitWidth();
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeInt, {id, bw, 1});
  } else if (auto* vt = dyn_cast<llvm::FixedVectorType>(t)) {
    Word elemID = typeFor(vt->getElementType());
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeVector,
             {id, elemID, (Word)vt->getNumElements()});
  } else if (auto* at = dyn_cast<llvm::ArrayType>(t)) {
    Word elemID = typeFor(at->getElementType());
    Word countID = constUint((uint32_t)at->getNumElements());
    id = allocID();
    appendOp(typesGlobals, SpvOpTypeArray, {id, elemID, countID});
  } else if (auto* st = dyn_cast<llvm::StructType>(t)) {
    std::vector<Word> memberIDs;
    for (auto* mt : st->elements()) memberIDs.push_back(typeFor(mt));
    id = allocID();
    std::vector<Word> ops;
    ops.push_back(id);
    for (Word w : memberIDs) ops.push_back(w);
    appendOpV(typesGlobals, SpvOpTypeStruct, ops);
  } else if (t->isPointerTy()) {
    // Opaque pointer: default to Function storage if requested standalone.
    // Most uses go through ptrTypeFor() which knows the storage class.
    id = ptrTypeFor(llvm::Type::getInt32Ty(M.getContext()),
                    SpvStorageClassFunction);
  } else {
    llvm::errs() << "[spv] unsupported type: ";
    t->print(llvm::errs());
    llvm::errs() << "\n";
    return 0;
  }
  typeIDs[t] = id;
  return id;
}

inline Word IRToSPIRV::ptrTypeFor(llvm::Type* pointee, SpvStorageClass sc) {
  Word pteID = typeFor(pointee);
  uint64_t key = ((uint64_t)sc << 32) | (uint64_t)pteID;
  auto it = ptrTypeCache.find(key);
  if (it != ptrTypeCache.end()) return it->second;
  Word id = allocID();
  appendOp(typesGlobals, SpvOpTypePointer, {id, (Word)sc, pteID});
  ptrTypeCache[key] = id;
  return id;
}

inline Word IRToSPIRV::funcTypeFor(llvm::Type* ret,
                                   llvm::ArrayRef<llvm::Type*> params) {
  Word retID = typeFor(ret);
  std::vector<Word> ops;
  ops.push_back(0);
  ops.push_back(retID);
  for (auto* p : params) ops.push_back(typeFor(p));
  Word id = allocID();
  ops[0] = id;
  appendOpV(typesGlobals, SpvOpTypeFunction, ops);
  return id;
}

inline Word IRToSPIRV::structTypeFor(llvm::ArrayRef<Word> memberTypeIDs) {
  Word id = allocID();
  std::vector<Word> ops;
  ops.push_back(id);
  for (Word w : memberTypeIDs) ops.push_back(w);
  appendOpV(typesGlobals, SpvOpTypeStruct, ops);
  return id;
}

inline Word IRToSPIRV::constInt(llvm::Type* ty, int64_t v) {
  Word tid = typeFor(ty);
  uint64_t bits = (uint64_t)(uint32_t)v;
  uint64_t key = ((uint64_t)tid << 32) | bits;
  auto it = intConstCache.find(key);
  if (it != intConstCache.end()) return it->second;
  Word id = allocID();
  appendOp(typesGlobals, SpvOpConstant, {tid, id, (Word)v});
  intConstCache[key] = id;
  return id;
}

inline Word IRToSPIRV::constFP(llvm::Type* ty, double v) {
  Word tid = typeFor(ty);
  Word w;
  if (ty->isFloatTy()) {
    float f = (float)v;
    std::memcpy(&w, &f, sizeof w);
  } else {
    // For doubles we'd emit two words; not needed here.
    float f = (float)v;
    std::memcpy(&w, &f, sizeof w);
  }
  uint64_t key = ((uint64_t)tid << 32) | (uint64_t)w;
  auto it = fpConstCache.find(key);
  if (it != fpConstCache.end()) return it->second;
  Word id = allocID();
  appendOp(typesGlobals, SpvOpConstant, {tid, id, w});
  fpConstCache[key] = id;
  return id;
}

inline Word IRToSPIRV::constBool(bool v) {
  Word tid = typeFor(llvm::Type::getInt1Ty(M.getContext()));
  Word id = allocID();
  appendOp(typesGlobals, v ? SpvOpConstantTrue : SpvOpConstantFalse, {tid, id});
  return id;
}

inline Word IRToSPIRV::constComposite(llvm::Type* ty,
                                      llvm::ArrayRef<Word> elems) {
  Word tid = typeFor(ty);
  Word id = allocID();
  std::vector<Word> ops;
  ops.push_back(tid);
  ops.push_back(id);
  for (Word w : elems) ops.push_back(w);
  appendOpV(typesGlobals, SpvOpConstantComposite, ops);
  return id;
}

inline Word IRToSPIRV::constNull(llvm::Type* ty) {
  Word tid = typeFor(ty);
  Word id = allocID();
  appendOp(typesGlobals, SpvOpConstantNull, {tid, id});
  return id;
}

inline Word IRToSPIRV::lowerConstant(llvm::Constant* c) {
  auto it = aggConstCache.find(c);
  if (it != aggConstCache.end()) return it->second;
  Word id = 0;
  if (auto* ci = dyn_cast<llvm::ConstantInt>(c)) {
    if (ci->getType()->isIntegerTy(1))
      id = constBool(ci->getZExtValue() != 0);
    else
      id = constInt(ci->getType(), ci->getSExtValue());
  } else if (auto* cf = dyn_cast<llvm::ConstantFP>(c)) {
    id = constFP(cf->getType(), cf->getValueAPF().convertToDouble());
  } else if (isa<llvm::ConstantAggregateZero>(c)) {
    id = constNull(c->getType());
  } else if (isa<llvm::UndefValue>(c)) {
    id = constNull(c->getType());  // safe default
  } else if (auto* cv = dyn_cast<llvm::ConstantVector>(c)) {
    std::vector<Word> elems;
    for (unsigned i = 0; i < cv->getType()->getNumElements(); ++i)
      elems.push_back(lowerConstant(cv->getAggregateElement(i)));
    id = constComposite(c->getType(), elems);
  } else if (auto* cda = dyn_cast<llvm::ConstantDataVector>(c)) {
    std::vector<Word> elems;
    for (unsigned i = 0, n = cda->getNumElements(); i < n; ++i)
      elems.push_back(lowerConstant(cda->getElementAsConstant(i)));
    id = constComposite(c->getType(), elems);
  } else {
    llvm::errs() << "[spv] unsupported constant: ";
    c->print(llvm::errs());
    llvm::errs() << "\n";
    id = constNull(c->getType());
  }
  aggConstCache[c] = id;
  return id;
}

inline Word IRToSPIRV::valueOf(llvm::Value* v) {
  auto it = valueIDs.find(v);
  if (it != valueIDs.end()) return it->second;

  // Functions, basic blocks, args, instructions, globals — just allocate an id
  // and remember it. lowerConstant() is only for actual literal constants.
  if (isa<llvm::Function>(v) || isa<llvm::BasicBlock>(v) ||
      isa<llvm::Argument>(v) || isa<llvm::Instruction>(v) ||
      isa<llvm::GlobalVariable>(v)) {
    Word id = allocID();
    valueIDs[v] = id;
    return id;
  }

  if (auto* c = dyn_cast<llvm::Constant>(v)) {
    Word id = lowerConstant(c);
    valueIDs[v] = id;
    return id;
  }

  // Forward reference fallback.
  Word id = allocID();
  valueIDs[v] = id;
  return id;
}

// ─────────────────────────────────────────────────────────────────────────────
// Module setup
// ─────────────────────────────────────────────────────────────────────────────
inline void IRToSPIRV::emitCapabilitiesAndModel() {
  appendOp(capabilities, SpvOpCapability, {SpvCapabilityShader});

  Word extID = allocID();
  glslExtID = extID;
  appendOpStr(extInstImports, SpvOpExtInstImport, {extID}, "GLSL.std.450");

  appendOp(memoryModel, SpvOpMemoryModel,
           {SpvAddressingModelLogical, SpvMemoryModelGLSL450});
}

inline bool IRToSPIRV::isUniformGlobal(llvm::GlobalVariable& G) {
  // Skip RISC-V trampoline/layout constants and SSBO descriptors.
  if (G.isConstant()) return false;
  llvm::StringRef nm = G.getName();
  if (nm.contains("_output_floats")) return false;
  if (nm.contains("_varying_floats")) return false;
  if (nm.contains("_total_floats")) return false;
  if (nm.contains("_layout")) return false;
  // Pointer-typed externals are SSBO descriptors (handled by analyzeSSBOs).
  if (G.getValueType()->isPointerTy()) return false;
  return true;
}

inline bool IRToSPIRV::isSSBOGlobal(llvm::GlobalVariable& G) {
  // Compute-stage storage buffers come through the IR as opaque pointer
  // externals (e.g. `@src = external global ptr`). The element type is
  // recovered from the GEP that consumes `load ptr, ptr @G`.
  if (G.isConstant()) return false;
  if (!G.getValueType()->isPointerTy()) return false;
  llvm::StringRef nm = G.getName();
  if (nm.contains("_output_floats")) return false;
  if (nm.contains("_varying_floats")) return false;
  if (nm.contains("_total_floats")) return false;
  if (nm.contains("_layout")) return false;
  // Sampler2D globals are also opaque pointers — distinguish by checking
  // whether the global is consumed by a __tex2d_sample call.
  if (isSamplerGlobal(G)) return false;
  return true;
}

inline bool IRToSPIRV::isSamplerGlobal(llvm::GlobalVariable& G) {
  // sampler2D uniforms appear as `@tex = global ptr null` in the IR.
  // Walk users: load ptr → call __tex2d_sample(ptr, ...) means it's a sampler.
  if (G.isConstant()) return false;
  if (!G.getValueType()->isPointerTy()) return false;
  for (auto* U : G.users()) {
    auto* LI = llvm::dyn_cast<llvm::LoadInst>(U);
    if (!LI) continue;
    for (auto* U2 : LI->users()) {
      auto* CI = llvm::dyn_cast<llvm::CallInst>(U2);
      if (!CI || !CI->getCalledFunction()) continue;
      llvm::StringRef cn = CI->getCalledFunction()->getName();
      if (cn == "__tex2d_sample" || cn == "__tex2d_sample_lod" ||
          cn == "__texcube_sample")
        return true;
    }
  }
  return false;
}

inline void IRToSPIRV::analyzeUniforms() {
  std::vector<llvm::GlobalVariable*> uniforms;
  for (auto& G : M.globals())
    if (isUniformGlobal(G)) uniforms.push_back(&G);

  if (uniforms.empty()) return;

  // Build a struct with one member per uniform.
  std::vector<Word> memberTypeIDs;
  for (auto* G : uniforms) memberTypeIDs.push_back(typeFor(G->getValueType()));

  pushConstStructTypeID = structTypeFor(memberTypeIDs);
  pushConstStructPtrID = allocID();
  appendOp(typesGlobals, SpvOpTypePointer,
           {pushConstStructPtrID, SpvStorageClassPushConstant,
            pushConstStructTypeID});

  pushConstVarID = allocID();
  appendOp(typesGlobals, SpvOpVariable,
           {pushConstStructPtrID, pushConstVarID, SpvStorageClassPushConstant});

  appendOp(annotations, SpvOpDecorate,
           {pushConstStructTypeID, SpvDecorationBlock});
  // SPIR-V 1.0: push-constants do NOT go in OpEntryPoint interface list
  // (only Input/Output do). Newer versions widen this; we target 1.0.

  // std140/std430-ish offsets, using LLVM type sizes (good enough for our flat
  // float/vec uniforms — all our shaders use only floats/vec2/vec3/vec4 here).
  auto& DL = M.getDataLayout();
  uint32_t offset = 0;
  for (uint32_t i = 0; i < uniforms.size(); ++i) {
    llvm::Type* ty = uniforms[i]->getValueType();
    uint32_t sz = (uint32_t)DL.getTypeAllocSize(ty);
    // Align to type size for std430 (vec3 aligned to 16, vec2 to 8, float to
    // 4).
    uint32_t align = sz;
    if (auto* vt = dyn_cast<llvm::FixedVectorType>(ty)) {
      unsigned n = vt->getNumElements();
      unsigned elemSz = (unsigned)DL.getTypeAllocSize(vt->getElementType());
      align = (n == 3 ? 4 : n) * elemSz;
    }
    offset = (offset + align - 1) & ~(align - 1);
    appendOp(annotations, SpvOpMemberDecorate,
             {pushConstStructTypeID, i, SpvDecorationOffset, offset});
    uniformMemberIdx[uniforms[i]] = i;
    offset += sz;
  }
}

inline void IRToSPIRV::analyzeSSBOs() {
  // SPIR-V 1.0 has no StorageBuffer storage class — SSBOs use the Uniform
  // storage class with the BufferBlock decoration on the wrapping struct.
  using namespace llvm;

  // Recover the runtime-array element type by walking users:
  //   %ptr = load ptr, ptr @G
  //   %elem.ptr = getelementptr inbounds <ELEM_TY>, ptr %ptr, ...
  // The GEP source-element-type is what ends up in the runtime array.
  auto inferElemType = [&](GlobalVariable& G) -> Type* {
    for (User* U : G.users()) {
      auto* LI = dyn_cast<LoadInst>(U);
      if (!LI) continue;
      for (User* U2 : LI->users()) {
        if (auto* GEP = dyn_cast<GetElementPtrInst>(U2))
          return GEP->getSourceElementType();
      }
    }
    return Type::getInt32Ty(M.getContext());  // fallback
  };

  uint32_t binding = 0;
  for (auto& G : M.globals()) {
    if (!isSSBOGlobal(G)) continue;

    Type* elemTy = inferElemType(G);
    Word elemTypeID = typeFor(elemTy);

    // OpTypeRuntimeArray <elem>
    Word rtArrID = allocID();
    appendOp(typesGlobals, SpvOpTypeRuntimeArray, {rtArrID, elemTypeID});
    uint32_t stride = (uint32_t)M.getDataLayout().getTypeAllocSize(elemTy);
    appendOp(annotations, SpvOpDecorate,
             {rtArrID, SpvDecorationArrayStride, stride});

    // OpTypeStruct { runtimeArray }, decorated BufferBlock with Offset 0
    Word structID = structTypeFor({rtArrID});
    appendOp(annotations, SpvOpDecorate, {structID, SpvDecorationBufferBlock});
    appendOp(annotations, SpvOpMemberDecorate,
             {structID, 0u, SpvDecorationOffset, 0u});

    // OpTypePointer Uniform <struct>
    Word structPtrID = allocID();
    appendOp(typesGlobals, SpvOpTypePointer,
             {structPtrID, SpvStorageClassUniform, structID});

    // OpVariable <struct ptr> Uniform
    Word varID = allocID();
    appendOp(typesGlobals, SpvOpVariable,
             {structPtrID, varID, SpvStorageClassUniform});

    // Descriptor decorations: set 0, binding N (host expects set 0).
    appendOp(annotations, SpvOpDecorate,
             {varID, SpvDecorationDescriptorSet, 0u});
    appendOp(annotations, SpvOpDecorate,
             {varID, SpvDecorationBinding, binding});

    // OpTypePointer Uniform <elem> — used as the result type of OpAccessChain.
    Word elemPtrTypeID = ptrTypeFor(elemTy, SpvStorageClassUniform);

    SSBOInfo info;
    info.varID = varID;
    info.elemPtrTypeID = elemPtrTypeID;
    info.binding = binding;
    ssboInfo[&G] = info;
    binding++;
  }
}

inline void IRToSPIRV::analyzeSamplers() {
  using namespace llvm;

  uint32_t binding = 0;
  for (auto& G : M.globals()) {
    if (!isSamplerGlobal(G)) continue;

    Word floatTID = typeFor(Type::getFloatTy(M.getContext()));

    // OpTypeImage %float 2D depth=0 arrayed=0 ms=0 sampled=1 format=Unknown
    Word imageTID = allocID();
    appendOp(typesGlobals, SpvOpTypeImage,
             {imageTID, floatTID, SpvDim2D, 0u, 0u, 0u, 1u,
              SpvImageFormatUnknown});

    // OpTypeSampledImage %image
    Word sampledImageTID = allocID();
    appendOp(typesGlobals, SpvOpTypeSampledImage, {sampledImageTID, imageTID});

    // OpTypePointer UniformConstant %sampledImage
    Word ptrTID = allocID();
    appendOp(typesGlobals, SpvOpTypePointer,
             {ptrTID, SpvStorageClassUniformConstant, sampledImageTID});

    // OpVariable %ptr UniformConstant
    Word varID = allocID();
    appendOp(typesGlobals, SpvOpVariable,
             {ptrTID, varID, SpvStorageClassUniformConstant});

    appendOp(annotations, SpvOpDecorate,
             {varID, SpvDecorationDescriptorSet, 0u});
    appendOp(annotations, SpvOpDecorate,
             {varID, SpvDecorationBinding, binding});

    SamplerInfo info;
    info.varID = varID;
    info.sampledImageTypeID = sampledImageTID;
    info.binding = binding;
    samplerInfo[&G] = info;
    binding++;
  }
}

inline SpvBuiltIn IRToSPIRV::builtinForName(llvm::StringRef name) {
  if (name == "gl_FragCoord") return SpvBuiltInFragCoord;
  if (name == "gl_Position") return SpvBuiltInPosition;
  if (name == "gl_VertexID" || name == "gl_VertexIndex")
    return SpvBuiltInVertexIndex;
  if (name == "gl_InstanceID" || name == "gl_InstanceIndex")
    return SpvBuiltInInstanceIndex;
  if (name == "gl_GlobalInvocationID") return SpvBuiltInGlobalInvocationId;
  if (name == "gl_LocalInvocationID") return SpvBuiltInLocalInvocationId;
  if (name == "gl_WorkGroupID") return SpvBuiltInWorkgroupId;
  return (SpvBuiltIn)-1;
}

inline bool IRToSPIRV::isFragColorAlloca(llvm::AllocaInst* AI) {
  return AI->hasName() && AI->getName() == "FragColor";
}

inline bool IRToSPIRV::isShadowAllocaForArg(llvm::AllocaInst* AI,
                                            llvm::Argument*& outArg,
                                            llvm::Function& F) {
  if (!AI->hasName()) return false;
  std::string n = AI->getName().str();
  // strip trailing digits (LLVM disambiguator)
  std::string base = n;
  while (!base.empty() && std::isdigit((unsigned char)base.back()))
    base.pop_back();
  for (auto& A : F.args()) {
    if (A.getName() == base && AI->getAllocatedType() == A.getType()) {
      outArg = &A;
      return true;
    }
  }
  return false;
}

inline bool IRToSPIRV::isPointerAlloca(llvm::AllocaInst* AI) {
  return AI->getAllocatedType()->isPointerTy();
}

inline void IRToSPIRV::emitStageIO() {
  if (!entryFn) return;

  auto declareInputBuiltin = [&](llvm::Argument& A, SpvBuiltIn bi) -> Word {
    Word ptrID = ptrTypeFor(A.getType(), SpvStorageClassInput);
    Word vid = allocID();
    appendOp(typesGlobals, SpvOpVariable, {ptrID, vid, SpvStorageClassInput});
    appendOp(annotations, SpvOpDecorate, {vid, SpvDecorationBuiltIn, (Word)bi});
    return vid;
  };
  auto declareInputLocation = [&](llvm::Argument& A, uint32_t loc) -> Word {
    Word ptrID = ptrTypeFor(A.getType(), SpvStorageClassInput);
    Word vid = allocID();
    appendOp(typesGlobals, SpvOpVariable, {ptrID, vid, SpvStorageClassInput});
    appendOp(annotations, SpvOpDecorate, {vid, SpvDecorationLocation, loc});
    return vid;
  };

  uint32_t inputLoc = 0;
  for (auto& A : entryFn->args()) {
    if (A.getType()->isPointerTy()) continue;  // skip _out
    SpvBuiltIn bi = builtinForName(A.getName());
    Word vid;
    if (bi != (SpvBuiltIn)-1) {
      vid = declareInputBuiltin(A, bi);
    } else {
      vid = declareInputLocation(A, inputLoc++);
    }
    argInputVarID[&A] = vid;
    entryPointInterface.push_back(vid);
  }

  // Output: assume fragment writes vec4 FragColor (Location 0). For vertex,
  // emit gl_Position (BuiltIn Position) and any user out-vars by alloca name.
  auto& EB = entryFn->getEntryBlock();
  for (auto& I : EB) {
    auto* AI = dyn_cast<llvm::AllocaInst>(&I);
    if (!AI || !AI->hasName()) continue;
    llvm::StringRef n = AI->getName();
    // Skip shadow allocas for params (handled below).
    llvm::Argument* a = nullptr;
    if (isShadowAllocaForArg(AI, a, *entryFn)) {
      // Map the shadow alloca to the corresponding input variable.
      allocaToInputVar[AI] = argInputVarID[a];
      continue;
    }
    // Skip pointer-typed allocas (_out trampoline).
    if (AI->getAllocatedType()->isPointerTy()) {
      deadValues.insert(AI);
      continue;
    }
    if (n == "FragColor") {
      llvm::Type* t = AI->getAllocatedType();
      Word ptrID = ptrTypeFor(t, SpvStorageClassOutput);
      Word vid = allocID();
      appendOp(typesGlobals, SpvOpVariable,
               {ptrID, vid, SpvStorageClassOutput});
      appendOp(annotations, SpvOpDecorate, {vid, SpvDecorationLocation, 0});
      allocaToOutputVar[AI] = vid;
      entryPointInterface.push_back(vid);
    } else if (n == "gl_Position") {
      llvm::Type* t = AI->getAllocatedType();
      Word ptrID = ptrTypeFor(t, SpvStorageClassOutput);
      Word vid = allocID();
      appendOp(typesGlobals, SpvOpVariable,
               {ptrID, vid, SpvStorageClassOutput});
      appendOp(annotations, SpvOpDecorate,
               {vid, SpvDecorationBuiltIn, SpvBuiltInPosition});
      allocaToOutputVar[AI] = vid;
      entryPointInterface.push_back(vid);
    }
    // (Vertex out-locations like vUV are detected per-store below.)
  }

  // Vertex shaders also store to alloca'd output variables (vUV, vColor) before
  // the trampoline copies them out. Detect those by name and create Output
  // vars.
  //
  // ast.cpp inserts each output alloca at the START of the entry block, so the
  // IR contains them in REVERSE declaration order. We iterate in reverse to
  // recover declaration order, which is what the FS expects for Location
  // numbering (FS reads Location 0 = first VS out-var, Location 1 = second).
  if (stage == ShaderStage::Vertex) {
    std::vector<llvm::AllocaInst*> vsOutAllocas;
    for (auto& I : EB) {
      auto* AI = dyn_cast<llvm::AllocaInst>(&I);
      if (!AI || !AI->hasName()) continue;
      if (AI->getAllocatedType()->isPointerTy()) continue;
      llvm::StringRef n = AI->getName();
      if (n == "FragColor" || n == "gl_Position") continue;
      if (!n.starts_with("v")) continue;
      auto* vt = dyn_cast<llvm::FixedVectorType>(AI->getAllocatedType());
      if (!vt || !vt->getElementType()->isFloatTy()) continue;
      vsOutAllocas.push_back(AI);
    }
    uint32_t outLoc = 0;
    for (auto it = vsOutAllocas.rbegin(); it != vsOutAllocas.rend(); ++it) {
      llvm::AllocaInst* AI = *it;
      Word ptrID = ptrTypeFor(AI->getAllocatedType(), SpvStorageClassOutput);
      Word vid = allocID();
      appendOp(typesGlobals, SpvOpVariable,
               {ptrID, vid, SpvStorageClassOutput});
      appendOp(annotations, SpvOpDecorate,
               {vid, SpvDecorationLocation, outLoc++});
      allocaToOutputVar[AI] = vid;
      entryPointInterface.push_back(vid);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// CFG analysis: detect loops and selections by block-name pattern.
// Our codegen always names them: for.cond / for.body / for.inc / for.end,
// then / else / ifend, logical.rhs / logical.merge.
// ─────────────────────────────────────────────────────────────────────────────
inline void IRToSPIRV::analyzeCFG(llvm::Function& F) {
  selectionMerge.clear();
  loopMerge.clear();
  loopContinue.clear();

  auto findByPrefix = [&](llvm::StringRef pfx) -> llvm::BasicBlock* {
    for (auto& BB : F)
      if (BB.getName().starts_with(pfx)) return &BB;
    return nullptr;
  };

  // Walk all basic blocks; classify by name.
  for (auto& BB : F) {
    llvm::StringRef n = BB.getName();

    if (n.starts_with("for.cond")) {
      // Loop header. Conditional branch to for.body / for.end.
      auto* term = BB.getTerminator();
      if (auto* br = dyn_cast<llvm::BranchInst>(term)) {
        if (br->isConditional()) {
          llvm::BasicBlock* mergeBB = nullptr;
          llvm::BasicBlock* contBB = nullptr;
          for (auto* succ : llvm::successors(&BB)) {
            if (succ->getName().starts_with("for.end")) mergeBB = succ;
            if (succ->getName().starts_with("for.body")) { /* not the merge */
            }
          }
          // Continue is the for.inc block; find by prefix matching this loop's
          // suffix. We're permissive: pick any for.inc block in the function
          // whose successor is this for.cond. For nested loops this needs
          // improvement — TODO.
          for (auto& B2 : F) {
            if (!B2.getName().starts_with("for.inc")) continue;
            auto* t2 = dyn_cast<llvm::BranchInst>(B2.getTerminator());
            if (t2 && !t2->isConditional() && t2->getSuccessor(0) == &BB) {
              contBB = &B2;
              break;
            }
          }
          if (mergeBB) loopMerge[&BB] = mergeBB;
          if (contBB) loopContinue[&BB] = contBB;
        }
      }
      continue;
    }

    // Selection header: a conditional branch where the merge point is named
    // ifend/if.end/logical.merge.
    auto* term = BB.getTerminator();
    if (auto* br = dyn_cast<llvm::BranchInst>(term)) {
      if (br->isConditional()) {
        llvm::BasicBlock* t = br->getSuccessor(0);
        llvm::BasicBlock* f = br->getSuccessor(1);
        auto isMerge = [](llvm::BasicBlock* b) {
          auto nm = b->getName();
          return nm.starts_with("ifend") || nm.starts_with("if.end") ||
                 nm.starts_with("logical.merge") ||
                 nm.starts_with("ternary.merge");
        };
        if (isMerge(f)) {
          selectionMerge[&BB] = f;
        } else if (isMerge(t)) {
          selectionMerge[&BB] = t;
        } else {
          // if/else: merge is whichever shared successor is the join.
          // For our codegen, "then" and "else" both branch to "ifend".
          for (auto* st : llvm::successors(t)) {
            if (isMerge(st)) {
              selectionMerge[&BB] = st;
              break;
            }
          }
          if (!selectionMerge.count(&BB)) {
            for (auto* sf : llvm::successors(f)) {
              if (isMerge(sf)) {
                selectionMerge[&BB] = sf;
                break;
              }
            }
          }
          // logical short-circuit: logical.rhs branches to logical.merge
          if (!selectionMerge.count(&BB)) {
            if (t->getName().starts_with("logical.rhs")) {
              selectionMerge[&BB] = f;  // logical.merge
            } else if (f->getName().starts_with("logical.rhs")) {
              selectionMerge[&BB] = t;
            }
          }
        }
      }
    }
  }
  (void)findByPrefix;
}

// ─────────────────────────────────────────────────────────────────────────────
// Function body emission
// ─────────────────────────────────────────────────────────────────────────────
inline bool IRToSPIRV::isExtInstFunction(llvm::Function* F, GLSLstd450& outOp) {
  if (!F) return false;
  if (!F->isIntrinsic()) {
    // Some codegen builtins map to extinsts as user fn names — we don't use
    // those.
    return false;
  }
  switch (F->getIntrinsicID()) {
    case llvm::Intrinsic::sin:
      outOp = GLSLstd450Sin;
      return true;
    case llvm::Intrinsic::cos:
      outOp = GLSLstd450Cos;
      return true;
    case llvm::Intrinsic::sqrt:
      outOp = GLSLstd450Sqrt;
      return true;
    case llvm::Intrinsic::exp:
      outOp = GLSLstd450Exp;
      return true;
    case llvm::Intrinsic::exp2:
      outOp = GLSLstd450Exp2;
      return true;
    case llvm::Intrinsic::log:
      outOp = GLSLstd450Log;
      return true;
    case llvm::Intrinsic::log2:
      outOp = GLSLstd450Log2;
      return true;
    case llvm::Intrinsic::pow:
      outOp = GLSLstd450Pow;
      return true;
    case llvm::Intrinsic::fabs:
      outOp = GLSLstd450FAbs;
      return true;
    case llvm::Intrinsic::floor:
      outOp = GLSLstd450Floor;
      return true;
    case llvm::Intrinsic::ceil:
      outOp = GLSLstd450Ceil;
      return true;
    case llvm::Intrinsic::round:
      outOp = GLSLstd450Round;
      return true;
    case llvm::Intrinsic::trunc:
      outOp = GLSLstd450Trunc;
      return true;
    case llvm::Intrinsic::maxnum:
      outOp = GLSLstd450FMax;
      return true;
    case llvm::Intrinsic::minnum:
      outOp = GLSLstd450FMin;
      return true;
    case llvm::Intrinsic::fma:
      outOp = GLSLstd450Fma;
      return true;
    default:
      return false;
  }
}

inline bool IRToSPIRV::isTrampolineFunction(llvm::Function& F) {
  llvm::StringRef n = F.getName();
  return n == "fs_invoke" || n == "vs_invoke" || n == "cs_invoke" ||
         n == "cs_dispatch_row";
}

inline llvm::Function* IRToSPIRV::findStageEntry() {
  for (auto& F : M) {
    if (F.isDeclaration()) continue;
    if (F.hasMetadata("shader.stage")) return &F;
  }
  return nullptr;
}

inline Word IRToSPIRV::uintTypeFor() {
  if (uint32TypeID) return uint32TypeID;
  uint32TypeID = allocID();
  appendOp(typesGlobals, SpvOpTypeInt, {uint32TypeID, 32, 0});
  return uint32TypeID;
}

inline Word IRToSPIRV::splatToVector(Word scalarID, llvm::Type* dstVecType,
                                     std::vector<Word>& out) {
  auto* vt = dyn_cast<llvm::FixedVectorType>(dstVecType);
  if (!vt) return scalarID;
  Word vTID = typeFor(vt);
  Word id = allocID();
  std::vector<Word> ops;
  ops.push_back(vTID);
  ops.push_back(id);
  for (unsigned i = 0; i < vt->getNumElements(); ++i) ops.push_back(scalarID);
  appendOpV(out, SpvOpCompositeConstruct, ops);
  return id;
}

inline void IRToSPIRV::emitInstruction(llvm::Instruction& I,
                                       std::vector<Word>& out) {
  using namespace llvm;

  // Skip dead (trampoline) values and their users.
  if (deadValues.count(&I)) return;

  // Allocas are emitted first as OpVariable in the entry block by the caller.
  if (isa<AllocaInst>(I)) return;

  if (auto* SI = dyn_cast<StoreInst>(&I)) {
    Value* ptr = SI->getPointerOperand();
    Value* val = SI->getValueOperand();
    if (deadValues.count(ptr)) return;
    // Param-shadow store: store %arg, ptr %argShadow — no-op (the shadow
    // alloca is mapped directly to the input variable).
    if (auto* AI = dyn_cast<AllocaInst>(ptr)) {
      if (allocaToInputVar.count(AI) && isa<Argument>(val)) return;
    }
    Word p = valueOf(ptr);
    Word v = valueOf(val);
    appendOp(out, SpvOpStore, {p, v});
    return;
  }

  if (auto* LI = dyn_cast<LoadInst>(&I)) {
    Value* ptr = LI->getPointerOperand();
    // Loading the SSBO base pointer (`%p = load ptr, ptr @ssbo`) emits no
    // SPIR-V — we just remember that %p stands in for that SSBO so downstream
    // GEPs can lower into OpAccessChain on the real OpVariable.
    if (auto* GV = dyn_cast<GlobalVariable>(ptr)) {
      if (ssboInfo.count(GV)) {
        valueToSSBO[&I] = GV;
        return;
      }
      // Same trick for sampler2D loads: track the sampler so the
      // following __tex2d_sample call can address the real OpVariable.
      if (samplerInfo.count(GV)) {
        valueToSampler[&I] = GV;
        return;
      }
    }
    if (deadValues.count(ptr) || (LI->getType()->isPointerTy())) {
      // Loading from a tombstone (e.g. _out pointer chain) — taint result.
      // Also: any load that produces a pointer is part of the trampoline
      // tail (we don't generate user-pointer values in shaders).
      deadValues.insert(&I);
      valueIDs[&I] = 0;
      if (getenv("IRGEN_SPIRV_DEBUG")) {
        llvm::errs() << "[spv] dead load: ";
        I.print(llvm::errs());
        llvm::errs() << "\n";
      }
      return;
    }
    // Load from a uniform global → access chain into push-const block + load.
    if (auto* GV = dyn_cast<GlobalVariable>(ptr)) {
      auto it = uniformMemberIdx.find(GV);
      if (it != uniformMemberIdx.end()) {
        Word idxConst = constUint(it->second);
        Word ptrTy =
            ptrTypeFor(GV->getValueType(), SpvStorageClassPushConstant);
        Word acID = allocID();
        appendOp(out, SpvOpAccessChain,
                 {ptrTy, acID, pushConstVarID, idxConst});
        Word resTy = typeFor(LI->getType());
        Word resID = allocID();
        valueIDs[&I] = resID;
        appendOp(out, SpvOpLoad, {resTy, resID, acID});
        return;
      }
    }
    Word resTy = typeFor(LI->getType());
    Word resID = allocID();
    valueIDs[&I] = resID;
    Word p = valueOf(ptr);
    appendOp(out, SpvOpLoad, {resTy, resID, p});
    return;
  }

  if (auto* GEP = dyn_cast<GetElementPtrInst>(&I)) {
    Value* basePtr = GEP->getPointerOperand();
    // GEP through an SSBO base — lower into OpAccessChain.
    auto ssboIt = valueToSSBO.find(basePtr);
    if (ssboIt != valueToSSBO.end()) {
      const SSBOInfo& info = ssboInfo[ssboIt->second];
      // With opaque pointers the GEP looks like
      //   getelementptr inbounds <ELEM_TY>, ptr %base, i32 %idx
      // i.e. exactly one index that addresses the runtime array.
      if (GEP->getNumIndices() != 1) {
        llvm::errs() << "[spv] unsupported SSBO GEP with "
                     << GEP->getNumIndices() << " indices: ";
        GEP->print(llvm::errs());
        llvm::errs() << "\n";
        deadValues.insert(&I);
        valueIDs[&I] = 0;
        return;
      }
      Word zeroID = constUint(0);  // member 0 = the runtime array
      Word idxID = valueOf(GEP->getOperand(1));
      Word acID = allocID();
      valueIDs[&I] = acID;
      appendOp(out, SpvOpAccessChain,
               {info.elemPtrTypeID, acID, info.varID, zeroID, idxID});
      return;
    }
    if (deadValues.count(basePtr)) {
      deadValues.insert(&I);
      valueIDs[&I] = 0;
      return;
    }
    // We don't yet need GEPs except for the trampoline (already dead).
    // Generic OpAccessChain: rare case — skip silently for now.
    deadValues.insert(&I);
    return;
  }

  if (auto* BO = dyn_cast<BinaryOperator>(&I)) {
    Word a = valueOf(BO->getOperand(0));
    Word b = valueOf(BO->getOperand(1));
    Word ty = typeFor(BO->getType());
    Word id = allocID();
    valueIDs[&I] = id;

    // OpUMod / OpUDiv require unsigned operand and result types. Bitcast
    // through a parallel unsigned i32 type and bitcast the result back so
    // downstream signedness-agnostic ops keep working unchanged.
    if (BO->getOpcode() == Instruction::URem ||
        BO->getOpcode() == Instruction::UDiv) {
      Word uTy = uintTypeFor();
      Word aU = allocID();
      appendOp(out, SpvOpBitcast, {uTy, aU, a});
      Word bU = allocID();
      appendOp(out, SpvOpBitcast, {uTy, bU, b});
      Word resU = allocID();
      SpvOp uop =
          (BO->getOpcode() == Instruction::URem) ? SpvOpUMod : SpvOpUDiv;
      appendOp(out, uop, {uTy, resU, aU, bU});
      appendOp(out, SpvOpBitcast, {ty, id, resU});
      return;
    }

    SpvOp op;
    switch (BO->getOpcode()) {
      case Instruction::FAdd:
        op = SpvOpFAdd;
        break;
      case Instruction::FSub:
        op = SpvOpFSub;
        break;
      case Instruction::FMul:
        op = SpvOpFMul;
        break;
      case Instruction::FDiv:
        op = SpvOpFDiv;
        break;
      case Instruction::FRem:
        op = SpvOpFRem;
        break;
      case Instruction::Add:
        op = SpvOpIAdd;
        break;
      case Instruction::Sub:
        op = SpvOpISub;
        break;
      case Instruction::Mul:
        op = SpvOpIMul;
        break;
      case Instruction::SDiv:
        op = SpvOpSDiv;
        break;
      case Instruction::UDiv:
        op = SpvOpUDiv;
        break;
      case Instruction::SRem:
        op = SpvOpSRem;
        break;
      case Instruction::URem:
        op = SpvOpUMod;
        break;
      case Instruction::Shl:
        op = SpvOpShiftLeftLogical;
        break;
      case Instruction::LShr:
        op = SpvOpShiftRightLogical;
        break;
      case Instruction::AShr:
        op = SpvOpShiftRightArithmetic;
        break;
      case Instruction::And:
        op = SpvOpBitwiseAnd;
        break;
      case Instruction::Or:
        op = SpvOpBitwiseOr;
        break;
      case Instruction::Xor:
        op = SpvOpBitwiseXor;
        break;
      default:
        op = SpvOpFAdd;
        break;
    }
    appendOp(out, op, {ty, id, a, b});
    return;
  }

  if (auto* UN = dyn_cast<UnaryOperator>(&I)) {
    if (UN->getOpcode() == Instruction::FNeg) {
      Word a = valueOf(UN->getOperand(0));
      Word ty = typeFor(UN->getType());
      Word id = allocID();
      valueIDs[&I] = id;
      appendOp(out, SpvOpFNegate, {ty, id, a});
      return;
    }
  }

  if (auto* FC = dyn_cast<FCmpInst>(&I)) {
    Word a = valueOf(FC->getOperand(0));
    Word b = valueOf(FC->getOperand(1));
    Word ty = typeFor(FC->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    SpvOp op;
    switch (FC->getPredicate()) {
      case CmpInst::FCMP_OLT:
        op = SpvOpFOrdLessThan;
        break;
      case CmpInst::FCMP_OLE:
        op = SpvOpFOrdLessThanEqual;
        break;
      case CmpInst::FCMP_OGT:
        op = SpvOpFOrdGreaterThan;
        break;
      case CmpInst::FCMP_OGE:
        op = SpvOpFOrdGreaterThanEqual;
        break;
      case CmpInst::FCMP_OEQ:
        op = SpvOpFOrdEqual;
        break;
      case CmpInst::FCMP_ONE:
        op = SpvOpFOrdNotEqual;
        break;
      case CmpInst::FCMP_ULT:
        op = SpvOpFUnordLessThan;
        break;
      case CmpInst::FCMP_ULE:
        op = SpvOpFUnordLessThanEqual;
        break;
      case CmpInst::FCMP_UGT:
        op = SpvOpFUnordGreaterThan;
        break;
      case CmpInst::FCMP_UGE:
        op = SpvOpFUnordGreaterThanEqual;
        break;
      case CmpInst::FCMP_UEQ:
        op = SpvOpFUnordEqual;
        break;
      case CmpInst::FCMP_UNE:
        op = SpvOpFUnordNotEqual;
        break;
      default:
        op = SpvOpFOrdEqual;
        break;
    }
    appendOp(out, op, {ty, id, a, b});
    return;
  }

  if (auto* IC = dyn_cast<ICmpInst>(&I)) {
    Word a = valueOf(IC->getOperand(0));
    Word b = valueOf(IC->getOperand(1));
    Word ty = typeFor(IC->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    SpvOp op;
    switch (IC->getPredicate()) {
      case CmpInst::ICMP_EQ:
        op = SpvOpIEqual;
        break;
      case CmpInst::ICMP_NE:
        op = SpvOpINotEqual;
        break;
      case CmpInst::ICMP_SLT:
        op = SpvOpSLessThan;
        break;
      case CmpInst::ICMP_SLE:
        op = SpvOpSLessThanEqual;
        break;
      case CmpInst::ICMP_SGT:
        op = SpvOpSGreaterThan;
        break;
      case CmpInst::ICMP_SGE:
        op = SpvOpSGreaterThanEqual;
        break;
      case CmpInst::ICMP_ULT:
        op = SpvOpULessThan;
        break;
      case CmpInst::ICMP_ULE:
        op = SpvOpULessThanEqual;
        break;
      case CmpInst::ICMP_UGT:
        op = SpvOpUGreaterThan;
        break;
      case CmpInst::ICMP_UGE:
        op = SpvOpUGreaterThanEqual;
        break;
      default:
        op = SpvOpIEqual;
        break;
    }
    appendOp(out, op, {ty, id, a, b});
    return;
  }

  if (auto* SEL = dyn_cast<SelectInst>(&I)) {
    Word c = valueOf(SEL->getCondition());
    Word t = valueOf(SEL->getTrueValue());
    Word f = valueOf(SEL->getFalseValue());
    Word ty = typeFor(SEL->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    appendOp(out, SpvOpSelect, {ty, id, c, t, f});
    return;
  }

  if (auto* EE = dyn_cast<ExtractElementInst>(&I)) {
    Word vec = valueOf(EE->getVectorOperand());
    Word ty = typeFor(EE->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    if (auto* ci = dyn_cast<ConstantInt>(EE->getIndexOperand())) {
      appendOp(out, SpvOpCompositeExtract,
               {ty, id, vec, (Word)ci->getZExtValue()});
    } else {
      Word idx = valueOf(EE->getIndexOperand());
      appendOp(out, SpvOpVectorExtractDynamic, {ty, id, vec, idx});
    }
    return;
  }

  if (auto* IE = dyn_cast<InsertElementInst>(&I)) {
    Word vec = valueOf(IE->getOperand(0));
    Word el = valueOf(IE->getOperand(1));
    Word ty = typeFor(IE->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    if (auto* ci = dyn_cast<ConstantInt>(IE->getOperand(2))) {
      appendOp(out, SpvOpCompositeInsert,
               {ty, id, el, vec, (Word)ci->getZExtValue()});
    } else {
      Word idx = valueOf(IE->getOperand(2));
      appendOp(out, SpvOpVectorInsertDynamic, {ty, id, vec, el, idx});
    }
    return;
  }

  // Casts ── all just become the appropriate Op*Convert*
  if (auto* CI = dyn_cast<CastInst>(&I)) {
    Word src = valueOf(CI->getOperand(0));
    Word ty = typeFor(CI->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    SpvOp op;
    switch (CI->getOpcode()) {
      case Instruction::SIToFP:
        op = SpvOpConvertSToF;
        break;
      case Instruction::UIToFP:
        op = SpvOpConvertUToF;
        break;
      case Instruction::FPToSI:
        op = SpvOpConvertFToS;
        break;
      case Instruction::FPToUI:
        op = SpvOpConvertFToU;
        break;
      case Instruction::FPTrunc:
        op = SpvOpFConvert;
        break;
      case Instruction::FPExt:
        op = SpvOpFConvert;
        break;
      case Instruction::SExt:
        op = SpvOpSConvert;
        break;
      case Instruction::ZExt:
        op = SpvOpUConvert;
        break;
      case Instruction::Trunc:
        op = SpvOpUConvert;
        break;
      case Instruction::BitCast:
        op = SpvOpBitcast;
        break;
      default:
        op = SpvOpBitcast;
        break;
    }
    appendOp(out, op, {ty, id, src});
    return;
  }

  if (auto* CALL = dyn_cast<CallInst>(&I)) {
    Function* callee = CALL->getCalledFunction();
    if (!callee) return;

    GLSLstd450 ext;
    if (isExtInstFunction(callee, ext)) {
      Word ty = typeFor(CALL->getType());
      Word id = allocID();
      valueIDs[&I] = id;
      std::vector<Word> ops;
      ops.push_back(ty);
      ops.push_back(id);
      ops.push_back(glslExtID);
      ops.push_back((Word)ext);
      for (unsigned i = 0; i < CALL->arg_size(); ++i)
        ops.push_back(valueOf(CALL->getArgOperand(i)));
      appendOpV(out, SpvOpExtInst, ops);
      return;
    }

    // Discard / barrier / texture sampling — skipped or emitted as TODOs.
    llvm::StringRef nm = callee->getName();
    if (nm == "__frag_discard") {
      appendOp(out, SpvOpKill, {});
      return;
    }

    // texture(sampler2D, vec2) was lowered to:
    //   call void @__tex2d_sample(ptr %sampler, float u, float v, ptr %out)
    // where %out is an alloca <4 x float>. Lower to OpImageSampleImplicitLod
    // and store the result into the alloca so the subsequent OpLoad reads it.
    if (nm == "__tex2d_sample" && CALL->arg_size() == 4) {
      Value* samplerArg = CALL->getArgOperand(0);
      auto it = valueToSampler.find(samplerArg);
      if (it == valueToSampler.end()) {
        llvm::errs() << "[spv] __tex2d_sample: sampler arg not tracked\n";
        return;
      }
      const SamplerInfo& info = samplerInfo[it->second];
      // Load the combined sampled image once per call.
      Word loadedID = allocID();
      appendOp(out, SpvOpLoad,
               {info.sampledImageTypeID, loadedID, info.varID});
      // Build a vec2 UV from the two scalar args.
      auto* vec2Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 2);
      Word vec2TID = typeFor(vec2Ty);
      Word uvID = allocID();
      appendOp(out, SpvOpCompositeConstruct,
               {vec2TID, uvID,
                valueOf(CALL->getArgOperand(1)),
                valueOf(CALL->getArgOperand(2))});
      // Sample.
      auto* vec4Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 4);
      Word vec4TID = typeFor(vec4Ty);
      Word sampleID = allocID();
      appendOp(out, SpvOpImageSampleImplicitLod,
               {vec4TID, sampleID, loadedID, uvID});
      // OpStore the result into the caller's out alloca.
      Word outPtrID = valueOf(CALL->getArgOperand(3));
      appendOp(out, SpvOpStore, {outPtrID, sampleID});
      return;
    }

    // User-defined function calls — not in our shaders today, but support if
    // needed. (helpers from codegen_state are inlined as intrinsics, so this
    // rarely fires)
    Word fid = valueOf(callee);
    Word ty = typeFor(CALL->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    std::vector<Word> ops;
    ops.push_back(ty);
    ops.push_back(id);
    ops.push_back(fid);
    for (unsigned i = 0; i < CALL->arg_size(); ++i)
      ops.push_back(valueOf(CALL->getArgOperand(i)));
    appendOpV(out, SpvOpFunctionCall, ops);
    return;
  }

  if (auto* PHI = dyn_cast<PHINode>(&I)) {
    Word ty = typeFor(PHI->getType());
    Word id = valueOf(&I);  // phi may be forward-referenced; reserve id
    std::vector<Word> ops;
    ops.push_back(ty);
    ops.push_back(id);
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
      ops.push_back(valueOf(PHI->getIncomingValue(i)));
      ops.push_back(valueOf(PHI->getIncomingBlock(i)));
    }
    appendOpV(out, SpvOpPhi, ops);
    return;
  }

  // Branch / Return are handled in emitTerminator.
  if (isa<BranchInst>(I) || isa<ReturnInst>(I) || isa<UnreachableInst>(I))
    return;
}

inline void IRToSPIRV::emitTerminator(llvm::BasicBlock& BB,
                                      std::vector<Word>& out) {
  auto* term = BB.getTerminator();
  if (auto* RI = dyn_cast<llvm::ReturnInst>(term)) {
    if (RI->getReturnValue()) {
      Word v = valueOf(RI->getReturnValue());
      appendOp(out, SpvOpReturnValue, {v});
    } else {
      appendOp(out, SpvOpReturn, {});
    }
    return;
  }
  if (auto* br = dyn_cast<llvm::BranchInst>(term)) {
    if (!br->isConditional()) {
      Word tgt = valueOf(br->getSuccessor(0));
      appendOp(out, SpvOpBranch, {tgt});
      return;
    }
    // Conditional. Insert a SelectionMerge or LoopMerge if this BB is a header.
    Word cond = valueOf(br->getCondition());
    Word t = valueOf(br->getSuccessor(0));
    Word f = valueOf(br->getSuccessor(1));

    if (auto it = loopMerge.find(&BB); it != loopMerge.end()) {
      Word mergeID = valueOf(it->second);
      Word contID =
          loopContinue.count(&BB) ? valueOf(loopContinue[&BB]) : mergeID;
      appendOp(out, SpvOpLoopMerge, {mergeID, contID, SpvLoopControlMaskNone});
    } else if (auto it2 = selectionMerge.find(&BB);
               it2 != selectionMerge.end()) {
      Word mergeID = valueOf(it2->second);
      appendOp(out, SpvOpSelectionMerge,
               {mergeID, SpvSelectionControlMaskNone});
    }
    appendOp(out, SpvOpBranchConditional, {cond, t, f});
    return;
  }
  if (isa<llvm::UnreachableInst>(term)) {
    appendOp(out, SpvOpUnreachable, {});
    return;
  }
}

inline void IRToSPIRV::emitFunction(llvm::Function& F) {
  using namespace llvm;
  if (F.isDeclaration()) return;
  if (isTrampolineFunction(F)) return;

  // The entry function gets a particular id; helpers get fresh ids.
  Word fnID = valueOf(&F);

  // Build function type.
  std::vector<llvm::Type*> paramTys;
  // For the entry function we pretend the function signature is `void()` — all
  // inputs/outputs are routed through globals.
  llvm::Type* retTy = nullptr;
  if (&F == entryFn) {
    retTy = llvm::Type::getVoidTy(M.getContext());
  } else {
    retTy = F.getReturnType();
    for (auto& A : F.args()) paramTys.push_back(A.getType());
  }
  Word fnTypeID = funcTypeFor(retTy, paramTys);

  appendOp(functions, SpvOpFunction,
           {typeFor(retTy), fnID, SpvFunctionControlMaskNone, fnTypeID});

  // Function parameters (skipped for entry function).
  if (&F != entryFn) {
    for (auto& A : F.args()) {
      Word id = allocID();
      valueIDs[&A] = id;
      appendOp(functions, SpvOpFunctionParameter, {typeFor(A.getType()), id});
    }
  }

  analyzeCFG(F);

  // Pre-allocate ids for all basic blocks (for forward references in
  // branches/phis).
  for (auto& BB : F) (void)valueOf(&BB);

  // Emit the entry block first.
  auto& EB = F.getEntryBlock();
  appendOp(functions, SpvOpLabel, {valueOf(&EB)});

  // ── Emit OpVariable for every alloca up-front (must be in entry block). ──
  for (auto& I : EB) {
    auto* AI = dyn_cast<AllocaInst>(&I);
    if (!AI) continue;

    // Outputs (FragColor, gl_Position, vUV) are external Output globals.
    if (allocaToOutputVar.count(AI)) {
      valueIDs[AI] = allocaToOutputVar[AI];
      continue;
    }
    // Param shadows are external Input globals.
    if (allocaToInputVar.count(AI)) {
      valueIDs[AI] = allocaToInputVar[AI];
      continue;
    }
    // Pointer alloca (`_out`) — dead.
    if (isPointerAlloca(AI)) {
      deadValues.insert(AI);
      valueIDs[AI] = 0;
      continue;
    }
    Word ptrID = ptrTypeFor(AI->getAllocatedType(), SpvStorageClassFunction);
    Word id = allocID();
    valueIDs[AI] = id;
    appendOp(functions, SpvOpVariable, {ptrID, id, SpvStorageClassFunction});
  }

  // Compute reverse postorder so every block appears after its dominators.
  // For our structured CFG this also keeps loop headers before their bodies
  // and selection headers before then/else, which is required by SPIR-V.
  std::vector<BasicBlock*> rpo;
  {
    std::set<BasicBlock*> seen;
    std::vector<BasicBlock*> postorder;
    std::function<void(BasicBlock*)> dfs = [&](BasicBlock* BB) {
      if (!seen.insert(BB).second) return;
      for (auto* s : llvm::successors(BB)) dfs(s);
      postorder.push_back(BB);
    };
    dfs(&EB);
    rpo.assign(postorder.rbegin(), postorder.rend());
  }

  // Emit entry block contents (label already emitted).
  for (auto& I : EB) emitInstruction(I, functions);
  emitTerminator(EB, functions);

  for (auto* BB : rpo) {
    if (BB == &EB) continue;
    appendOp(functions, SpvOpLabel, {valueOf(BB)});
    for (auto& I : *BB) emitInstruction(I, functions);
    emitTerminator(*BB, functions);
  }

  appendOp(functions, SpvOpFunctionEnd, {});
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-level emit()
// ─────────────────────────────────────────────────────────────────────────────
inline std::vector<uint8_t> IRToSPIRV::emit() {
  using namespace llvm;

  entryFn = findStageEntry();
  if (!entryFn) {
    llvm::errs() << "[spv] no shader entry function found\n";
    return {};
  }
  entryFnID = valueOf(entryFn);

  emitCapabilitiesAndModel();

  // Pre-build standard small types (improves output readability and assigns
  // cached ids).
  typeFor(Type::getVoidTy(M.getContext()));
  typeFor(Type::getFloatTy(M.getContext()));
  typeFor(Type::getInt32Ty(M.getContext()));
  typeFor(Type::getInt1Ty(M.getContext()));

  analyzeUniforms();
  analyzeSSBOs();
  analyzeSamplers();
  emitStageIO();

  // Emit ExecutionMode entry now that the entry id is stable.
  if (stage == ShaderStage::Fragment) {
    appendOp(executionMode, SpvOpExecutionMode,
             {entryFnID, SpvExecutionModeOriginUpperLeft});
  } else if (stage == ShaderStage::Compute) {
    // Read the workgroup size from the entry function's metadata, if present.
    Word x = 1, y = 1, z = 1;
    if (auto* md = entryFn->getMetadata("shader.workgroup_size")) {
      if (md->getNumOperands() >= 3) {
        auto getDim = [&](unsigned i) {
          if (auto* mdv = dyn_cast<ConstantAsMetadata>(md->getOperand(i)))
            if (auto* ci = dyn_cast<ConstantInt>(mdv->getValue()))
              return (Word)ci->getZExtValue();
          return (Word)1;
        };
        x = getDim(0);
        y = getDim(1);
        z = getDim(2);
      }
    }
    appendOp(executionMode, SpvOpExecutionMode,
             {entryFnID, SpvExecutionModeLocalSize, x, y, z});
  }

  // OpEntryPoint (with interface): execution model, fn id, name, [interface
  // ids]
  {
    std::vector<Word> prefix;
    SpvExecutionModel em = SpvExecutionModelFragment;
    if (stage == ShaderStage::Vertex) em = SpvExecutionModelVertex;
    if (stage == ShaderStage::Compute) em = SpvExecutionModelGLCompute;
    prefix.push_back((Word)em);
    prefix.push_back(entryFnID);
    // OpEntryPoint name is encoded BEFORE interface ids; we use appendOpStr's
    // prefix/suffix split. The interface ids are *after* the name string,
    // so we'll hand-roll the encoding here.
    std::vector<Word> nameWords = packString("main");
    std::vector<Word> all = prefix;
    for (Word w : nameWords) all.push_back(w);
    for (Word v : entryPointInterface) all.push_back(v);
    Word wc = 1u + (Word)all.size();
    entryPoint.push_back((wc << 16) | (Word)SpvOpEntryPoint);
    for (Word w : all) entryPoint.push_back(w);
  }

  // Source / debug info (optional but helpful in spirv-dis output).
  appendOp(debugSource, SpvOpSource, {SpvSourceLanguageGLSL, 450});

  // Emit the shader entry function.
  emitFunction(*entryFn);

  // Helper functions (rarely; but support if any non-trampoline non-decl
  // exists).
  for (auto& F : M) {
    if (F.isDeclaration()) continue;
    if (&F == entryFn) continue;
    if (isTrampolineFunction(F)) continue;
    emitFunction(F);
  }

  // ── Stitch sections in the order required by the SPIR-V spec. ────────────
  std::vector<Word> stream;
  // Header: magic, version (1.0), generator, id-bound, schema(0)
  stream.push_back(SpvMagicNumber);
  stream.push_back(0x00010000);  // pin to SPIR-V 1.0 for Vulkan 1.0 compat
  stream.push_back(0x474C534C);  // generator magic ("GLSL"-ish), our marker
  stream.push_back(nextID);      // id bound (exclusive upper)
  stream.push_back(0);           // schema

  auto extend = [&](const std::vector<Word>& s) {
    stream.insert(stream.end(), s.begin(), s.end());
  };
  extend(capabilities);
  extend(extInstImports);
  extend(memoryModel);
  extend(entryPoint);
  extend(executionMode);
  extend(debugSource);
  extend(debugNames);
  extend(annotations);
  extend(typesGlobals);
  extend(functions);

  // Repack into bytes (SPIR-V binary is just a stream of LE 32-bit words).
  std::vector<uint8_t> bytes;
  bytes.resize(stream.size() * 4);
  for (size_t i = 0; i < stream.size(); ++i) {
    Word w = stream[i];
    bytes[i * 4 + 0] = (uint8_t)(w & 0xff);
    bytes[i * 4 + 1] = (uint8_t)((w >> 8) & 0xff);
    bytes[i * 4 + 2] = (uint8_t)((w >> 16) & 0xff);
    bytes[i * 4 + 3] = (uint8_t)((w >> 24) & 0xff);
  }
  return bytes;
}

}  // namespace spv_emit

inline std::vector<uint8_t> emitSPIRVFromIR(llvm::Module& M, ShaderStage stage) {
  spv_emit::IRToSPIRV emitter(M, stage);
  return emitter.emit();
}
