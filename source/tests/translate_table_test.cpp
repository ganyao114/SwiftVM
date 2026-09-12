#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/translate_table.h"

using swift::runtime::TranslateEntry;
using swift::runtime::TranslateTable;
using swift::runtime::TranslateTableHash;

TEST_CASE("translation tables reject overflowing storage sizes", "[cache-memory]") {
    constexpr auto bits = std::numeric_limits<std::size_t>::digits;
    REQUIRE_THROWS_AS(TranslateTable(bits), std::bad_alloc);
    REQUIRE_THROWS_AS(TranslateTable(bits - 1), std::bad_alloc);
    constexpr auto entry_bits = std::countr_zero(sizeof(TranslateEntry));
    REQUIRE_THROWS_AS(TranslateTable(bits - entry_bits - 1, TranslateTableHash::Direct),
                      std::bad_alloc);
}

TEST_CASE("translation tables keep unused pages uncommitted", "[cache-memory]") {
    for (auto mode : {TranslateTableHash::Folded, TranslateTableHash::Direct}) {
        TranslateTable table{12, mode};
        const auto page = static_cast<std::size_t>(getpagesize());
        const auto address = reinterpret_cast<std::uintptr_t>(table.Data());
        const auto start = (address + page - 1) & ~(page - 1);
        constexpr auto bytes = (std::size_t{1} << 12) * sizeof(TranslateEntry);
        const auto count = (address + bytes - start) / page;
        REQUIRE(count > 0);
#if defined(__APPLE__)
        std::vector<char> residency(count);
#else
        std::vector<unsigned char> residency(count);
#endif
        REQUIRE(mincore(reinterpret_cast<void*>(start), count * page, residency.data()) == 0);
        REQUIRE(std::none_of(residency.begin(), residency.end(), [](auto state) { return state & 1; }));
        REQUIRE(table.Lookup(0x1234) == 0);
        REQUIRE(table.Data()[4095].key == 0);
        REQUIRE(table.Data()[4095].value == 0);
    }
}

TEST_CASE("translation tables preserve collisions and alignment across reset", "[cache-memory]") {
    for (auto mode : {TranslateTableHash::Folded, TranslateTableHash::Direct}) {
        TranslateTable table{8, mode};
        constexpr auto first = std::size_t{4};
        const auto second = mode == TranslateTableHash::Direct ? first + 256 : 4 * 256;
        constexpr auto invalid_base = std::size_t{0x10000000};
        for (unsigned pass = 0; pass < 2; ++pass) {
            REQUIRE(reinterpret_cast<std::uintptr_t>(table.Data()) % table.DataAlignment() == 0);
            REQUIRE(table.Data()[265].key == std::size_t(-1));
            REQUIRE(table.Hash(first) == table.Hash(second));
            table.SetIndexedInvalidValue(reinterpret_cast<void*>(invalid_base));
            REQUIRE(table.Put(first, 0xabc0));
            REQUIRE(table.Put(second, 0xdef0));
            REQUIRE(table.Lookup(first) == 0xabc0);
            REQUIRE(table.Lookup(second) == 0xdef0);
            REQUIRE(table.Zero(first));
            REQUIRE(table.Lookup(first) == invalid_base + table.Hash(first) * sizeof(TranslateEntry));
            REQUIRE(table.Lookup(second) == 0xdef0);
            auto* const published_base = table.Data();
            table.Clear();
            REQUIRE(table.Data() == published_base);
            REQUIRE(table.Lookup(first) == 0);
            REQUIRE(table.Lookup(second) == 0);
            table.Reset();
        }
    }
}
