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

    [[nodiscard]] T* Get(LocationDescriptor addr) {
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
        auto start_hash = Hash(std::max(start - max_node_size, addr_start));
        auto end_hash = Hash(end);

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
        auto hash = Hash(addr);
        auto map_ptr = hash_map[hash];
        if (!map_ptr) {
            return {};
        }
        if (auto itr = map_ptr->find(T{addr}); itr != map_ptr->end()) {
            map_ptr->erase(itr);
            return itr.operator->();
        }
        return {};
    }

    void Destroy() {
        for (auto &intrusive_map : intrusive_maps) {
            for (auto it = intrusive_map.begin(); it != intrusive_map.end();) {
                auto pre = it;
                it = intrusive_map.erase(it);
                delete pre.operator->();
            }
        }
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
