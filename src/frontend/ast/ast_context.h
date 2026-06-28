// ast/ast_context.h — Compilation-lifetime arena allocator and type factory.
//
// All AST nodes and unique types are allocated within this context's continuous
// BumpPtrAllocator block to eliminate allocation overhead and improve cache locality.
//
// Nodes are placement-new'd via create<T>() and registered for tracking.
// On ~ASTContext(), virtual destructors are invoked for tracked nodes to clean up
// non-trivial members (e.g., vectors, strings) before the pool deletes everything in one shot.
//
// Provides string interning via UniqueStringSaver for fast pointer-based compares.
#ifndef AST_CONTEXT_H
#define AST_CONTEXT_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/StringSaver.h>

#include <utility>
#include <vector>

#include "type.h"

class ExprAST;  // fwd — full type only needed in the .cpp's destructor walk.

class ASTContext {
   public:
    ASTContext();   // out-of-line: runs an interning self-check
    ~ASTContext();

    // Non-copyable/non-movable: AST nodes hold raw pointers into this context's
    // arena; copying or moving the context would leave dangling references.
    ASTContext(const ASTContext&)            = delete;
    ASTContext& operator=(const ASTContext&) = delete;

    /// Placement-news an AST node into the arena and tracks it via its 
    /// base class (ExprAST) for late virtual destruction.
    template <class T, class... Args>
    T* create(Args&&... args) {
        void* mem  = bump_.Allocate(sizeof(T), alignof(T));
        T* node = new (mem) T(std::forward<Args>(args)...);
        nodes_.push_back(static_cast<ExprAST*>(node));
        return node;
    }

    /// Interns string 's' into the arena. Deduplicates identical content 
    /// to return a stable StringRef, enabling fast pointer-based equality.
    llvm::StringRef intern(llvm::StringRef s) { return saver_.save(s); }

    // Canonical types
    // Each function returns the unique, single instance allocated in the arena.
    // Enables fast O(1) type validation via pointer comparisons (a == b).
    const glsl::Type* getVoidTy();
    const glsl::Type* getBoolTy();
    const glsl::Type* getIntTy();
    const glsl::Type* getUintTy();
    const glsl::Type* getFloatTy();
    const glsl::Type* getDoubleTy();

    // Uniqued via context maps (DenseMap/StringMap) to ensure pointer equality.
    const glsl::Type* getVectorTy(const glsl::Type* elem, unsigned n);
    const glsl::Type* getMatrixTy(unsigned cols, unsigned rows);
    const glsl::Type* getArrayTy(const glsl::Type* elem, unsigned size);
    const glsl::Type* getUnsizedArrayTy(const glsl::Type* elem); // Dynamic SSBO trailing member
    const glsl::Type* getSamplerTy(glsl::Type::SamplerKind kind);
    const glsl::Type* getStructTy(llvm::StringRef name); // Lazily completed user-type

    // Convenience wrappers for the common concrete vector/matrix types.
    const glsl::Type* getVec2Ty() { return getVectorTy(getFloatTy(), 2); }
    const glsl::Type* getVec3Ty() { return getVectorTy(getFloatTy(), 3); }
    const glsl::Type* getVec4Ty() { return getVectorTy(getFloatTy(), 4); }

    /// Direct allocator access for custom raw byte allocations inside the pool. 
    /// Used to bypass heap overhead for temporary structures. Use sparingly.
    llvm::BumpPtrAllocator& allocator() { return bump_; }

   private:
    // Blits a type prototype directly into the arena. Types are trivially 
    // destructible and explicitly bypass destruction tracking for maximum performance.
    const glsl::Type* newType(const glsl::Type& proto);

    // Shared uniquing for getArrayTy / getUnsizedArrayTy (size 0 == unsized).
    const glsl::Type* internArrayTy(const glsl::Type* elem, unsigned size);

    llvm::BumpPtrAllocator   bump_;
    llvm::UniqueStringSaver  saver_;  // de-duplicating: equal content ⇒ same ptr
    std::vector<ExprAST*>    nodes_;

    // Type uniquing tables. Scalars and samplers are fixed small sets indexed
    // by kind; vectors/matrices/structs are keyed by their parameters.
    const glsl::Type* scalarTys_[6] = {};   // indexed by glsl::Type::Kind
    // Indexed by glsl::Type::SamplerKind; size tracks builtin_types.def.
    const glsl::Type* samplerTys_[glsl::kNumSamplerKinds] = {};
    llvm::DenseMap<std::pair<const glsl::Type*, unsigned>, const glsl::Type*>
        vectorTys_;
    llvm::DenseMap<std::pair<unsigned, unsigned>, const glsl::Type*> matrixTys_;
    // Keyed by (element, length); length 0 is the canonical unsized array.
    llvm::DenseMap<std::pair<const glsl::Type*, unsigned>, const glsl::Type*>
        arrayTys_;
    llvm::StringMap<const glsl::Type*> structTys_;
};

#endif  // AST_CONTEXT_H
