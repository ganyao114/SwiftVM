//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include <type_traits>
#include <utility>

#include "boost/variant.hpp"
#include "runtime/common/variant_util.h"
#include "runtime/ir/args.h"
#include "runtime/ir/location.h"

namespace swift::runtime::ir {

namespace terminal {

struct Invalid {};
struct ReturnToDispatch {};
struct ReturnToHost {};

struct LinkBlock {
    explicit LinkBlock(const Location& next_)
            : next(next_) {}
    Location next;
};

struct LinkBlockFast {
    explicit LinkBlockFast(const Location& next_)
            : next(next_) {}
    Location next;
};

struct ExternalLinkBlock {
    explicit ExternalLinkBlock(const Location& next_)
            : next(next_) {}
    Location next;
};

struct PopRSBHint {};

struct If;
struct Switch;
struct CheckHalt;
struct Condition;

using Terminal = boost::variant<
        Invalid,
        ReturnToDispatch,
        ReturnToHost,
        LinkBlock,
        LinkBlockFast,
        ExternalLinkBlock,
        PopRSBHint,
        boost::recursive_wrapper<If>,
        boost::recursive_wrapper<Condition>,
        boost::recursive_wrapper<Switch>,
        boost::recursive_wrapper<CheckHalt>>;

struct If {
    If(BOOL cond, Terminal then_, Terminal else_)
            : cond(cond), then_(std::move(then_)), else_(std::move(else_)) {
        cond.Use();
    }
    BOOL cond;
    Terminal then_;
    Terminal else_;
};

struct Condition {
    Condition(Cond cond, Terminal then_, Terminal else_)
            : cond(cond), then_(std::move(then_)), else_(std::move(else_)) {}
    Cond cond;
    Terminal then_;
    Terminal else_;
};

struct Switch {
    struct Case {
        Imm case_value;
        Terminal then;
    };

    Switch(Value value, const Vector<Case> &cases)
            : value(value), cases(cases) {
        value.Use();
    }
    Value value;
    Vector<Case> cases;
};

struct CheckHalt {
    explicit CheckHalt(Terminal else_)
            : else_(std::move(else_)) {}
    Terminal else_;
};

}

using terminal::Terminal;

namespace terminal::detail {

template<bool check, class ValueVisitor, class NodePredicate>
bool VisitValues(const Terminal& terminal, ValueVisitor& visit, NodePredicate& accept) {
    return VisitVariant<bool>(terminal, [&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (check) {
            if constexpr (std::is_same_v<T, If>) {
                if (!node.cond.HasValidDataView() || !IsScalarIntegerType(node.cond.Type()))
                    return false;
            } else if constexpr (std::is_same_v<T, Condition>) {
                if (!IsValidCondition(node.cond)) return false;
            } else if constexpr (std::is_same_v<T, Switch>) {
                if (!node.value.HasValidDataView() || !IsScalarIntegerType(node.value.Type()))
                    return false;
            }
        }
        if (!accept(node)) return false;
        if constexpr (std::is_same_v<T, If>) {
            visit(node.cond);
            return VisitValues<check>(node.then_, visit, accept) &&
                   VisitValues<check>(node.else_, visit, accept);
        } else if constexpr (std::is_same_v<T, Condition>) {
            return VisitValues<check>(node.then_, visit, accept) &&
                   VisitValues<check>(node.else_, visit, accept);
        } else if constexpr (std::is_same_v<T, Switch>) {
            visit(node.value);
            for (const auto& item : node.cases) {
                if constexpr (check)
                    if (!IsScalarIntegerType(item.case_value.GetType())) return false;
                if (!VisitValues<check>(item.then, visit, accept)) return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, CheckHalt>) {
            return VisitValues<check>(node.else_, visit, accept);
        } else {
            // Invalid is the ordinary flat-block dispatcher exit. Individual
            // entry contracts can require an explicit terminal via accept.
            return true;
        }
    });
}

}  // namespace terminal::detail

// Check every nested node while visiting its raw value occurrences in order.
// The value callback never receives an invalid data view. Entry-specific
// ownership or node restrictions can join this walk without a second scan.
template<class ValueVisitor, class NodePredicate>
bool CheckTerminal(const Terminal& input, ValueVisitor&& visit, NodePredicate&& accept) {
    return terminal::detail::VisitValues<true>(input, visit, accept);
}

template<class ValueVisitor>
bool CheckTerminal(const Terminal& input, ValueVisitor&& visit) {
    const auto accept = [](const auto&) { return true; };
    return CheckTerminal(input, std::forward<ValueVisitor>(visit), accept);
}

// Facts that only need raw uses preserve duplicates and each use's view.
// Validation belongs to the admission/SSA lifecycle, not every use query.
template<class ValueVisitor>
void ForEachTerminalValue(const Terminal& input, ValueVisitor&& visit) {
    const auto accept = [](const auto&) { return true; };
    (void)terminal::detail::VisitValues<false>(input, visit, accept);
}

}  // namespace swift::runtime::ir
