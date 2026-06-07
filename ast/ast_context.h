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

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/StringSaver.h>

#include <utility>
#include <vector>

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

    // Direct allocator access — for the rare caller that needs to put
    // raw bytes (not an AST node) into the same pool. Use sparingly.
    llvm::BumpPtrAllocator& allocator() { return bump_; }

   private:
    llvm::BumpPtrAllocator   bump_;
    llvm::UniqueStringSaver  saver_;  // de-duplicating: equal content ⇒ same ptr
    std::vector<ExprAST*>    nodes_;
};

#endif  // AST_CONTEXT_H
