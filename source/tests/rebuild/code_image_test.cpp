#include "base/error.h"
#include "base/hash.h"
#include "runtime/arch/arm32/location.h"
#include "runtime/execution/code_image.h"
#include "runtime/execution/disk_cache.h"
#include "runtime/platform/linux/artifact_file.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <catch2/catch_test_macros.hpp>
#include <unistd.h>

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::execution;

std::span<const std::uint8_t> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

CodeImage Example() {
    CodeImage image;
    image.key.host_isa = 0xa64;
    image.key.build_id = HashBytes(Bytes("build"));
    image.key.guest_id = HashBytes(Bytes("guest"));
    image.key.configuration = HashBytes(Bytes("configuration"));
    CodeUnit unit;
    unit.pc = 0x401000;
    unit.scope = ir::Scope::Function;
    unit.code.relocatable = true;
    unit.code.words = {0xd503201f, 0xd503201f, 0x12345678, 0x87654321};
    unit.code.frame_bytes = 32;
    unit.code.spill_slots = 2;
    unit.code.faults.push_back({0, 4, unit.pc, 0});
    unit.code.links.push_back({backend::LinkKind::Static, 8, 0, 4, 0x402000});
    unit.code.entries.push_back({unit.pc + 2, 4});
    unit.code.relocations.push_back(
            {8, backend::RelocationKind::Absolute64, backend::HostSymbol::Dispatcher, -16});
    unit.sources.push_back({{unit.pc, 7}, HashBytes(Bytes("source1")), 0});
    unit.sources.push_back({{0x402000, 7}, HashBytes(Bytes("source2")), 1});
    image.units.push_back(unit);
    unit.fp_mode = 3;
    unit.code.words[0] = 0xd503203f;
    image.units.push_back(std::move(unit));
    return image;
}

struct Directory {
    std::string path;

    Directory() {
        const auto* scratch = std::getenv("SVM_TEST_SCRATCH");
        path = (scratch ? std::string{scratch} : std::filesystem::current_path().string()) +
               "/svm-cache-test-XXXXXX";
        if (!mkdtemp(path.data())) ThrowSystemError("create test cache directory", errno);
    }

    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
}  // namespace

TEST_CASE("SHA256 agrees with known vectors across padding and update boundaries",
          "[compile][persistence]") {
    CHECK(HexDigest(HashBytes({})) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(HexDigest(HashBytes(Bytes("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    constexpr std::string_view long_input =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256 streamed;
    for (const auto c : long_input) streamed.Update({reinterpret_cast<const std::uint8_t*>(&c), 1});
    CHECK(HexDigest(streamed.Finish()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string block(1000, 'a');
    Sha256 million;
    for (unsigned i = 0; i < 1000; ++i) million.Update(Bytes(block));
    CHECK(HexDigest(million.Finish()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("one native image preserves source dependencies and all execution metadata",
          "[compile][persistence]") {
    const auto original = Example();
    const auto bytes = EncodeCodeImage(original);
    CodeImage decoded;
    std::string error;
    REQUIRE(DecodeCodeImage(bytes, decoded, error));
    CHECK(decoded.key == original.key);
    REQUIRE(decoded.units.size() == 2);
    CHECK(decoded.units[0].scope == ir::Scope::Function);
    CHECK(decoded.units[1].fp_mode == 3);
    CHECK(decoded.units[0].code.faults[0].guest_pc == 0x401000);
    CHECK(decoded.units[0].code.links[0].target_pc == 0x402000);
    CHECK(decoded.units[0].code.relocations[0].addend == -16);
    CHECK(decoded.units[0].sources[1].mode == 1);
    CHECK(EncodeCodeImage(decoded) == bytes);
}

TEST_CASE("external AOT text is shared once and its failure stub is checksummed",
          "[compile][persistence]") {
    auto original = Example();
    original.units[0].code.words.resize(4096, 0xd503201f);
    const std::array<std::uint8_t, 8> tail{1, 2, 3};
    std::vector<std::uint8_t> text;
    const auto metadata = EncodeCodeMetadata(original, text, tail);
    CHECK(metadata.size() < 1024);
    CodeImage decoded;
    std::string error;
    REQUIRE(DecodeCodeMetadata(metadata, text, decoded, error));
    CHECK(decoded.units[0].code.words == original.units[0].code.words);
    text.back() ^= 1;
    CHECK_FALSE(DecodeCodeMetadata(metadata, text, decoded, error));
    CHECK(error.find("checksum") != std::string::npos);
}

TEST_CASE("bad images reject before replacing a caller's valid output", "[compile][persistence]") {
    auto image = Example();
    auto bytes = EncodeCodeImage(image);
    CodeImage output = image;
    std::string error;
    for (const auto length : {std::size_t{0}, std::size_t{55}, bytes.size() - 1}) {
        CHECK_FALSE(DecodeCodeImage(std::span{bytes}.first(length), output, error));
        CHECK(output.key == image.key);
    }
    bytes.back() ^= 1;
    CHECK_FALSE(DecodeCodeImage(bytes, output, error));
    CHECK(error.find("checksum") != std::string::npos);
    image.units[0].code.relocations[0].offset = 12;
    CHECK_THROWS_AS(EncodeCodeImage(image), RuntimeError);
}

TEST_CASE("disk cache reopens canonical code with FP domains and public entries",
          "[compile][persistence]") {
    Directory directory;
    const auto image = Example();
    {
        DiskCache writer{directory.path, image.key};
        CHECK_FALSE(writer.Load());
        for (const auto& unit : image.units) writer.Record(unit);
        REQUIRE(writer.Flush());
        CHECK_FALSE(writer.Dirty());
    }
    DiskCache reader{directory.path, image.key};
    REQUIRE(reader.Load());
    REQUIRE(reader.Find(0x401002, 3));
    CHECK(reader.Find(0x401002, 3)->pc == 0x401000);
    CHECK_FALSE(reader.Find(0x401000, 1));
    reader.Disable();
    CHECK_FALSE(reader.Find(0x401000, 0));
    CHECK_FALSE(reader.Flush());
    auto other = image.key;
    other.configuration[0] ^= 1;
    CHECK_FALSE(DiskCache{directory.path, other}.Load());
}

TEST_CASE("corrupt optional cache is replaceable and never a partial hit",
          "[compile][persistence]") {
    Directory directory;
    const auto image = Example();
    const auto path = directory.path + "/" + HexDigest(CodeImageKeyDigest(image.key)) + ".svmcache";
    platform::WriteArtifactFile(path, std::array<std::uint8_t, 4>{1, 2, 3, 4});
    DiskCache cache{directory.path, image.key};
    CHECK_FALSE(cache.Load());
    CHECK_FALSE(cache.Find(0x401000, 0));
    cache.Record(image.units[0]);
    REQUIRE(cache.Flush());
    DiskCache fresh{directory.path, image.key};
    REQUIRE(fresh.Load());
    CHECK(fresh.Find(0x401000, 0));
    CHECK(std::distance(std::filesystem::directory_iterator{directory.path},
                        std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("ARM32 persistence keeps translation context while sources use physical addresses",
          "[compile][persistence][arm32]") {
    auto image = Example();
    image.key.guest_isa = GuestISA::Arm32;
    image.key.guest_profile = 0x701;
    const auto location = arm32::Location::Make(0x401000, true, 0x18, 1u << 22).Key();
    for (auto& unit : image.units) {
        unit.pc = location;
        unit.code.faults[0].guest_pc = location;
        unit.code.entries[0].pc = location + 2;
        unit.code.links[0].target_pc = arm32::Location{location}.WithPC(0x402000).Key();
    }
    CodeImage decoded;
    std::string error;
    REQUIRE(DecodeCodeImage(EncodeCodeImage(image), decoded, error));
    CHECK(decoded.key == image.key);
    CHECK(decoded.units[0].pc == location);
    CHECK(decoded.units[0].sources[0].span.pc == 0x401000);
    CHECK(decoded.units[0].code.faults[0].guest_pc == location);
}
