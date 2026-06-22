// ast/type.h — the language's semantic type system.
//
// Modelled on llvm::Type: every distinct type has exactly ONE instance, owned
// and uniqued by the ASTContext (see ASTContext::get*Ty). That makes type
// equality a pointer comparison — `a == b` instead of string compares — and
// lets the rest of the frontend pass `const Type*` around cheaply.
//
// Types are created only through ASTContext (the constructors are private,
// ASTContext is a friend). They are allocated in the context's bump pool and
// are trivially destructible (no owning members — the struct name is an
// interned StringRef), so they need no destructor registration.
//
// Step 1 of the type-system bring-up: this header is purely additive. Nothing
// reads a Type yet; SemanticAnalyzer will start stamping ExprAST nodes with one
// (Step 2), after which codegen dispatches on it instead of assuming float.
#ifndef AST_TYPE_H
#define AST_TYPE_H

#include <llvm/ADT/StringRef.h>

#include <cassert>
#include <cstdint>
#include <string>

class ASTContext;        // global fwd — the factory; befriended below.
class StructDeclExprAST;  // global fwd — a struct type points back at its decl.

// Scoped as `glsl::Type` so it never collides with `llvm::Type`, which the
// codegen translation units pull in via `using namespace llvm`. Same split
// Clang uses (clang::Type vs llvm::Type).
namespace glsl {

class Type {
 public:
  enum class Kind : uint8_t {
    Void,
    Bool,
    Int,
    Uint,
    Float,
    Double,
    Vector,   // elem scalar × N components  (vec/ivec/uvec)
    Matrix,   // cols × rows of float
    Array,    // elem × N  (N == 0 ⇒ runtime-sized, e.g. SSBO last member)
    Sampler,  // opaque sampler/image handle
    Struct,   // user-defined aggregate, points at its declaration
  };

  // Generated from builtin_types.def (the single source of truth shared with
  // the lexer, parser, sema, and codegen, so the type lists can't diverge).
  enum class SamplerKind : uint8_t {
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) Kind,
#include "builtin_types.def"
  };

  Kind kind() const { return kind_; }

  // ── Predicates ─────────────────────────────────────────────────────────────
  bool isVoid() const { return kind_ == Kind::Void; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isInt() const { return kind_ == Kind::Int; }
  bool isUint() const { return kind_ == Kind::Uint; }
  bool isFloat() const { return kind_ == Kind::Float; }
  bool isDouble() const { return kind_ == Kind::Double; }
  bool isVector() const { return kind_ == Kind::Vector; }
  bool isMatrix() const { return kind_ == Kind::Matrix; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isSampler() const { return kind_ == Kind::Sampler; }
  bool isStruct() const { return kind_ == Kind::Struct; }

  // A runtime-sized array (the `[]` of an SSBO's trailing member). Sized arrays
  // report their length via arraySize(); unsized ones report 0.
  bool isUnsizedArray() const { return kind_ == Kind::Array && arraySize_ == 0; }

  bool isScalar() const {
    return kind_ == Kind::Bool || kind_ == Kind::Int || kind_ == Kind::Uint ||
           kind_ == Kind::Float || kind_ == Kind::Double;
  }
  // Integer-family scalar (signedness lives in the operations, not the type —
  // this is what tells BinaryExprAST to pick sdiv/ashr vs udiv/lshr).
  bool isIntegral() const { return kind_ == Kind::Int || kind_ == Kind::Uint; }
  bool isFloatingPoint() const {
    return kind_ == Kind::Float || kind_ == Kind::Double;
  }

  // ── Accessors (assert the kind matches) ────────────────────────────────────
  const Type* elementType() const {
    assert(kind_ == Kind::Vector && "elementType() on non-vector");
    return elem_;
  }
  unsigned vectorSize() const {
    assert(kind_ == Kind::Vector && "vectorSize() on non-vector");
    return a_;
  }
  unsigned matrixCols() const {
    assert(kind_ == Kind::Matrix && "matrixCols() on non-matrix");
    return a_;
  }
  unsigned matrixRows() const {
    assert(kind_ == Kind::Matrix && "matrixRows() on non-matrix");
    return b_;
  }
  const Type* arrayElementType() const {
    assert(kind_ == Kind::Array && "arrayElementType() on non-array");
    return elem_;
  }
  // Element count, or 0 for a runtime-sized array (see isUnsizedArray()).
  unsigned arraySize() const {
    assert(kind_ == Kind::Array && "arraySize() on non-array");
    return arraySize_;
  }
  SamplerKind samplerKind() const {
    assert(kind_ == Kind::Sampler && "samplerKind() on non-sampler");
    return sampler_;
  }
  llvm::StringRef structName() const {
    assert(kind_ == Kind::Struct && "structName() on non-struct");
    return name_;
  }
  // The declaration this struct type was built from, or nullptr if the type is
  // still incomplete (the name has been seen but the body hasn't been processed
  // yet — the C "incomplete type" state).
  //
  // Completeness is what makes forward references and cycles fall out without a
  // separate graph DFS, *provided the analyzer follows C's rule*: a struct-
  // typed field requires a COMPLETE element type at the point of the field
  // declaration. With that rule,
  //     struct A { B b; };   // error: 'B' is incomplete here
  //     struct B { A a; };
  // is diagnosed on A's first field and the A→B→A cycle can never form. (A
  // shader language has no pointers/references, so there's no legal indirection
  // that would allow a cycle — C's rule is the complete answer.)
  const ::StructDeclExprAST* structDecl() const {
    assert(kind_ == Kind::Struct && "structDecl() on non-struct");
    return decl_;
  }
  bool isIncompleteStruct() const {
    return kind_ == Kind::Struct && decl_ == nullptr;
  }
  // Complete an incomplete struct type. The ONE sanctioned mutation of a Type
  // after creation — called once by SemanticAnalyzer when it processes the
  // struct body. `decl_` is mutable so this works through the `const Type*`
  // that the rest of the frontend holds (Clang completes RecordDecls the same
  // lazy way).
  //
  // CONTRACT: the caller MUST check isIncompleteStruct() first. A second
  // definition (`struct Foo {...}; struct Foo {...};`) is a *user error* the
  // analyzer diagnoses ("redefinition of 'struct Foo'") — it must not reach
  // here. The assert below is only a backstop against an analyzer bug, never a
  // substitute for that diagnostic (in a release build it vanishes and the
  // redefinition would silently win, which is exactly why the check belongs in
  // the caller).
  void setStructDecl(const ::StructDeclExprAST* decl) const {
    assert(kind_ == Kind::Struct && "setStructDecl() on non-struct");
    assert(decl_ == nullptr && "struct completed twice — caller must check "
                               "isIncompleteStruct() and diagnose redefinition");
    decl_ = decl;
  }

  // GLSL-style spelling, for diagnostics ("vec3", "ivec2", "mat2x3", "float",
  // "struct Foo"). Inline so the header stays free-standing (Step 1).
  std::string toString() const {
    switch (kind_) {
      case Kind::Void:   return "void";
      case Kind::Bool:   return "bool";
      case Kind::Int:    return "int";
      case Kind::Uint:   return "uint";
      case Kind::Float:  return "float";
      case Kind::Double: return "double";
      case Kind::Vector: {
        const char* prefix = elem_->isInt()  ? "ivec"
                             : elem_->isUint() ? "uvec"
                                               : "vec";  // float vectors
        return prefix + std::to_string(a_);
      }
      case Kind::Matrix:
        return a_ == b_ ? "mat" + std::to_string(a_)
                        : "mat" + std::to_string(a_) + "x" + std::to_string(b_);
      case Kind::Array:
        return elem_->toString() +
               (arraySize_ == 0 ? "[]" : "[" + std::to_string(arraySize_) + "]");
      case Kind::Sampler:
        switch (sampler_) {
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) \
          case SamplerKind::Kind: return Spelling;
#include "builtin_types.def"
        }
        return "sampler";
      case Kind::Struct: return "struct " + name_.str();
    }
    return "<type>";
  }

 private:
  friend class ::ASTContext;  // the sole factory — keeps types uniqued

  // Disambiguates the array constructor from the vector one (both take a
  // `const Type*` + a count).
  enum class ArrayCtor { Tag };

  explicit Type(Kind k) : kind_(k) {}  // scalar/void
  Type(const Type* elem, unsigned n)   // vector
      : kind_(Kind::Vector), a_(static_cast<uint8_t>(n)), elem_(elem) {
    assert(elem && (elem->isInt() || elem->isUint() || elem->isFloat()) &&
           "vector element must be int/uint/float (no bvec/dvec)");
    assert(n >= 2 && n <= 4 && "vector size out of range");
  }
  Type(unsigned cols, unsigned rows)  // matrix
      : kind_(Kind::Matrix),
        a_(static_cast<uint8_t>(cols)),
        b_(static_cast<uint8_t>(rows)) {
    assert(cols >= 2 && cols <= 4 && rows >= 2 && rows <= 4 &&
           "matrix dimensions out of range");
  }
  Type(const Type* elem, uint32_t size, ArrayCtor)  // array (size 0 = unsized)
      : kind_(Kind::Array), elem_(elem), arraySize_(size) {
    assert(elem && !elem->isVoid() && "array element must be a value type");
  }
  explicit Type(SamplerKind s) : kind_(Kind::Sampler), sampler_(s) {}  // sampler
  explicit Type(llvm::StringRef name)                                 // struct
      : kind_(Kind::Struct), name_(name) {}

  Kind kind_;
  SamplerKind sampler_{};
  uint8_t a_ = 0;  // vector: component count;  matrix: column count
  uint8_t b_ = 0;  // matrix: row count
  uint32_t arraySize_ = 0;      // array length (0 ⇒ runtime-sized)
  const Type* elem_ = nullptr;  // element type of a vector or array
  llvm::StringRef name_;        // struct name (interned in ASTContext)
  // Mutable so a struct type can be completed once after creation; see
  // setStructDecl(). nullptr ⇒ incomplete (forward-declared) struct.
  mutable const ::StructDeclExprAST* decl_ = nullptr;
};

// Number of SamplerKinds, counted from the .def so the ASTContext uniquing array
// (samplerTys_) self-resizes when a sampler is added — no hand-kept constant.
inline constexpr unsigned kNumSamplerKinds = 0
#define BTYPE_SAMPLER(Tok, Spelling, Kind, Dim, Arrayed, IsImage) + 1
#include "builtin_types.def"
    ;

}  // namespace glsl

#endif  // AST_TYPE_H
