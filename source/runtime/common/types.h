//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <vector>
#include <list>
#include <set>
#include "base/types.h"
#include <boost/intrusive/slist.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/container/small_vector.hpp>
#include "runtime/externals/utils/util_intrusive_red_black_tree.hpp"
#include "runtime/externals/utils/util_intrusive_list.hpp"

namespace swift::runtime {

struct NonTriviallyDummy {
    NonTriviallyDummy() noexcept = default;
};

template <auto NodeMember>
using IntrusiveList = typename ams::util::IntrusiveListMemberTraits<NodeMember>::ListType;
using IntrusiveListNode = ams::util::IntrusiveListNode;
template<class Derived>
using IntrusiveListBaseNode = ams::util::IntrusiveListBaseNode<Derived>;
template<class Derived>
using IntrusiveListBaseTraits = ams::util::IntrusiveListBaseTraits<Derived>;

using SingleIntrusiveListNode = boost::intrusive::slist_member_hook<>;
template<auto Member, typename Parent = ams::util::impl::GetParentType<Member>>
using SingleIntrusiveList = typename boost::intrusive::slist<Parent, boost::intrusive::member_hook<Parent, SingleIntrusiveListNode, Member>>;

//using IntrusiveMapNode = boost::intrusive::set_member_hook<>;
//template<auto Member, typename Parent = ams::util::impl::GetParentType<Member>>
//using IntrusiveMap = typename boost::intrusive::rbtree<Parent, boost::intrusive::member_hook<Parent, IntrusiveMapNode, Member>, boost::intrusive::compare<std::less<Parent>>>;

using IntrusiveMapNode = ams::util::IntrusiveRedBlackTreeNode;
template<auto Member, typename Comparator = ams::util::impl::GetParentType<Member>>
using IntrusiveMap = typename ams::util::IntrusiveRedBlackTreeMemberTraitsDeferredAssert<Member>::template TreeType<Comparator>;

using BitVector = boost::dynamic_bitset<>;

template <typename T>
using Vector = typename std::vector<T>;

template <typename T>
using List = typename std::list<T>;

template <typename T>
using Set = typename std::set<T>;

template <typename T, size_t N>
using StackVector = typename boost::container::small_vector<T, N>;

}
