#include "tests/correctness/case.h"
#include "tests/correctness/catalog.h"
#include "tests/correctness/execute.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "base/error.h"

namespace test = swift::tests::correctness;

namespace {
const test::Form* PinnedForm() {
    static const auto form = [] {
        const auto* value = std::getenv("SVM_FUZZ_FORM");
        if (!value) return static_cast<const test::Form*>(nullptr);
        const auto* found = test::FindForm(value);
        if (!found) std::abort();
        return found;
    }();
    return form;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    test::Case c;
    try {
        c = test::Decode({data, size});
    } catch (const swift::RuntimeError&) {
        return 0;
    }
    if (!test::ValidateCase(c)) return 0;
    const auto* pinned = PinnedForm();
    if (pinned && c.form_id != pinned->id) return 0;
    const auto direct = test::ExecuteDirect(c);
    if (pinned && direct.outcome == test::Outcome::Rejected) std::abort();
    test::RunConfig config;
    const auto choice = c.id ^ c.seed ^ c.initial.gpr[0];
    config.backend = static_cast<test::Backend>(1 + choice % 4);
    config.scope = static_cast<test::Scope>((choice >> 3) & 1);
    config.target = static_cast<test::TargetMode>((choice >> 4) % 3);
    config.optimize = (choice >> 6) & 1;
    config.compile_only = true;
    const auto result = test::ExecuteCompile(c, config);
    if (pinned && result.outcome == test::Outcome::Rejected) std::abort();
    if (result.outcome == test::Outcome::Rejected &&
        (result.stage == "optimize" ||
         (result.stage == "compile" && result.error.starts_with("backend=1 "))))
        std::abort();
    return 0;
}

extern "C" std::size_t LLVMFuzzerCustomMutator(std::uint8_t* data,
                                               std::size_t size,
                                               std::size_t maximum,
                                               unsigned seed) {
    test::Case c;
    bool existing{};
    try {
        c = test::Decode({data, size});
        existing = test::ValidateCase(c);
    } catch (const swift::RuntimeError&) {
    }
    const auto forms = test::Catalog();
    const auto* pinned = PinnedForm();
    if (!existing || (pinned ? c.form_id != pinned->id : (seed & 7) == 0)) {
        const auto id = pinned ? pinned->id : forms[seed % forms.size()].id;
        c = test::Generate(id, seed, seed >> 3);
    } else {
        const auto count = c.isa == test::ISA::X86_64 ? 16 : c.isa == test::ISA::Arm64 ? 32 : 15;
        const auto r = (seed >> 3) % count;
        const auto bit = (seed >> 8) % (c.isa == test::ISA::Arm32 ? 32 : 64);
        c.initial.gpr[r] ^= std::uint64_t{1} << bit;
        if (seed & 2) {
            const auto v = (seed >> 14) % (c.isa == test::ISA::Arm64 ? 32 : 16);
            const auto byte = (seed >> 19) % (c.isa == test::ISA::X86_64 ? 32 : 16);
            c.initial.vectors[v][byte] ^= std::uint8_t(1u << ((seed >> 24) & 7));
        }
    }
    const auto encoded = test::Encode(c);
    if (encoded.size() > maximum) return size;
    std::memcpy(data, encoded.data(), encoded.size());
    return encoded.size();
}
