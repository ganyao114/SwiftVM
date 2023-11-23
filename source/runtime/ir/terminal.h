//
// Created by 甘尧 on 2023/9/6.
//

#pragma once

#include "boost/variant.hpp"
#include "runtime/ir/args.h"
#include "runtime/ir/location.h"

namespace swift::runtime::ir {

namespace terminal {

struct Invalid {};
struct ReturnToDispatch {};

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

struct PopRSBHint {};

struct If;
struct Switch;
struct CheckHalt;

using Terminal = boost::variant<
        Invalid,
        ReturnToDispatch,
        LinkBlock,
        LinkBlockFast,
        PopRSBHint,
        boost::recursive_wrapper<If>,
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

}  // namespace swift::runtime::ir