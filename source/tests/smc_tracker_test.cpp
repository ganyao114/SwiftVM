#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/backend/translate_table.h"
#include "runtime/ir/block.h"

namespace {

using swift::runtime::Config;
using swift::runtime::TranslateTable;
using swift::runtime::backend::AddressSpace;
using swift::runtime::backend::IsEmpty;
using swift::runtime::backend::Module;
using swift::runtime::ir::Block;
using swift::runtime::ir::Location;

class SmcFixture {
public:
    explicit SmcFixture(std::size_t page_count)
            : page_size_(static_cast<std::size_t>(getpagesize()))
            , size_(page_size_ * page_count)
            , memory_(mmap(nullptr,
                           size_,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON,
                           -1,
                           0)) {
        if (memory_ == MAP_FAILED) {
            throw std::runtime_error("SMC test mmap failed");
        }
        Config config{
                .loc_start = 0,
                .loc_end = size_,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = swift::runtime::kArm64,
                .memory_base = memory_,
                .guest_addr_mask = size_ - 1,
        };
        space_ = std::make_unique<AddressSpace>(config);
        module_ = space_->GetDefaultModule();
    }

    ~SmcFixture() {
        // SmcTracker owns references to published nodes and may still inspect
        // their page records during AddressSpace teardown. Drop it before the
        // backing guest window.
        module_.reset();
        space_.reset();
        if (memory_ != MAP_FAILED) {
            munmap(memory_, size_);
        }
    }

    Block* Publish(std::uintptr_t guest, std::size_t size = 1) {
        auto* block = new Block(Location{guest});
        block->SetEndLocation(Location{guest + size});
        if (!module_->Push(block)) {
            throw std::runtime_error("SMC test duplicate block publication");
        }
        Tracker().RegisterNode(module_, block, guest, guest + size);
        return block;
    }

    [[nodiscard]] bool HasNode(std::uintptr_t guest) const {
        return !IsEmpty(module_->GetNode(Location{guest}));
    }

    [[nodiscard]] std::uint8_t* Host(std::uintptr_t guest) const {
        return static_cast<std::uint8_t*>(memory_) + guest;
    }

    [[nodiscard]] std::size_t PageSize() const { return page_size_; }
    [[nodiscard]] AddressSpace& Space() const { return *space_; }
    [[nodiscard]] swift::runtime::backend::SmcTracker& Tracker() const {
        return space_->GetSmcTracker();
    }

private:
    std::size_t page_size_{};
    std::size_t size_{};
    void* memory_{};
    std::unique_ptr<AddressSpace> space_{};
    std::shared_ptr<Module> module_{};
};

}  // namespace

TEST_CASE("SMC empty closes preserve the next real write fault", "[smc][dirty-hint]") {
    SmcFixture fixture{1};
    TranslateTable l1{8};
    constexpr std::uintptr_t kGuest = 0x100;
    constexpr std::size_t kEmptyCloses = 512;

    fixture.Publish(kGuest);
    REQUIRE(fixture.HasNode(kGuest));

    // With SVM_SMC_DIRTY_HINT=1 these are the lock-free fast-return case. With
    // the switch OFF they intentionally exercise the unchanged full scan.
    for (std::size_t i = 0; i < kEmptyCloses; ++i) {
        fixture.Tracker().CloseWriteWindow(fixture.Space(), l1);
    }
    REQUIRE(fixture.HasNode(kGuest));

    REQUIRE(fixture.Tracker().HandleWriteFault(
            fixture.Space(), l1, reinterpret_cast<std::uintptr_t>(fixture.Host(kGuest))));
    *fixture.Host(kGuest) = 0xC3;
    fixture.Tracker().CloseWriteWindow(fixture.Space(), l1);
    REQUIRE_FALSE(fixture.HasNode(kGuest));
    REQUIRE(*fixture.Host(kGuest) == 0xC3);

    // The empty-batch close leaves an RW tombstone specifically so a delayed
    // second protection fault is claimed without inventing another dirty
    // window. Empty closes must not disturb that claim_stale_fault state.
    for (std::size_t i = 0; i < kEmptyCloses; ++i) {
        fixture.Tracker().CloseWriteWindow(fixture.Space(), l1);
    }
    REQUIRE(fixture.Tracker().HandleWriteFault(
            fixture.Space(), l1, reinterpret_cast<std::uintptr_t>(fixture.Host(kGuest))));

    // Re-publication clears the tombstone and re-arms protection. This final
    // mutation catches the unsafe failure mode where a previous false hint
    // survives and causes the real dirty close to be skipped.
    fixture.Publish(kGuest);
    REQUIRE(fixture.Tracker().HandleWriteFault(
            fixture.Space(), l1, reinterpret_cast<std::uintptr_t>(fixture.Host(kGuest))));
    *fixture.Host(kGuest) = 0x90;
    fixture.Tracker().CloseWriteWindow(fixture.Space(), l1);
    REQUIRE_FALSE(fixture.HasNode(kGuest));
    REQUIRE(*fixture.Host(kGuest) == 0x90);
}

TEST_CASE("SMC concurrent write windows invalidate every runtime cache",
          "[smc][dirty-hint][thread]") {
    SmcFixture fixture{2};
    TranslateTable l1_a{8};
    TranslateTable l1_b{8};
    auto token_a = fixture.Tracker().RegisterRuntime(l1_a);
    auto token_b = fixture.Tracker().RegisterRuntime(l1_b);
    const std::uintptr_t guest_a = 0x100;
    const std::uintptr_t guest_b = fixture.PageSize() + 0x100;
    constexpr std::size_t kFakeA = 0x11110000;
    constexpr std::size_t kFakeB = 0x22220000;

    fixture.Publish(guest_a);
    fixture.Publish(guest_b);
    fixture.Space().PushCodeCache(Location{guest_a}, reinterpret_cast<void*>(kFakeA));
    fixture.Space().PushCodeCache(Location{guest_b}, reinterpret_cast<void*>(kFakeB));
    REQUIRE(l1_a.Put(guest_a, kFakeA));
    REQUIRE(l1_a.Put(guest_b, kFakeB));
    REQUIRE(l1_b.Put(guest_a, kFakeA));
    REQUIRE(l1_b.Put(guest_b, kFakeB));

    std::atomic_bool start{false};
    std::atomic<unsigned> faulted{0};
    std::atomic_bool fault_a{false};
    std::atomic_bool fault_b{false};
    auto worker = [&](std::uintptr_t guest,
                      TranslateTable& l1,
                      std::atomic_bool& result) {
        while (!start.load(std::memory_order_acquire)) {
        }
        result.store(fixture.Tracker().HandleWriteFault(
                             fixture.Space(),
                             l1,
                             reinterpret_cast<std::uintptr_t>(fixture.Host(guest))),
                     std::memory_order_release);
        faulted.fetch_add(1, std::memory_order_acq_rel);
        while (faulted.load(std::memory_order_acquire) != 2) {
        }
        // Both CPUs close their host-side boundary. invalidation_mutex_
        // serializes the detach/reclaim phase; either closer may consume both
        // dirty pages, and the other must then be a harmless empty close.
        fixture.Tracker().CloseWriteWindow(fixture.Space(), l1);
    };

    std::thread thread_a(worker, guest_a, std::ref(l1_a), std::ref(fault_a));
    std::thread thread_b(worker, guest_b, std::ref(l1_b), std::ref(fault_b));
    start.store(true, std::memory_order_release);
    thread_a.join();
    thread_b.join();

    REQUIRE(fault_a.load(std::memory_order_acquire));
    REQUIRE(fault_b.load(std::memory_order_acquire));
    REQUIRE_FALSE(fixture.HasNode(guest_a));
    REQUIRE_FALSE(fixture.HasNode(guest_b));
    REQUIRE(fixture.Space().GetCodeCacheTable().Lookup(guest_a) == 0);
    REQUIRE(fixture.Space().GetCodeCacheTable().Lookup(guest_b) == 0);
    REQUIRE(l1_a.Lookup(guest_a) == 0);
    REQUIRE(l1_a.Lookup(guest_b) == 0);
    REQUIRE(l1_b.Lookup(guest_a) == 0);
    REQUIRE(l1_b.Lookup(guest_b) == 0);

    fixture.Tracker().UnregisterRuntime(token_a);
    fixture.Tracker().UnregisterRuntime(token_b);
}

TEST_CASE("SMC range invalidation clears the L1 value used by inline indirect exits",
          "[smc][indirect-l1]") {
    SmcFixture fixture{1};
    TranslateTable l1{8};
    auto token = fixture.Tracker().RegisterRuntime(l1);
    constexpr std::uintptr_t kGuest = 0x180;
    constexpr std::size_t kFakeCode = 0x1234'5678;

    fixture.Publish(kGuest);
    fixture.Space().PushCodeCache(Location{kGuest},
                                  reinterpret_cast<void*>(kFakeCode));
    REQUIRE(l1.Put(kGuest, kFakeCode));
    REQUIRE(l1.Lookup(kGuest) == kFakeCode);
    REQUIRE(fixture.Space().GetCodeCacheTable().Lookup(kGuest) == kFakeCode);

    fixture.Tracker().InvalidateRange(
            fixture.Space(), &l1, kGuest, kGuest + 1);
    // Inline dispatch loads this same aligned value word and treats zero as a
    // miss. The target cannot be reclaimed while an old L1 pointer survives.
    REQUIRE(l1.Lookup(kGuest) == 0);
    REQUIRE(fixture.Space().GetCodeCacheTable().Lookup(kGuest) == 0);
    REQUIRE_FALSE(fixture.HasNode(kGuest));

    fixture.Tracker().UnregisterRuntime(token);
}
