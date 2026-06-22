// ast/ast_context.cpp — out-of-line destructor that walks the node list.
//
// Kept out of the header so the .h can fwd-declare ExprAST instead of
// including the whole ast.h (~700 lines pulled into every TU). Here, we
// have the full type and the virtual destructor is callable.

#include "ast_context.h"

#include <cassert>

#include "ast.h"

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
