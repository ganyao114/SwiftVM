#pragma once

#include "runtime/compile/passes/passes.h"

namespace swift::runtime::test {

// Keep the workspace alive through the borrowed rewrite context.
class RewriteFixture final {
public:
    explicit RewriteFixture(ir::Block* block, bool eliminate_dead = true,
                            ir::HIRFunction* owner = nullptr)
            : rewrite(block, workspace.Get(), owner, eliminate_dead) {}
    explicit RewriteFixture(ir::HIRFunction* function, bool eliminate_dead = true)
            : rewrite(function, workspace.Get(), eliminate_dead) {}

    [[nodiscard]] compile::RewriteContext& Get() { return rewrite; }

private:
    compile::CompileWorkspaceScope workspace;
    compile::RewriteContext rewrite;
};

}  // namespace swift::runtime::test
