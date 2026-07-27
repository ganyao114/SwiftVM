//
// Created by 甘尧 on 2024/7/1.
//

#pragma once

#include <memory>
#include <optional>
#include <span>
#include "types.h"
#include "virtual_vector.h"

namespace swift::runtime {

template <auto Member, typename T = GetParentType<Member>> class AddressHashMap {
public:
    using Nodes = StackVector<T*, 32>;

    explicit AddressHashMap(LocationDescriptor start,
                            LocationDescriptor end,
                            LocationDescriptor hash_item_size = 1_MB)
            : addr_start(start), addr_end(end), hash_item_size(hash_item_size) {
        auto size = end - start;
        if (size <= hash_item_size * 512) {
            std_vector = std::make_unique<std::vector<IntrusiveMap<Member>*>>(
                    (size / hash_item_size) + 1);
            hash_map = {std_vector->data(), std_vector->size()};
        } else {
            virtual_vector = std::make_unique<VirtualVector<IntrusiveMap<Member>*>>(
                    (size / hash_item_size) + 1);
            hash_map = {virtual_vector->data(), virtual_vector->size()};
        }
    }

    [[nodiscard]] LocationDescriptor Hash(LocationDescriptor addr) const {
        return (addr - addr_start) / hash_item_size;
    }

    // Whether addr hashes into this map at all. hash_map is a raw span, so an
    // address outside [addr_start, addr_end) indexes out of bounds -- and the
    // addresses reaching these lookups are GUEST-CONTROLLED (a block location
    // is wherever the guest's last branch pointed). `jmp rax` with rax = -1
    // hashes to ~2^44 and read hash_map[2^44], faulting the host. Callers
    // treat "not in this map" as a miss, which is the right answer for an
    // address this map was never sized for.
    [[nodiscard]] bool InRange(LocationDescriptor addr) const {
        return addr >= addr_start && Hash(addr) < hash_map.size();
    }

    [[nodiscard]] T* Get(LocationDescriptor addr) {
        if (!InRange(addr)) {
            return {};
        }
        auto hash = Hash(addr);
        auto map_ptr = hash_map[hash];
        if (map_ptr) {
            if (auto itr = map_ptr->find(T{addr}); itr != map_ptr->end()) {
                return itr.operator->();
            }
        }
        return {};
    }

    Nodes GetRange(LocationDescriptor start, LocationDescriptor end) {
        constexpr auto max_node_size = 64_KB; // Maybe no safe?
        Nodes nodes{};
        if (end < addr_start || start > addr_end) {
            return nodes;
        }
        auto start_hash = Hash(std::max(start - max_node_size, addr_start));
        auto end_hash = std::min<LocationDescriptor>(Hash(end), hash_map.size() - 1);

        for (auto i = start_hash; i <= end_hash; i++) {
            if (auto map_ptr = hash_map[i]; map_ptr) {
                auto start_itr = map_ptr->lower_bound(T{start});
                if (start_itr != map_ptr->begin()) {
                    start_itr--;
                }
                auto end_itr = map_ptr->upper_bound(T{end});
                for (auto itr = start_itr; itr != end_itr; itr++) {
                    if (itr->Overlap(start, end)) {
                        nodes.push_back(itr.operator->());
                    }
                }
            }
        }
        return nodes;
    }

    template <bool check_exist = false>
    bool Put(LocationDescriptor addr, T* value) {
        if (!InRange(addr)) {
            return false;
        }
        auto hash = Hash(addr);
        auto map_ptr = hash_map[hash];
        if (!map_ptr) {
            map_ptr = CreateIntrusiveMap();
            hash_map[hash] = map_ptr;
        }
        if constexpr (check_exist) {
            if (auto itr = map_ptr->find(*value); itr != map_ptr->end()) {
                return false;
            }
        }
        map_ptr->insert(*value);
        return true;
    }

    T* Remove(LocationDescriptor addr) {
        if (!InRange(addr)) {
            return {};
        }
        auto hash = Hash(addr);
        auto map_ptr = hash_map[hash];
        if (!map_ptr) {
            return {};
        }
        if (auto itr = map_ptr->find(T{addr}); itr != map_ptr->end()) {
            map_ptr->erase(itr);
            auto ret = itr.operator->();
            if (map_ptr->empty()) {
                hash_map[hash] = nullptr;
                std::erase_if(intrusive_maps, [map_ptr](auto& item) { return &item == map_ptr; });
            }
            return ret;
        }
        return {};
    }

    // Empties the map, handing every node to `on_release` instead of deleting
    // it, and leaves the map usable (all buckets dropped).
    //
    // There is deliberately NO Destroy()-style variant that `delete`s the
    // nodes, and adding one back would be a use-after-free. A map entry here
    // is one *reference* to a node, not sole ownership of it: SmcTracker holds
    // its own strong ir::NodeRef on exactly the nodes this map contains (see
    // the TrackedNode comment in smc_tracker.h -- borrowing there was the UAF
    // fixed by 8d8ddba), and callers hold IntrusivePtr<Block>/<Function>
    // handles obtained from Module::GetNode*. Deleting from here frees memory
    // those owners are still reading. The owner-agnostic way to end this map's
    // participation is to drop its reference and let the refcount decide, so
    // that is the only operation offered.
    // `key_of(node)` must return the address the node was Put() under; it is
    // used to clear the node's bucket. Clearing bucket-by-bucket rather than
    // wiping `hash_map` wholesale is not micro-optimisation: hash_map is sized
    // from the module's address range, and the default module spans
    // [0, 1<<49) at 1 MB per bucket -- 537 million slots over a lazily
    // committed 4 GB VirtualVector. Writing nullptr across all of it faults in
    // the entire 4 GB and cost a flat ~0.18 s of system time on every process
    // exit (measured: `hello` went 0.01 s -> 0.19 s, and the cost was flat in
    // the number of nodes, which is what gave the wipe away). The populated
    // buckets are at most one per node.
    template <typename KeyFn, typename F>
    void ReleaseAll(KeyFn&& key_of, F&& on_release) {
        for (auto& intrusive_map : intrusive_maps) {
            for (auto it = intrusive_map.begin(); it != intrusive_map.end();) {
                // Unlink and read the key before releasing: on_release may run
                // the node's destructor, and both the intrusive hook and the
                // key live inside the node.
                auto* node = it.operator->();
                const auto addr = key_of(node);
                it = intrusive_map.erase(it);
                if (InRange(addr)) {
                    hash_map[Hash(addr)] = nullptr;
                }
                on_release(node);
            }
        }
        intrusive_maps.clear();
    }

private:
    IntrusiveMap<Member>* CreateIntrusiveMap() { return &intrusive_maps.emplace_back(); }

    const LocationDescriptor addr_start;
    const LocationDescriptor addr_end;
    const LocationDescriptor hash_item_size;
    std::list<IntrusiveMap<Member>> intrusive_maps{};
    std::unique_ptr<std::vector<IntrusiveMap<Member>*>> std_vector;
    std::unique_ptr<VirtualVector<IntrusiveMap<Member>*>> virtual_vector;
    std::span<IntrusiveMap<Member>*> hash_map;
};

}  // namespace swift::runtime
