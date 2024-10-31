//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <list>
#include <set>
#include <vector>
#include <map>
#include <boost/container/small_vector.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include "base/common_funcs.h"
#include "base/types.h"
#include "runtime/externals/utils/util_intrusive_list.hpp"
#include "runtime/externals/utils/util_intrusive_red_black_tree.hpp"

namespace swift::runtime {

struct NonTriviallyDummy {
    NonTriviallyDummy() noexcept = default;
};

template <auto NodeMember> using IntrusiveList =
        typename ams::util::IntrusiveListMemberTraits<NodeMember>::ListType;
using IntrusiveListNode = ams::util::IntrusiveListNode;
template <class Derived> using IntrusiveListBaseNode = ams::util::IntrusiveListBaseNode<Derived>;
template <class Derived> using IntrusiveListBaseTraits =
        ams::util::IntrusiveListBaseTraits<Derived>;

using SingleIntrusiveListNode = boost::intrusive::slist_member_hook<>;
template <auto Member, typename Parent = ams::util::impl::GetParentType<Member>>
using SingleIntrusiveList = typename boost::intrusive::
        slist<Parent, boost::intrusive::member_hook<Parent, SingleIntrusiveListNode, Member>>;

// using IntrusiveMapNode = boost::intrusive::set_member_hook<>;
// template<auto Member, typename Parent = ams::util::impl::GetParentType<Member>>
// using IntrusiveMap = typename boost::intrusive::rbtree<Parent,
// boost::intrusive::member_hook<Parent, IntrusiveMapNode, Member>,
// boost::intrusive::compare<std::less<Parent>>>;

template<auto MemberPtr>
using GetParentType = ams::util::impl::GetParentType<MemberPtr>;

using IntrusiveMapNode = ams::util::IntrusiveRedBlackTreeNode;
template <auto Member, typename Comparator = GetParentType<Member>>
using IntrusiveMap = typename ams::util::IntrusiveRedBlackTreeMemberTraitsDeferredAssert<
        Member>::template TreeType<Comparator>;

using BitVector = boost::dynamic_bitset<>;

template <typename T> using Vector = typename std::vector<T>;

template <typename T> using List = typename std::list<T>;

template <typename T> using Set = typename std::set<T>;

template <typename K, typename V> using Map = typename std::map<K, V>;

template <typename T, size_t N> using StackVector = typename boost::container::small_vector<T, N>;

template <typename T> using IntrusiveRefCounter =
        boost::intrusive_ref_counter<T>;

template <typename T> using IntrusivePtr = boost::intrusive_ptr<T>;

template <typename DerivedT> inline void IntrusivePtrAddRef(
        const IntrusiveRefCounter<DerivedT>* p) BOOST_SP_NOEXCEPT {
    intrusive_ptr_add_ref(p);
}

template <typename DerivedT> inline void IntrusivePtrRelease(
        const IntrusiveRefCounter<DerivedT>* p) BOOST_SP_NOEXCEPT {
    intrusive_ptr_release(p);
}

}  // namespace swift::runtime
