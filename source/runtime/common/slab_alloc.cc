#include "slab_alloc.h"

namespace swift::runtime {

void* SlabAllocator::Allocate() {
    Node* ret = head.load();

    do {
        if (ret == nullptr) {
            break;
        }
    } while (!head.compare_exchange_weak(ret, ret->next));

    return ret;
}

void SlabAllocator::Free(void* obj) {
    Node* node = static_cast<Node*>(obj);

    Node* cur_head = head.load();
    do {
        node->next = cur_head;
    } while (!head.compare_exchange_weak(cur_head, node));
}

}  // namespace swift::runtime
