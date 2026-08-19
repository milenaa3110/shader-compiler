// emit_spirv_from_ir.h — Walk LLVM IR and emit a SPIR-V binary directly.

#pragma once

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../../frontend/parser/parser.h"  // for ShaderStage

namespace spv_emit {

using Word = uint32_t;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

// Word-stream helpers
inline void appendOp(std::vector<Word>& dst, SpvOp op, std::initializer_list<Word> ops) {
    Word wordCount = 1u + (Word)ops.size();
    dst.push_back((wordCount << 16) | (Word)op);
    for (Word w : ops)
        dst.push_back(w);
}

inline void appendOpV(std::vector<Word>& dst, SpvOp op, const std::vector<Word>& ops) {
    Word wordCount = 1u + (Word)ops.size();
    dst.push_back((wordCount << 16) | (Word)op);
    for (Word w : ops)
        dst.push_back(w);
}

// A matrix is the named wrapper struct %glsl.matCxR = { [C x <R x float>] }.
// This is a local copy of utils.h's matrixShapeOf; the two predicates must stay
// in sync.
struct SpvMatShape {
    unsigned cols, rows;
};

inline std::optional<SpvMatShape> spvMatrixShape(llvm::Type* t) {
    auto* st = llvm::dyn_cast_or_null<llvm::StructType>(t);
    if (!st || !st->hasName() || !st->getName().starts_with("glsl.mat")) return std::nullopt;
    if (st->getNumElements() != 1) return std::nullopt;
    auto* at = llvm::dyn_cast<llvm::ArrayType>(st->getElementType(0));
    if (!at) return std::nullopt;
    auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(at->getElementType());
    if (!vt) return std::nullopt;
    return SpvMatShape{ static_cast<unsigned>(at->getNumElements()), vt->getNumElements() };
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
inline void appendOpStr(std::vector<Word>& dst, SpvOp op, const std::vector<Word>& prefix, llvm::StringRef str) {
    std::vector<Word> packed = packString(str);
    Word wordCount = 1u + (Word)prefix.size() + (Word)packed.size();
    dst.push_back((wordCount << 16) | (Word)op);
    for (Word w : prefix)
        dst.push_back(w);
    for (Word w : packed)
        dst.push_back(w);
}

// Translator

class IRToSPIRV {
 public:
  IRToSPIRV(llvm::Module& mod, ShaderStage st) : M(mod), stage(st) {}

  // Emit and return the binary as a byte vector.
  std::vector<uint8_t> emit();

 private:
  llvm::Module& M;
  ShaderStage stage;

  // ID management
  Word nextID = 1;
  Word allocID() { return nextID++; }

  llvm::DenseMap<llvm::Value*, Word> valueIDs;  // LLVM value -> SPIR-V id
  llvm::DenseMap<llvm::Type*, Word> typeIDs;    // base types
  llvm::DenseMap<uint64_t, Word> ptrTypeCache;  // (sc << 32) | typeID -> ptr type id
  llvm::DenseMap<uint64_t, Word> intConstCache;         // (typeID << 32) | bits
  llvm::DenseMap<uint64_t, Word> fpConstCache;          // (typeID << 32) | bits
  llvm::DenseMap<llvm::Constant*, Word> aggConstCache;  // composite constants

  // Sections (output ordering matters in SPIR-V)
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

  // Cached well-known IDs 
  Word glslExtID = 0;
  Word voidTypeID = 0;
  Word boolTypeID = 0;
  Word floatTypeID = 0;
  Word int32TypeID = 0;
  Word uint32TypeID = 0;

  // Push-constant block
  Word pushConstStructTypeID = 0;
  Word pushConstVarID = 0;
  Word pushConstStructPtrID = 0;  // ptr to struct (PushConstant)
  llvm::DenseMap<llvm::GlobalVariable*, uint32_t> uniformMemberIdx;  // global -> member index

  // SSBO descriptors (compute storage buffers)
  struct SSBOInfo {
    Word varID = 0;          // OpVariable id (Uniform storage class)
    Word elemPtrTypeID = 0;  // OpTypePointer Uniform <elem> (for OpAccessChain)
    uint32_t binding = 0;
  };
  llvm::DenseMap<llvm::GlobalVariable*, SSBOInfo> ssboInfo;
  // Tracks `%p = load ptr, ptr @ssbo_global` - used for GEP
  llvm::DenseMap<llvm::Value*, llvm::GlobalVariable*> valueToSSBO;

  // Combined image-sampler descriptors (sampler2D uniforms)
  struct SamplerInfo {
    Word varID = 0;             // OpVariable id (UniformConstant storage class)
    Word sampledImageTypeID = 0;  // 0 for a storage image (no sampled-image type)
    Word imageTypeID = 0;         // the OpTypeImage id (for storage image load/store)
    uint32_t binding = 0;
    SpvDim dim = SpvDim2D;
    bool arrayed = false;
    bool isImage = false;         // storage image (Sampled=2) vs sampled texture
  };
  llvm::DenseMap<llvm::GlobalVariable*, SamplerInfo> samplerInfo;
  // Tracks `%p = load ptr, ptr @sampler_global` so that subsequent
  // `__tex2d_sample(%p, …)` calls know which OpVariable to use.
  llvm::DenseMap<llvm::Value*, llvm::GlobalVariable*> valueToSampler;

  // Stage IO
  // Map function-arg/alloca -> input/output OpVariable id.
  llvm::DenseMap<llvm::Argument*, Word> argInputVarID;
  llvm::DenseMap<llvm::AllocaInst*, Word> allocaToOutputVar;  // FragColor/gl_Position
  llvm::DenseMap<llvm::AllocaInst*, Word> allocaToInputVar;   // allocas for inputs
  std::vector<Word> entryPointInterface;  // Input/Output var ids

  // Trampoline / dead value tracking 
  std::set<llvm::Value*> deadValues;  // values from `_out` chain — skip stores

  // The entry function and its info
  llvm::Function* entryFn = nullptr;
  Word entryFnID = 0;

  // CFG / structurization
  // header BB -> merge BB id (for OpSelectionMerge / OpLoopMerge)
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> selectionMerge;
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> loopMerge;
  llvm::DenseMap<llvm::BasicBlock*, llvm::BasicBlock*> loopContinue;

  // Maps matrix inner-array SSA values to their shape for SPIR-V OpTypeMatrix lowerings.
  llvm::DenseMap<const llvm::Value*, SpvMatShape> matColVals;
  void buildMatColVals(llvm::Function& F);

  // Set when an unsupported construct (an unlowered `__tex*`/`__image*` sampler
  // intrinsic) is hit. emit() then returns an empty module so the driver reports
  // failure instead of silently writing an invalid `.spv` and exiting 0.
  bool emitFailed_ = false;

  // Access-chain into the push-constant block for a matrix GEP whose base is a
  // uniform global (`M[i]`, `M[i][j]`). Returns the chain id, or 0 if the base
  // is not a uniform. Shared by the constant-GEP load path and the dynamic GEP.
  Word uniformMatrixAccessChain(llvm::GEPOperator* gep, std::vector<Word>& out);

  // Type / constant builders
  Word typeFor(llvm::Type* t);
  Word ptrTypeFor(llvm::Type* pointee, SpvStorageClass sc);
  Word funcTypeFor(llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params);
  Word funcTypeForIDs(Word retID, llvm::ArrayRef<Word> paramIDs);
  Word paramTypeID(llvm::Argument& A);
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

  // Module setup
  void emitCapabilitiesAndModel();
  void analyzeUniforms();  // build push-constant block
  void analyzeSSBOs();     // emit StorageBuffer descriptors
  void analyzeSamplers();  // emit combined image-sampler descriptors
  void emitStageIO();      // gl_FragCoord, vUV, FragColor, ...
  void emitFunction(llvm::Function& F);

  // Function emission
  void analyzeCFG(llvm::Function& F);
  void emitBlockBody(llvm::BasicBlock& BB, std::vector<Word>& out);
  void emitInstruction(llvm::Instruction& I, std::vector<Word>& out);
  void emitTerminator(llvm::BasicBlock& BB, std::vector<Word>& out);

  // Helpers
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
  Word splatToVector(Word scalarID, llvm::Type* dstVecType, std::vector<Word>& out);
  Word uintTypeFor();
};

// Interface slots a stage variable of this type occupies
inline uint32_t spvLocationSlots(llvm::Type* t) {
    if (auto shape = spvMatrixShape(t)) return shape->cols;
    auto* at = dyn_cast<llvm::ArrayType>(t);
    if (at && at->getElementType()->isVectorTy()) return (uint32_t)at->getNumElements();
    return 1;
}

// Implementation
inline Word IRToSPIRV::typeFor(llvm::Type* t) {
    auto it = typeIDs.find(t);
    if (it != typeIDs.end()) return it->second;

    Word id = 0;
    if (t->isVoidTy()) {
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeVoid, { id });
        voidTypeID = id;
    } else if (t->isFloatTy()) {
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeFloat, { id, 32 });
        floatTypeID = id;
    } else if (t->isDoubleTy()) {
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeFloat, { id, 64 });
    } else if (t->isIntegerTy(1)) {
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeBool, { id });
        boolTypeID = id;
    } else if (t->isIntegerTy(32)) {
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeInt, { id, 32, 1 });  // signed
        int32TypeID = id;
    } else if (t->isIntegerTy()) {
        unsigned bw = t->getIntegerBitWidth();
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeInt, { id, bw, 1 });
    } else if (auto* vt = dyn_cast<llvm::FixedVectorType>(t)) {
        Word elemID = typeFor(vt->getElementType());
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeVector, { id, elemID, (Word)vt->getNumElements() });
    } else if (auto* at = dyn_cast<llvm::ArrayType>(t)) {
        Word elemID = typeFor(at->getElementType());
        Word countID = constUint((uint32_t)at->getNumElements());
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeArray, { id, elemID, countID });
    } else if (auto shape = spvMatrixShape(t)) {
        // A matrix lowers to a real OpTypeMatrix of `cols` column vectors
        Word colVecID = typeFor(
            llvm::FixedVectorType::get(llvm::Type::getFloatTy(M.getContext()), shape->rows));
        id = allocID();
        appendOp(typesGlobals, SpvOpTypeMatrix, { id, colVecID, shape->cols });
    } else if (auto* st = dyn_cast<llvm::StructType>(t)) {
        std::vector<Word> memberIDs;
        for (auto* mt : st->elements())
            memberIDs.push_back(typeFor(mt));
        id = allocID();
        std::vector<Word> ops;
        ops.push_back(id);
        for (Word w : memberIDs)
            ops.push_back(w);
        appendOpV(typesGlobals, SpvOpTypeStruct, ops);
    } else if (t->isPointerTy()) {
        // Opaque pointer: default to Function storage if requested standalone.
        // Most uses go through ptrTypeFor() which knows the storage class.
        id = ptrTypeFor(llvm::Type::getInt32Ty(M.getContext()), SpvStorageClassFunction);
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

inline Word IRToSPIRV::funcTypeForIDs(Word retID,
                                      llvm::ArrayRef<Word> paramIDs) {
  std::vector<Word> ops;
  ops.push_back(0);
  ops.push_back(retID);
  for (Word p : paramIDs) ops.push_back(p);
  Word id = allocID();
  ops[0] = id;
  appendOpV(typesGlobals, SpvOpTypeFunction, ops);
  return id;
}

inline Word IRToSPIRV::paramTypeID(llvm::Argument& A) {
  if (!A.getType()->isPointerTy()) return typeFor(A.getType());
  llvm::Type* pointee = nullptr;
  for (auto* U : A.users()) {
    if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(U)) {
      if (LI->getPointerOperand() == &A) { pointee = LI->getType(); break; }
    } else if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
      if (SI->getPointerOperand() == &A) {
        pointee = SI->getValueOperand()->getType();
        break;
      }
    }
  }
  if (!pointee) pointee = llvm::Type::getInt32Ty(M.getContext());
  return ptrTypeFor(pointee, SpvStorageClassFunction);
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
    // A double would take two words; not needed here.
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
  } else if (auto shape = spvMatrixShape(c->getType())) {
    // Unwrap ConstantStruct and build OpConstantComposite of column vectors for OpTypeMatrix.
    llvm::Constant* arr = c->getAggregateElement(0u);
    std::vector<Word> cols;
    for (unsigned i = 0; i < shape->cols; ++i)
      cols.push_back(lowerConstant(arr->getAggregateElement(i)));
    id = constComposite(c->getType(), cols);
  } else if (isa<llvm::ConstantArray>(c) || isa<llvm::ConstantStruct>(c) ||
             isa<llvm::ConstantDataArray>(c)) {
    // Without this, a constant-folded aggregate (a matrix literal is
    // [C x <R x float>]) falls through to OpConstantNull below, making mat4(1.0)
    // identity on the RISC-V path and a zero matrix on SPIR-V from the same
    // source — and the module still passes validation.
    // ConstantDataArray keeps its elements as raw data rather than operands, so
    // the count has to come from the type, not getNumOperands().
    unsigned n = 0;
    if (auto* at = dyn_cast<llvm::ArrayType>(c->getType()))
      n = (unsigned)at->getNumElements();
    else if (auto* st = dyn_cast<llvm::StructType>(c->getType()))
      n = st->getNumElements();
    std::vector<Word> elems;
    for (unsigned i = 0; i < n; ++i)
      elems.push_back(lowerConstant(c->getAggregateElement(i)));
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

// Module setup
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
  if (G.isConstant()) return false;
  if (!G.getValueType()->isPointerTy()) return false;
  llvm::StringRef nm = G.getName();
  if (nm.contains("_output_floats")) return false;
  if (nm.contains("_varying_floats")) return false;
  if (nm.contains("_total_floats")) return false;
  if (nm.contains("_layout")) return false;
  if (isSamplerGlobal(G)) return false;
  return true;
}

inline bool IRToSPIRV::isSamplerGlobal(llvm::GlobalVariable& G) {
  if (G.isConstant()) return false;
  if (!G.getValueType()->isPointerTy()) return false;
  for (auto* U : G.users()) {
    auto* LI = llvm::dyn_cast<llvm::LoadInst>(U);
    if (!LI) continue;
    for (auto* U2 : LI->users()) {
      auto* CI = llvm::dyn_cast<llvm::CallInst>(U2);
      if (!CI || !CI->getCalledFunction()) continue;
      llvm::StringRef cn = CI->getCalledFunction()->getName();
      if (cn.starts_with("__tex") || cn.starts_with("__image"))
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
  auto& DL = M.getDataLayout();
  uint32_t offset = 0;
  for (uint32_t i = 0; i < uniforms.size(); ++i) {
    llvm::Type* ty = uniforms[i]->getValueType();
    uint32_t sz = (uint32_t)DL.getTypeAllocSize(ty);
    // Align to type size for std430 (vec3 aligned to 16, vec2 to 8, float to 4).
    uint32_t align = sz;
    auto matShape = spvMatrixShape(ty);
    if (auto* vt = dyn_cast<llvm::FixedVectorType>(ty)) {
      unsigned n = vt->getNumElements();
      unsigned elemSz = (unsigned)DL.getTypeAllocSize(vt->getElementType());
      align = (n == 3 ? 4 : n) * elemSz;
    } else if (matShape) {
      // A matrix column is a <rows x float> vector; its base alignment is the
      // MatrixStride, and the matrix member itself aligns to that.
      align = (matShape->rows == 3 ? 4 : matShape->rows) * 4;
    }
    offset = (offset + align - 1) & ~(align - 1);
    appendOp(annotations, SpvOpMemberDecorate,
             {pushConstStructTypeID, i, SpvDecorationOffset, offset});
    if (matShape) {
      // std140/std430 require ColMajor + MatrixStride on a matrix block member.
      uint32_t stride = (matShape->rows == 3 ? 4 : matShape->rows) * 4;
      appendOp(annotations, SpvOpMemberDecorate,
               {pushConstStructTypeID, i, SpvDecorationColMajor});
      appendOp(annotations, SpvOpMemberDecorate,
               {pushConstStructTypeID, i, SpvDecorationMatrixStride, stride});
    }
    uniformMemberIdx[uniforms[i]] = i;
    offset += sz;
  }
}

// Explicit layout(binding=N) recorded on a sampler/SSBO global by codegen
// (ast.cpp), or -1 when the shader gave none — in which case the caller keeps
// its positional counter.
static inline int explicitBindingOf(llvm::GlobalVariable& G) {
  if (auto* md = G.getMetadata("shader.binding"))
    return (int)llvm::mdconst::extract<llvm::ConstantInt>(md->getOperand(0))
        ->getSExtValue();
  return -1;
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

    int explicitB = explicitBindingOf(G);
    uint32_t slot = explicitB >= 0 ? (uint32_t)explicitB : binding++;

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
             {varID, SpvDecorationBinding, slot});

    // OpTypePointer Uniform <elem> — used as the result type of OpAccessChain.
    Word elemPtrTypeID = ptrTypeFor(elemTy, SpvStorageClassUniform);

    SSBOInfo info;
    info.varID = varID;
    info.elemPtrTypeID = elemPtrTypeID;
    info.binding = slot;
    ssboInfo[&G] = info;
  }
}

inline void IRToSPIRV::analyzeSamplers() {
  using namespace llvm;

  uint32_t binding = 0;
  bool anyStorage = false, anyBuffer = false;
  for (auto& G : M.globals()) {
    if (!isSamplerGlobal(G)) continue;

    int explicitB = explicitBindingOf(G);
    uint32_t slot = explicitB >= 0 ? (uint32_t)explicitB : binding++;

    // Decode dimensionality from shader.sampler.kind metadata
    // (dimCode 0=2D 1=3D 2=Cube 3=Buffer | arrayed<<4 | isImage<<5). The LLVM
    // type is an opaque ptr, so the dim can only come from here.
    int code = 0;
    if (auto* md = G.getMetadata("shader.sampler.kind"))
      code = (int)llvm::mdconst::extract<llvm::ConstantInt>(md->getOperand(0))
                 ->getZExtValue();
    int dimCode = code & 0xF;
    bool arrayed = (code >> 4) & 1;
    bool isImage = (code >> 5) & 1;
    SpvDim dim = dimCode == 1 ? SpvDim3D
                 : dimCode == 2 ? SpvDimCube
                 : dimCode == 3 ? SpvDimBuffer
                                : SpvDim2D;
    uint32_t sampled = isImage ? 2u : 1u;  // 2 = storage image, 1 = sampled
    if (isImage) { anyStorage = true; if (dim == SpvDimBuffer) anyBuffer = true; }

    Word floatTID = typeFor(Type::getFloatTy(M.getContext()));

    // OpTypeImage %float <dim> depth=0 arrayed ms=0 sampled format=Unknown
    Word imageTID = allocID();
    appendOp(typesGlobals, SpvOpTypeImage,
             {imageTID, floatTID, (Word)dim, 0u, arrayed ? 1u : 0u, 0u, sampled,
              SpvImageFormatUnknown});

    // Sampled textures wrap the image in OpTypeSampledImage; a storage image is
    // accessed directly, so the pointer points at the image itself.
    Word sampledImageTID = 0;
    Word ptrPointeeTID = imageTID;
    if (!isImage) {
      sampledImageTID = allocID();
      appendOp(typesGlobals, SpvOpTypeSampledImage, {sampledImageTID, imageTID});
      ptrPointeeTID = sampledImageTID;
    }

    Word ptrTID = allocID();
    appendOp(typesGlobals, SpvOpTypePointer,
             {ptrTID, SpvStorageClassUniformConstant, ptrPointeeTID});

    Word varID = allocID();
    appendOp(typesGlobals, SpvOpVariable,
             {ptrTID, varID, SpvStorageClassUniformConstant});

    appendOp(annotations, SpvOpDecorate,
             {varID, SpvDecorationDescriptorSet, 0u});
    appendOp(annotations, SpvOpDecorate,
             {varID, SpvDecorationBinding, slot});

    SamplerInfo info;
    info.varID = varID;
    info.sampledImageTypeID = sampledImageTID;
    info.imageTypeID = imageTID;
    info.binding = slot;
    info.dim = dim;
    info.arrayed = arrayed;
    info.isImage = isImage;
    samplerInfo[&G] = info;
  }
  // Storage images with an Unknown format require these capabilities for
  // OpImageRead/OpImageWrite; a buffer image also needs ImageBuffer.
  if (anyStorage) {
    appendOp(capabilities, SpvOpCapability,
             {(Word)SpvCapabilityStorageImageReadWithoutFormat});
    appendOp(capabilities, SpvOpCapability,
             {(Word)SpvCapabilityStorageImageWriteWithoutFormat});
  }
  if (anyBuffer)
    appendOp(capabilities, SpvOpCapability, {(Word)SpvCapabilityImageBuffer});
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

  // Explicit `layout(location=N)` slots, keyed by in/out var name, as recorded
  // on the entry function by ast.cpp. A name present here is decorated with that
  // exact Location (matching VS-out to FS-in across modules); anything else falls
  // back to the positional counters below.
  std::unordered_map<std::string, uint32_t> explicitLoc;
  if (auto* md = entryFn->getMetadata("shader.io.location")) {
    for (auto& op : md->operands()) {
      auto* t = llvm::cast<llvm::MDNode>(op.get());
      auto name = llvm::cast<llvm::MDString>(t->getOperand(0))->getString();
      auto loc = llvm::mdconst::extract<llvm::ConstantInt>(t->getOperand(1))
                     ->getZExtValue();
      explicitLoc[name.str()] = static_cast<uint32_t>(loc);
    }
  }

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
      auto it = explicitLoc.find(A.getName().str());
      const uint32_t slots = spvLocationSlots(A.getType());
      uint32_t loc = it != explicitLoc.end() ? it->second : inputLoc;
      if (it == explicitLoc.end()) inputLoc += slots;
      vid = declareInputLocation(A, loc);
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
      auto it = explicitLoc.find("FragColor");
      uint32_t loc = it != explicitLoc.end() ? it->second : 0;
      appendOp(annotations, SpvOpDecorate, {vid, SpvDecorationLocation, loc});
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
  // IR contains them in REVERSE declaration order. Iterating in reverse recovers
  // declaration order, which is what the FS expects for Location numbering
  // (FS reads Location 0 = first VS out-var, Location 1 = second).
  if (stage == ShaderStage::Vertex) {
    std::vector<llvm::AllocaInst*> vsOutAllocas;
    for (auto& I : EB) {
      auto* AI = dyn_cast<llvm::AllocaInst>(&I);
      if (!AI || !AI->hasName()) continue;
      if (AI->getAllocatedType()->isPointerTy()) continue;
      llvm::StringRef n = AI->getName();
      if (n == "FragColor" || n == "gl_Position") continue;
      if (!n.starts_with("v")) continue;
      // A float-vector or a matrix varying (a matrix spans its C columns'
      // locations; spvLocationSlots accounts for that). Anything else is skipped.
      auto* allocTy = AI->getAllocatedType();
      auto* vt = dyn_cast<llvm::FixedVectorType>(allocTy);
      bool floatVec = vt && vt->getElementType()->isFloatTy();
      if (!floatVec && !spvMatrixShape(allocTy)) continue;
      vsOutAllocas.push_back(AI);
    }
    uint32_t outLoc = 0;
    for (auto it = vsOutAllocas.rbegin(); it != vsOutAllocas.rend(); ++it) {
      llvm::AllocaInst* AI = *it;
      Word ptrID = ptrTypeFor(AI->getAllocatedType(), SpvStorageClassOutput);
      Word vid = allocID();
      appendOp(typesGlobals, SpvOpVariable,
               {ptrID, vid, SpvStorageClassOutput});
      auto le = explicitLoc.find(AI->getName().str());
      const uint32_t slots = spvLocationSlots(AI->getAllocatedType());
      uint32_t loc = le != explicitLoc.end() ? le->second : outLoc;
      if (le == explicitLoc.end()) outLoc += slots;
      appendOp(annotations, SpvOpDecorate,
               {vid, SpvDecorationLocation, loc});
      allocaToOutputVar[AI] = vid;
      entryPointInterface.push_back(vid);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// CFG analysis: detect loops and selections by block-name pattern.
// Codegen always names them: for.cond / for.body / for.inc / for.end,
// then / else / ifend, logical.rhs / logical.merge.
// ─────────────────────────────────────────────────────────────────────────────
inline void IRToSPIRV::analyzeCFG(llvm::Function& F) {
  selectionMerge.clear();
  loopMerge.clear();
  loopContinue.clear();

  // Used to locate a loop's back-edge latch (the continue target) precisely,
  // including nested loops: the latch is the in-loop predecessor of the header,
  // i.e. the predecessor the header dominates.
  llvm::DominatorTree DT(F);

  auto findByPrefix = [&](llvm::StringRef pfx) -> llvm::BasicBlock* {
    for (auto& BB : F)
      if (BB.getName().starts_with(pfx)) return &BB;
    return nullptr;
  };

  // Walk all basic blocks; classify by name.
  for (auto& BB : F) {
    llvm::StringRef n = BB.getName();
    if (n.starts_with("for.cond") || n.starts_with("while.cond")) {
      auto* term = BB.getTerminator();
      if (auto* br = dyn_cast<llvm::BranchInst>(term)) {
        if (br->isConditional()) {
          llvm::BasicBlock* mergeBB = nullptr;
          for (auto* succ : llvm::successors(&BB)) {
            llvm::StringRef sn = succ->getName();
            if (sn.starts_with("for.end") || sn.starts_with("while.end"))
              mergeBB = succ;
          }
          llvm::BasicBlock* contBB = nullptr;
          for (auto* pred : llvm::predecessors(&BB)) {
            // The pre-header sits outside the loop (not dominated by the
            // header); the back-edge latch is inside it (dominated).
            if (pred != &BB && DT.dominates(&BB, pred)) {
              contBB = pred;
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
          // Codegen emits "then" and "else" both branching to "ifend".
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
    // Some codegen builtins map to extinsts as user fn names — those are not
    // handled here.
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
    // SPIR-V — %p is only recorded as standing in for that SSBO so downstream
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
      // tail (shaders produce no user-pointer values).
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
    // Load through a constant GEP into a uniform matrix (`M[2]`, `M[1][3]`): a
    // literal index folds the GEP into a ConstantExpr in the load pointer. Build
    // the push-constant access chain and load from it. (A dynamic `M[i]` is a
    // separate GEP instruction, lowered by the GEP handler below.)
    if (auto* ce = dyn_cast<llvm::ConstantExpr>(ptr)) {
      if (ce->getOpcode() == llvm::Instruction::GetElementPtr) {
        if (Word ac = uniformMatrixAccessChain(cast<llvm::GEPOperator>(ce), out)) {
          Word resTy = typeFor(LI->getType());
          Word resID = allocID();
          valueIDs[&I] = resID;
          appendOp(out, SpvOpLoad, {resTy, resID, ac});
          return;
        }
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
    // GEP whose base is a uniform matrix global (dynamic `M[i]`): access-chain
    // into the push-constant block rather than treating @M as a standalone var.
    if (Word ac = uniformMatrixAccessChain(cast<GEPOperator>(GEP), out)) {
      valueIDs[&I] = ac;
      return;
    }
    // Generic GEP on a Function-storage aggregate (local array element or
    // struct field, possibly chained: `s.a[i]`) → OpAccessChain. The first LLVM
    // index addresses the base pointer itself (always 0 for an alloca) and is
    // implicit in a SPIR-V access chain, so drop it; the remaining indices walk
    // into the pointee. Struct-member indices are ConstantInts, which valueOf
    // lowers to the OpConstant that OpAccessChain requires there.
    Word baseID = valueOf(basePtr);
    if (baseID == 0) {
      deadValues.insert(&I);
      valueIDs[&I] = 0;
      return;
    }
    // The result pointer's storage class must match the base's. Local allocas
    // are Function, but an arrayed in/out var's shadow alloca is mapped to an
    // Input/Output interface variable — index that in its own storage class.
    SpvStorageClass sc = SpvStorageClassFunction;
    if (auto* AI = dyn_cast<AllocaInst>(basePtr)) {
      if (allocaToInputVar.count(AI)) sc = SpvStorageClassInput;
      else if (allocaToOutputVar.count(AI)) sc = SpvStorageClassOutput;
    }
    Word resPtrTy = ptrTypeFor(GEP->getResultElementType(), sc);
    Word acID = allocID();
    valueIDs[&I] = acID;
    std::vector<Word> ops = {resPtrTy, acID, baseID};
    // Drop the leading 0 (the base-pointer deref, implicit in an access chain).
    // Also drop any matrix-wrapper struct-member index: an OpTypeMatrix has no
    // wrapper struct, so the column index that follows addresses it directly.
    // Walking the GEP's indexed types finds that member wherever it nests (a
    // plain matrix `m[i]` → `%m,0,0,i`; an array of matrices `arr[i][j]` →
    // `%arr,0,i,0,j`).
    auto GTI = llvm::gep_type_begin(GEP);
    bool firstIdx = true;
    for (auto& idx : GEP->indices()) {
      llvm::StructType* container = GTI.getStructTypeOrNull();
      ++GTI;
      if (firstIdx) { firstIdx = false; continue; }
      if (container && spvMatrixShape(container)) continue;
      ops.push_back(valueOf(idx.get()));
    }
    appendOpV(out, SpvOpAccessChain, ops);
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
      // On i1 (bool) operands SPIR-V requires the Logical* ops — OpBitwise* is
      // only defined for integers. `mat == mat` and-reduces per-lane <R x i1>
      // comparisons (and CreateNot → `xor i1 x, true`), so without this the
      // bool reduction emits an invalid OpBitwiseAnd/Xor.
      case Instruction::And:
        op = BO->getType()->isIntegerTy(1) ? SpvOpLogicalAnd : SpvOpBitwiseAnd;
        break;
      case Instruction::Or:
        op = BO->getType()->isIntegerTy(1) ? SpvOpLogicalOr : SpvOpBitwiseOr;
        break;
      case Instruction::Xor:
        op = BO->getType()->isIntegerTy(1) ? SpvOpLogicalNotEqual
                                           : SpvOpBitwiseXor;
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

  // Aggregate field access (struct members): extractvalue / insertvalue carry
  // constant LITERAL indices, lowering to OpCompositeExtract / OpCompositeInsert
  // (distinct from the vector Extract/InsertElement above, whose index is an id).
  if (auto* EV = dyn_cast<ExtractValueInst>(&I)) {
    // Peel `extractvalue matWrapper, 0`: matrix has no wrapper struct under OpTypeMatrix.
    // Column reads (`extractvalue mat, c`) fall through to standard OpCompositeExtract.
    if (spvMatrixShape(EV->getAggregateOperand()->getType())) {
      valueIDs[&I] = valueOf(EV->getAggregateOperand());
      return;
    }
    Word agg = valueOf(EV->getAggregateOperand());
    Word ty = typeFor(EV->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    std::vector<Word> ops = {ty, id, agg};
    for (unsigned idx : EV->indices()) ops.push_back(idx);
    appendOpV(out, SpvOpCompositeExtract, ops);
    return;
  }

  if (auto* IV = dyn_cast<InsertValueInst>(&I)) {
    // Wrap `insertvalue matWrapper undef, cols, {0}`: alias to the columns value
    // (itself emitted as an OpTypeMatrix); no instruction needed.
    if (spvMatrixShape(IV->getType())) {
      valueIDs[&I] = valueOf(IV->getInsertedValueOperand());
      return;
    }
    Word el = valueOf(IV->getInsertedValueOperand());
    Word id = allocID();
    valueIDs[&I] = id;
    // Matrix column build (`insertvalue matColumns, col, {c}`): emit an
    // OpCompositeInsert into the OpTypeMatrix, not the OpTypeArray. A constant
    // undef/zero base is synthesized fresh — the shared inner-array constant must
    // not be reused as a matrix.
    if (auto mcv = matColVals.find(&I); mcv != matColVals.end()) {
      std::string mn = "glsl.mat" + std::to_string(mcv->second.cols) + "x" +
                       std::to_string(mcv->second.rows);
      if (llvm::Type* matTy =
              llvm::StructType::getTypeByName(M.getContext(), mn)) {
        Word ty = typeFor(matTy);
        llvm::Value* aggV = IV->getAggregateOperand();
        Word agg = isa<llvm::Constant>(aggV) ? constNull(matTy) : valueOf(aggV);
        std::vector<Word> ops = {ty, id, el, agg};
        for (unsigned idx : IV->indices()) ops.push_back(idx);
        appendOpV(out, SpvOpCompositeInsert, ops);
        return;
      }
    }
    Word agg = valueOf(IV->getAggregateOperand());
    Word ty = typeFor(IV->getType());
    std::vector<Word> ops = {ty, id, el, agg};
    for (unsigned idx : IV->indices()) ops.push_back(idx);
    appendOpV(out, SpvOpCompositeInsert, ops);
    return;
  }

  // shufflevector — selects components from two vectors by a constant mask.
  // GLSL scalar→vector splats (`vec4(x)`) lower to `insertelement poison, x, 0`
  // followed by a splat shuffle (mask all-zeros), so this MUST be handled or
  // the shuffle result stays an undefined forward reference.
  if (auto* SV = dyn_cast<ShuffleVectorInst>(&I)) {
    Word v1 = valueOf(SV->getOperand(0));
    Word v2 = valueOf(SV->getOperand(1));
    Word ty = typeFor(SV->getType());
    Word id = allocID();
    valueIDs[&I] = id;
    llvm::SmallVector<int, 16> mask;
    SV->getShuffleMask(mask);
    std::vector<Word> ops = {ty, id, v1, v2};
    // LLVM uses -1 for an undef/poison lane; SPIR-V spells that 0xFFFFFFFF.
    for (int m : mask)
      ops.push_back(m < 0 ? 0xFFFFFFFFu : (Word)m);
    appendOpV(out, SpvOpVectorShuffle, ops);
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
    // Arg layout (all sampler/image intrinsics): (sampler, i32 slot, coords…,
    // out/value). The slot (arg 1) is RISC-V-only; SPIR-V reads the descriptor
    // via valueToSampler and ignores it.
    if (nm == "__tex2d_sample" && CALL->arg_size() == 5) {
      Value* samplerArg = CALL->getArgOperand(0);
      auto it = valueToSampler.find(samplerArg);
      if (it == valueToSampler.end()) {
        llvm::errs() << "[spv] __tex2d_sample: sampler arg not tracked\n";
        emitFailed_ = true;
        return;
      }
      const SamplerInfo& info = samplerInfo[it->second];
      Word loadedID = allocID();
      appendOp(out, SpvOpLoad,
               {info.sampledImageTypeID, loadedID, info.varID});
      auto* vec2Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 2);
      Word uvID = allocID();
      appendOp(out, SpvOpCompositeConstruct,
               {typeFor(vec2Ty), uvID, valueOf(CALL->getArgOperand(2)),
                valueOf(CALL->getArgOperand(3))});
      auto* vec4Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 4);
      Word sampleID = allocID();
      appendOp(out, SpvOpImageSampleImplicitLod,
               {typeFor(vec4Ty), sampleID, loadedID, uvID});
      appendOp(out, SpvOpStore, {valueOf(CALL->getArgOperand(4)), sampleID});
      return;
    }

    // texture(sampler{Cube,3D,2DArray}, vec3) → __texcube_sample(ptr, x,y,z,out).
    // A single vec3 sample covers all three: the OpTypeImage dim comes from the
    // sampler descriptor (analyzeSamplers reads it from metadata), so cube uses a
    // direction, 3D a volume coord, 2DArray (x,y,layer). (The RISC-V runtime
    // needs distinct functions per dim; SPIR-V does not.)
    if ((nm == "__texcube_sample" || nm == "__tex3d_sample" ||
         nm == "__tex2darray_sample") && CALL->arg_size() == 6) {
      Value* samplerArg = CALL->getArgOperand(0);
      auto it = valueToSampler.find(samplerArg);
      if (it == valueToSampler.end()) {
        llvm::errs() << "[spv] " << nm << ": sampler arg not tracked\n";
        emitFailed_ = true;
        return;
      }
      const SamplerInfo& info = samplerInfo[it->second];
      Word loadedID = allocID();
      appendOp(out, SpvOpLoad,
               {info.sampledImageTypeID, loadedID, info.varID});
      auto* vec3Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 3);
      Word coordID = allocID();
      appendOp(out, SpvOpCompositeConstruct,
               {typeFor(vec3Ty), coordID, valueOf(CALL->getArgOperand(2)),
                valueOf(CALL->getArgOperand(3)),
                valueOf(CALL->getArgOperand(4))});
      auto* v4Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 4);
      Word sampleID = allocID();
      appendOp(out, SpvOpImageSampleImplicitLod,
               {typeFor(v4Ty), sampleID, loadedID, coordID});
      appendOp(out, SpvOpStore, {valueOf(CALL->getArgOperand(5)), sampleID});
      return;
    }

    // textureLod → __tex{2d,cube,3d,2darray}_sample_lod. Explicit-LOD sample
    // (OpImageSampleExplicitLod + Lod operand). 2D coord is vec2 (arg_size 6);
    // cube/3D/array coord is vec3 (arg_size 7). Layout: (sampler, slot, coords…,
    // lod, out).
    if (nm == "__tex2d_sample_lod" || nm == "__texcube_sample_lod" ||
        nm == "__tex3d_sample_lod" || nm == "__tex2darray_sample_lod") {
      bool vec3 = (nm != "__tex2d_sample_lod");
      Value* samplerArg = CALL->getArgOperand(0);
      auto it = valueToSampler.find(samplerArg);
      if (it == valueToSampler.end()) {
        llvm::errs() << "[spv] " << nm << ": sampler arg not tracked\n";
        emitFailed_ = true;
        return;
      }
      const SamplerInfo& info = samplerInfo[it->second];
      Word loadedID = allocID();
      appendOp(out, SpvOpLoad,
               {info.sampledImageTypeID, loadedID, info.varID});
      unsigned n = vec3 ? 3u : 2u;
      auto* coordTy = FixedVectorType::get(Type::getFloatTy(M.getContext()), n);
      std::vector<Word> cc = {typeFor(coordTy), 0};
      for (unsigned i = 0; i < n; ++i)
        cc.push_back(valueOf(CALL->getArgOperand(2 + i)));
      Word coordID = allocID();
      cc[1] = coordID;
      appendOpV(out, SpvOpCompositeConstruct, cc);
      // lod and out are the last two operands.
      Word lodID = valueOf(CALL->getArgOperand(CALL->arg_size() - 2));
      auto* v4Ty = FixedVectorType::get(Type::getFloatTy(M.getContext()), 4);
      Word sampleID = allocID();
      appendOp(out, SpvOpImageSampleExplicitLod,
               {typeFor(v4Ty), sampleID, loadedID, coordID,
                SpvImageOperandsLodMask, lodID});
      appendOp(out, SpvOpStore,
               {valueOf(CALL->getArgOperand(CALL->arg_size() - 1)), sampleID});
      return;
    }

    // Storage-image load/store → OpImageRead / OpImageWrite. The image global
    // points directly at the OpTypeImage (analyzeSamplers, isImage). image2D uses
    // an ivec2 coord (x,y); imageBuffer a scalar int.
    if (nm == "__image2d_load" || nm == "__imagebuffer_load" ||
        nm == "__image2d_store" || nm == "__imagebuffer_store") {
      bool isBuf = nm.contains("buffer");
      bool isStore = nm.contains("store");
      auto it = valueToSampler.find(CALL->getArgOperand(0));
      if (it == valueToSampler.end()) {
        llvm::errs() << "[spv] " << nm << ": image arg not tracked\n";
        emitFailed_ = true;
        return;
      }
      const SamplerInfo& info = samplerInfo[it->second];
      Word imgID = allocID();
      appendOp(out, SpvOpLoad, {info.imageTypeID, imgID, info.varID});
      // Coordinate (after sampler+slot): scalar int for buffer at arg 2, ivec2
      // for image2D at args 2,3.
      Word coordID;
      if (isBuf) {
        coordID = valueOf(CALL->getArgOperand(2));
      } else {
        auto* iv2 = FixedVectorType::get(Type::getInt32Ty(M.getContext()), 2);
        coordID = allocID();
        appendOp(out, SpvOpCompositeConstruct,
                 {typeFor(iv2), coordID, valueOf(CALL->getArgOperand(2)),
                  valueOf(CALL->getArgOperand(3))});
      }
      if (isStore) {
        // The value is the last operand, passed as a ptr (alloca) — OpLoad it.
        auto* v4 = FixedVectorType::get(Type::getFloatTy(M.getContext()), 4);
        Word valPtr = valueOf(CALL->getArgOperand(CALL->arg_size() - 1));
        Word valID = allocID();
        appendOp(out, SpvOpLoad, {typeFor(v4), valID, valPtr});
        appendOp(out, SpvOpImageWrite, {imgID, coordID, valID});
      } else {
        // args: (img, coord..., out-ptr). out is the last operand.
        auto* v4 = FixedVectorType::get(Type::getFloatTy(M.getContext()), 4);
        Word readID = allocID();
        appendOp(out, SpvOpImageRead, {typeFor(v4), readID, imgID, coordID});
        appendOp(out, SpvOpStore,
                 {valueOf(CALL->getArgOperand(CALL->arg_size() - 1)), readID});
      }
      return;
    }

    // Fail loudly on an unlowered sampler/image intrinsic instead of emitting an
    // OpFunctionCall to a body-less external (which writes an invalid module and
    // exits 0). Every implemented dim is lowered above; anything left here is a
    // gap, not a user call.
    if (nm.starts_with("__tex") || nm.starts_with("__image")) {
      llvm::errs() << "[spv] unsupported sampler/image intrinsic '" << nm
                   << "' — not lowered by the SPIR-V backend\n";
      emitFailed_ = true;
      return;
    }

    // User-defined function calls — absent from the shaders today, but supported
    // anyway (helpers from codegen_state are inlined as intrinsics, so this
    // rarely fires).
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

// Classify the inner column-array SSA values that are really matrix columns, so
// their building insertvalue is emitted into an OpTypeMatrix. Seeds from matrix
// wraps (`insertvalue matWrapper, cols, {0}`) and propagates backward through the
// column-build insertvalue chain. Anchoring on actual wrapper instructions keeps
// a genuine `vec[C]` array (identical LLVM type) from being misclassified.
inline void IRToSPIRV::buildMatColVals(llvm::Function& F) {
  matColVals.clear();
  std::vector<const llvm::Value*> work;
  for (auto& BB : F)
    for (auto& I : BB)
      if (auto* IV = dyn_cast<llvm::InsertValueInst>(&I))
        if (auto shape = spvMatrixShape(IV->getType())) {
          const llvm::Value* col = IV->getInsertedValueOperand();
          if (isa<llvm::Instruction>(col) && !matColVals.count(col)) {
            matColVals[col] = *shape;
            work.push_back(col);
          }
        }
  while (!work.empty()) {
    const llvm::Value* v = work.back();
    work.pop_back();
    SpvMatShape shape = matColVals[v];
    if (auto* IV = dyn_cast<llvm::InsertValueInst>(v))
      if (IV->getNumIndices() == 1) {
        const llvm::Value* agg = IV->getAggregateOperand();
        if (isa<llvm::InsertValueInst>(agg) && !matColVals.count(agg)) {
          matColVals[agg] = shape;
          work.push_back(agg);
        }
      }
  }
}

inline Word IRToSPIRV::uniformMatrixAccessChain(llvm::GEPOperator* gep,
                                                std::vector<Word>& out) {
  auto* GV = dyn_cast<llvm::GlobalVariable>(gep->getPointerOperand());
  if (!GV) return 0;
  auto it = uniformMemberIdx.find(GV);
  if (it == uniformMemberIdx.end()) return 0;
  // {pushConstVar, memberIdx, <remaining GEP indices, matrix-wrapper member
  // stripped>}. The wrapper strip mirrors the local matrix GEP (an OpTypeMatrix
  // has no wrapper struct, so the column index addresses it directly).
  Word resPtrTy =
      ptrTypeFor(gep->getResultElementType(), SpvStorageClassPushConstant);
  Word acID = allocID();
  std::vector<Word> ops = {resPtrTy, acID, pushConstVarID,
                           constUint(it->second)};
  auto GTI = llvm::gep_type_begin(gep);
  bool firstIdx = true;
  for (auto& idx : gep->indices()) {
    llvm::StructType* container = GTI.getStructTypeOrNull();
    ++GTI;
    if (firstIdx) { firstIdx = false; continue; }
    if (container && spvMatrixShape(container)) continue;
    ops.push_back(valueOf(idx.get()));
  }
  appendOpV(out, SpvOpAccessChain, ops);
  return acID;
}

inline void IRToSPIRV::emitFunction(llvm::Function& F) {
  using namespace llvm;
  if (F.isDeclaration()) return;
  if (isTrampolineFunction(F)) return;

  // The entry function gets a particular id; helpers get fresh ids.
  Word fnID = valueOf(&F);

  // Build function type. The entry function's signature is forced to
  // `void()` — all inputs/outputs are routed through globals. Param type ids are
  // computed via paramTypeID so opaque-pointer (out/inout) params get the right
  // pointee, and the OpFunctionParameter decls reuse the very same ids.
  llvm::Type* retTy =
      (&F == entryFn) ? llvm::Type::getVoidTy(M.getContext()) : F.getReturnType();
  std::vector<Word> paramTypeIDs;
  if (&F != entryFn)
    for (auto& A : F.args()) paramTypeIDs.push_back(paramTypeID(A));
  Word fnTypeID = funcTypeForIDs(typeFor(retTy), paramTypeIDs);

  appendOp(functions, SpvOpFunction,
           {typeFor(retTy), fnID, SpvFunctionControlMaskNone, fnTypeID});

  // Function parameters (skipped for entry function).
  if (&F != entryFn) {
    unsigned k = 0;
    for (auto& A : F.args()) {
      Word id = allocID();
      valueIDs[&A] = id;
      appendOp(functions, SpvOpFunctionParameter, {paramTypeIDs[k++], id});
    }
  }

  analyzeCFG(F);
  buildMatColVals(F);

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
  // For this structured CFG it also keeps loop headers before their bodies
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
    // OpEntryPoint name is encoded BEFORE interface ids, mirroring appendOpStr's
    // prefix/suffix split. The interface ids come *after* the name string,
    // so the encoding is hand-rolled here.
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

  // An unsupported construct was hit — return no module so the driver fails
  // rather than writing an invalid `.spv`.
  if (emitFailed_) return {};

  // ── Stitch sections in the order required by the SPIR-V spec. ────────────
  std::vector<Word> stream;
  // Header: magic, version (1.0), generator, id-bound, schema(0)
  stream.push_back(SpvMagicNumber);
  stream.push_back(0x00010000);  // pin to SPIR-V 1.0 for Vulkan 1.0 compat
  stream.push_back(0x474C534C);  // generator magic ("GLSL"-ish), this emitter's marker
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
