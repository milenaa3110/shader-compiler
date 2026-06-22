// ast/ast_context.cpp — out-of-line destructor that walks the node list.
//
// Kept out of the header so the .h can fwd-declare ExprAST instead of
// including the whole ast.h (~700 lines pulled into every TU). Here, we
// have the full type and the virtual destructor is callable.

#include "ast_context.h"

#include <cassert>

#include "ast.h"

// This TU does not pull bare `llvm::Type` into scope, so aliasing the semantic
// type keeps the factory bodies readable without re-introducing the collision.
using glsl::Type;

ASTContext::ASTContext() : saver_(bump_) {
    // One-shot invariant check: two intern calls on equal content return
    // pointer-equal results. Catches a refactor that swaps in the wrong
    // allocator or accidentally returns the input view. Single assert
    // per ASTContext construction; effectively free.
    llvm::StringRef a = intern("vec2");
    llvm::StringRef b = intern(std::string("vec2"));
    assert(a.data() == b.data() &&
           "ASTContext::intern must return pointer-equal results "
           "for equal content (StringSaver invariant)");
    (void)a;
    (void)b;

    // Same invariant for the type tables: a type requested two different ways
    // must be the one canonical instance, so == is a pointer compare.
    assert(getVec2Ty() == getVectorTy(getFloatTy(), 2) &&
           "ASTContext type uniquing broken: vec2 has two instances");
    assert(getIntTy() == getIntTy() &&
           "ASTContext type uniquing broken: scalar not memoised");
    assert(getArrayTy(getFloatTy(), 4) == getArrayTy(getFloatTy(), 4) &&
           getArrayTy(getFloatTy(), 4) != getUnsizedArrayTy(getFloatTy()) &&
           "ASTContext type uniquing broken: array key must include length");
}

const Type* ASTContext::newType(const Type& proto) {
    void* mem = bump_.Allocate(sizeof(Type), alignof(Type));
    return new (mem) Type(proto);  // trivially destructible — no dtor registry
}

// ── Scalar singletons (indexed by kind) ─────────────────────────────────────
const Type* ASTContext::getVoidTy() {
    auto& slot = scalarTys_[static_cast<int>(Type::Kind::Void)];
    if (!slot) slot = newType(Type(Type::Kind::Void));
    return slot;
}
const Type* ASTContext::getBoolTy() {
    auto& slot = scalarTys_[static_cast<int>(Type::Kind::Bool)];
    if (!slot) slot = newType(Type(Type::Kind::Bool));
    return slot;
}
const Type* ASTContext::getIntTy() {
    auto& slot = scalarTys_[static_cast<int>(Type::Kind::Int)];
    if (!slot) slot = newType(Type(Type::Kind::Int));
    return slot;
}
const Type* ASTContext::getUintTy() {
    auto& slot = scalarTys_[static_cast<int>(Type::Kind::Uint)];
    if (!slot) slot = newType(Type(Type::Kind::Uint));
    return slot;
}
const Type* ASTContext::getFloatTy() {
    auto& slot = scalarTys_[static_cast<int>(Type::Kind::Float)];
    if (!slot) slot = newType(Type(Type::Kind::Float));
    return slot;
}
const Type* ASTContext::getDoubleTy() {
    auto& slot = scalarTys_[static_cast<int>(Type::Kind::Double)];
    if (!slot) slot = newType(Type(Type::Kind::Double));
    return slot;
}

// ── Composite types (keyed by their parameters) ─────────────────────────────
const Type* ASTContext::getVectorTy(const Type* elem, unsigned n) {
    assert(elem && elem->isScalar() && "vector element must be a scalar type");
    assert(n >= 2 && n <= 4 && "vector size out of range");
    auto key = std::make_pair(elem, n);
    auto it = vectorTys_.find(key);
    if (it != vectorTys_.end()) return it->second;
    const Type* t = newType(Type(elem, n));
    vectorTys_[key] = t;
    return t;
}
const Type* ASTContext::getMatrixTy(unsigned cols, unsigned rows) {
    assert(cols >= 2 && cols <= 4 && rows >= 2 && rows <= 4 &&
           "matrix dimensions out of range");
    auto key = std::make_pair(cols, rows);
    auto it = matrixTys_.find(key);
    if (it != matrixTys_.end()) return it->second;
    const Type* t = newType(Type(cols, rows));
    matrixTys_[key] = t;
    return t;
}
const Type* ASTContext::internArrayTy(const Type* elem, unsigned size) {
    assert(elem && !elem->isVoid() && "array element must be a value type");
    auto key = std::make_pair(elem, size);
    auto it = arrayTys_.find(key);
    if (it != arrayTys_.end()) return it->second;
    const Type* t = newType(Type(elem, size, Type::ArrayCtor::Tag));
    arrayTys_[key] = t;
    return t;
}
const Type* ASTContext::getArrayTy(const Type* elem, unsigned size) {
    assert(size >= 1 &&
           "fixed array size must be >= 1; use getUnsizedArrayTy for the "
           "runtime-sized [] of an SSBO member");
    return internArrayTy(elem, size);
}
const Type* ASTContext::getUnsizedArrayTy(const Type* elem) {
    return internArrayTy(elem, /*size=*/0);
}
const Type* ASTContext::getSamplerTy(Type::SamplerKind kind) {
    auto& slot = samplerTys_[static_cast<int>(kind)];
    if (!slot) slot = newType(Type(kind));
    return slot;
}
const Type* ASTContext::getStructTy(llvm::StringRef name) {
    llvm::StringRef key = intern(name);  // stable storage + pointer identity
    auto it = structTys_.find(key);
    if (it != structTys_.end()) return it->second;
    const Type* t = newType(Type(key));
    structTys_[key] = t;
    return t;
}

ASTContext::~ASTContext() {
    // Walk in reverse construction order — children were registered
    // before their parents (the parser builds bottom-up), so this
    // destructs leaves first. Not strictly necessary (the BumpPtrAllocator
    // releases all memory after this loop regardless), but cheaper for
    // any subclass destructor that touches its own members vs children.
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        (*it)->~ExprAST();
    }
    // bump_ destructor releases every byte allocated through it,
    // including the saver_'s interned strings.
}
