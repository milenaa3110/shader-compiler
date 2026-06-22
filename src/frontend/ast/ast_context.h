// ast/ast_context.h — per-compilation arena that owns AST nodes + their
// interned strings.
//
// Lifetime: created in main() before ParseProgram, destroyed after codegen.
// While alive, every AST node returned by Parser/Sema/etc. is allocated
// out of this context's BumpPtrAllocator — one big block, no per-node
// new/delete, contiguous in memory for better codegen-time cache hits.
//
// `create<T>(args...)` placement-news a T into the bump pool and registers
// it for destruction. On `~ASTContext` we walk the destruction list
// virtually so AST subclasses with non-trivial members (`std::string`,
// `std::vector`) clean themselves up before the bump pool releases its
// memory. The whole tree is freed in one shot.
//
// `intern(s)` saves a copy of `s` into the same bump pool and returns a
// stable `StringRef`. Two interned strings with equal content share
// storage — useful for hot identifier comparisons that want pointer
// equality.
//
// Current usage is conservative: AST node string fields are still
// `std::string`. Migrating them to `llvm::StringRef` (with interning
// at parse time) is a follow-up that pays off when:
//   * codegen does repeated `name == "..."` dispatch (sin/cos/etc.);
//   * a longer-running incremental / LSP frontend reads the same shader
//     multiple times and benefits from O(1) cross-reference compares.
// Both are out of scope here; `intern` is provided as infrastructure
// so that migration is a contained pass when it becomes worthwhile.

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

    // No copy / move: AST nodes hold raw pointers into this context's
    // bump pool, so a moved-from context would leave dangling references
    // everywhere.
    ASTContext(const ASTContext&)            = delete;
    ASTContext& operator=(const ASTContext&) = delete;

    // Allocate + construct a node in the bump pool. Returns a raw pointer;
    // ownership is the context's. Destructor is called on ~ASTContext.
    template <class T, class... Args>
    T* create(Args&&... args) {
        void* mem  = bump_.Allocate(sizeof(T), alignof(T));
        T*    node = new (mem) T(std::forward<Args>(args)...);
        // Up-cast to ExprAST so the destruction walk can dispatch
        // virtually. All AST nodes derive from ExprAST.
        nodes_.push_back(static_cast<ExprAST*>(node));
        return node;
    }

    // Save `s` into the bump pool and return a stable view of it. Two
    // calls with equal content share storage.
    llvm::StringRef intern(llvm::StringRef s) { return saver_.save(s); }

    // ── Canonical types ────────────────────────────────────────────────────
    // Each getter returns the one-and-only instance for that type, so equal
    // types compare pointer-equal. Types live in the bump pool for the life of
    // the context. (See type.h. Scoped `glsl::Type` to avoid colliding with
    // llvm::Type in the codegen TUs.)
    const glsl::Type* getVoidTy();
    const glsl::Type* getBoolTy();
    const glsl::Type* getIntTy();
    const glsl::Type* getUintTy();
    const glsl::Type* getFloatTy();
    const glsl::Type* getDoubleTy();

    // Vector of `elem` (a scalar type) with `n` components (2..4).
    const glsl::Type* getVectorTy(const glsl::Type* elem, unsigned n);
    // Matrix of `cols` × `rows` floats (2..4 each).
    const glsl::Type* getMatrixTy(unsigned cols, unsigned rows);
    // Array of `elem` with a fixed `size` (>= 1). The reserved length-0
    // "runtime-sized" encoding is NOT reachable here — an SSBO's `name[]` calls
    // getUnsizedArrayTy, so a stray `T[0]` can never alias an unsized array.
    const glsl::Type* getArrayTy(const glsl::Type* elem, unsigned size);
    // Runtime-sized array (an SSBO's trailing `name[]` member).
    const glsl::Type* getUnsizedArrayTy(const glsl::Type* elem);
    const glsl::Type* getSamplerTy(glsl::Type::SamplerKind kind);
    // Named user struct. Returns the one canonical instance for `name`, created
    // incomplete on first reference (so forward refs resolve to the same type)
    // and completed later via glsl::Type::setStructDecl from the analyzer.
    const glsl::Type* getStructTy(llvm::StringRef name);

    // Convenience wrappers for the common concrete vector/matrix types.
    const glsl::Type* getVec2Ty() { return getVectorTy(getFloatTy(), 2); }
    const glsl::Type* getVec3Ty() { return getVectorTy(getFloatTy(), 3); }
    const glsl::Type* getVec4Ty() { return getVectorTy(getFloatTy(), 4); }

    // Direct allocator access — for the rare caller that needs to put
    // raw bytes (not an AST node) into the same pool. Use sparingly.
    llvm::BumpPtrAllocator& allocator() { return bump_; }

   private:
    // Copy a freshly-built type prototype into the bump pool. Types are
    // trivially destructible, so (unlike AST nodes) they need no destruction
    // registration.
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
