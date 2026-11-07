#include "aot/artifact.h"
#include "aot/guest_call.h"
#include "tests/support/aot_fixture.h"
#include "tests/support/check.h"

namespace {
namespace aot = swift::aot;
namespace execution = swift::runtime::execution;
namespace fixture = swift::tests::aot_fixture;

aot::Artifact Artifact() {
    aot::Artifact artifact;
    artifact.guest_elf = fixture::Guest();
    artifact.guest_path = "/source-is-deliberately-absent/aot-guest";
    artifact.code.key.host_isa = 0xa64;
    artifact.code.key.guest_id = swift::HashBytes(artifact.guest_elf);
    for (const auto pc : {fixture::Sum7, fixture::Resolver, fixture::Nine}) {
        execution::CodeUnit unit;
        unit.pc = pc;
        unit.code.relocatable = true;
        // Portable format fixtures only. Native execution has separate cases.
        unit.code.words = {0xd2800020, 0xd65f03c0};
        unit.sources.push_back(
                {{pc, 1},
                 swift::HashBytes(std::span{artifact.guest_elf}.subspan(pc - fixture::Base, 1)),
                 0});
        artifact.code.units.push_back(std::move(unit));
    }
    aot::ResolveSymbols(artifact);
    return artifact;
}

struct Mixed {
    std::uint64_t integer;
    double real;
};

struct Big {
    std::uint64_t a, b, c;
};
}  // namespace

TEST_CASE("AOT embeds a complete guest and preserves symbol semantics", "[compile][aot]") {
    const auto input = Artifact();
    const auto encoded = aot::EncodeArtifact(input);
    aot::Artifact restored;
    std::string error;
    REQUIRE(aot::DecodeArtifact(encoded, restored, error));
    CHECK(restored.guest_elf == input.guest_elf);
    CHECK(restored.guest_path == input.guest_path);
    CHECK(restored.code.units.size() == 3);
    const auto* function = aot::FindSymbol(restored, "sum7");
    const auto* alias = aot::FindSymbol(restored, "sum_alias");
    const auto* resolver = aot::FindSymbol(restored, "choose");
    const auto* object = aot::FindSymbol(restored, "global");
    const auto* missing = aot::FindSymbol(restored, "unsupported");
    REQUIRE((function && alias && resolver && object && missing));
    CHECK(function->Compiled());
    CHECK(alias->Compiled());
    CHECK(function->unit == alias->unit);
    CHECK(alias->size == 0);
    CHECK(resolver->type == 10);
    CHECK(resolver->Compiled());
    CHECK(resolver->guest_value == fixture::Resolver);
    CHECK(object->guest_value == fixture::Data);
    CHECK(!object->Compiled());
    CHECK(!missing->Compiled());
    const aot::elf::View container{encoded};
    const auto* text = container.Find(".svmaot.text");
    const auto* guest = container.Find(".svmaot.guest");
    const auto* symbols = container.Find(".symtab");
    REQUIRE((text && guest && symbols));
    bool checked_alias{}, checked_resolver{}, checked_stub{}, checked_object{};
    for (const auto& raw : container.Symbols(*symbols)) {
        const auto name = container.SymbolName(*symbols, raw);
        if (name == "sum_alias") {
            CHECK(raw.value == aot::CodeVirtualAddress);
            CHECK(raw.size == 8);
            checked_alias = true;
        } else if (name == "choose") {
            CHECK((raw.info & 15) == 10);
            CHECK(raw.value == aot::CodeVirtualAddress + 16);
            checked_resolver = true;
        } else if (name == "unsupported") {
            CHECK(raw.value == aot::CodeVirtualAddress + text->size - 8);
            checked_stub = true;
        } else if (name == "global") {
            CHECK(raw.value == fixture::Data);
            CHECK(raw.section == 2);
            checked_object = true;
        }
    }
    CHECK(checked_alias && checked_resolver && checked_stub && checked_object);
    // Original data sections borrow the embedded image, not another payload.
    CHECK(container.Find(".data")->offset == guest->offset + 0x2000);
}

TEST_CASE("AOT rejects damaged native guest metadata and rewritten symbols", "[compile][aot]") {
    const auto encoded = aot::EncodeArtifact(Artifact());
    const aot::elf::View container{encoded};
    for (const auto name : {".svmaot.guest", ".svmaot.text", ".svmaot.info"}) {
        auto damaged = encoded;
        const auto* section = container.Find(name);
        REQUIRE(section);
        damaged[section->offset + section->size - 1] ^= 1;
        aot::Artifact output;
        std::string error;
        CHECK(!aot::DecodeArtifact(damaged, output, error));
        CHECK(!error.empty());
        CHECK(output.code.units.empty());
    }
    auto damaged = encoded;
    const auto* symbols = container.Find(".symtab");
    REQUIRE(symbols);
    // Mutate the actual artifact table, retaining valid guest/native hashes.
    damaged[symbols->offset + 2 * sizeof(aot::elf::Symbol) + offsetof(aot::elf::Symbol, value)] ^=
            0x10;
    aot::Artifact output;
    std::string error;
    CHECK(!aot::DecodeArtifact(damaged, output, error));
    CHECK(error.find("symbol") != std::string::npos);
}

TEST_CASE("GuestFn keeps independent SysV register banks and whole aggregate spilling",
          "[compile][aot][abi]") {
    using namespace swift::guest_call;
    constexpr auto seven = MakeCallRecipe<std::uint64_t, int, int, int, int, int, int, int>();
    CHECK(seven.int_used == 6);
    CHECK(seven.args[6].on_stack);
    CHECK(seven.stack_area == 16);
    constexpr auto mixed = MakeCallRecipe<Mixed, double, Mixed>();
    CHECK(mixed.int_used == 1);
    CHECK(mixed.sse_used == 2);
    CHECK(mixed.ret_eb[0] == Eightbyte::Integer);
    CHECK(mixed.ret_eb[1] == Eightbyte::Sse);
    constexpr auto memory = MakeCallRecipe<Big, std::uint64_t>();
    CHECK(memory.ret_memory);
    CHECK(memory.args[0].reg[0] == 1);
    static_assert(!kIsMarshalable<const char*> && kIsMarshalable<GuestPtr<const char>>);
    static_assert(std::is_same_v<VarargPromote<float>, double>);
    const auto absent = GuestFn<std::uint64_t()>{}.Try();
    CHECK(!absent);
    CHECK(absent.status() == GuestCallStatus::SetupFailed);
}
