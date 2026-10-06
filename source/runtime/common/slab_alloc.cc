#include "slab_alloc.h"

namespace swift::runtime {

void* SlabAllocator::Allocate() {
    std::lock_guard guard(mutex);
    Node* ret = head.load();
    if (ret) {
        head.store(ret->next);
    }
    return ret;
}

void SlabAllocator::Free(void* obj) {
    std::lock_guard guard(mutex);
    Node* node = static_cast<Node*>(obj);

    node->next = head.load();
    head.store(node);
}

}  // namespace swift::runtime
