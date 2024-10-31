#pragma once

#include <boost/variant.hpp>

namespace swift::runtime {
namespace detail {

template <typename ReturnT, typename Lambda> struct VariantVisitor : boost::static_visitor<ReturnT>,
                                                                     Lambda {
    explicit VariantVisitor(Lambda&& lambda) : Lambda(std::move(lambda)) {}

    using Lambda::operator();
};

}  // namespace detail

template <typename ReturnT, typename Variant, typename Lambda>
inline ReturnT VisitVariant(Variant&& variant, Lambda&& lambda) {
    return boost::apply_visitor(detail::VariantVisitor<ReturnT, Lambda>(std::forward<Lambda>(lambda)),
                                variant);
}

}  // namespace swift::runtime
