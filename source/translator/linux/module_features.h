#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runtime/common/svm_config.h"

namespace swift::linux {

struct ModuleFeatureBindings {
    runtime::FeatureOverrides main{};
    std::vector<std::string> warnings{};
};

// Driver 私有语法解析；环境读取仍只发生在 main.cpp。当前只接受
// main:field=0|1[,field=0|1...]，坏条目仅记录 warning，不中止启动。
inline ModuleFeatureBindings ParseModuleFeatureBindings(std::string_view spec) {
    ModuleFeatureBindings result;
    if (spec.empty()) return result;

    const auto colon = spec.find(':');
    if (colon == std::string_view::npos) {
        result.warnings.emplace_back("missing role separator ':'");
        return result;
    }
    const auto role = spec.substr(0, colon);
    if (role != "main") {
        result.warnings.emplace_back("unknown role '" + std::string{role} + "'");
        return result;
    }

    auto entries = spec.substr(colon + 1);
    while (!entries.empty()) {
        const auto comma = entries.find(',');
        const auto entry = entries.substr(0, comma);
        entries = comma == std::string_view::npos
                ? std::string_view{}
                : entries.substr(comma + 1);

        const auto equal = entry.find('=');
        if (equal == std::string_view::npos || equal == 0 ||
            equal + 1 >= entry.size() || entry.find('=', equal + 1) != std::string_view::npos) {
            result.warnings.emplace_back("invalid entry '" + std::string{entry} + "'");
            continue;
        }
        const auto field_name = entry.substr(0, equal);
        const auto value = entry.substr(equal + 1);
        const auto field = runtime::FeatureIdFromName(field_name);
        if (!field) {
            result.warnings.emplace_back("unknown field '" + std::string{field_name} + "'");
            continue;
        }
        if (value != "0" && value != "1") {
            result.warnings.emplace_back("invalid value '" + std::string{value} +
                                         "' for field '" +
                                         std::string{field_name} + "'");
            continue;
        }
        result.main.Set(*field, value == "1");
    }
    return result;
}

}  // namespace swift::linux
