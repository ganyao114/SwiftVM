#include "test_support.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <span>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "translator/arm64/translator.h"

namespace swift::tests::loongarch64 {
namespace {

class CodeMemory final : public MemoryInterface {
public:
    std::span<u8> bytes;
    CodeMemory() {
        const auto page = sysconf(_SC_PAGESIZE);
        if (page <= 0) throw std::runtime_error("query host page size for guest code fixture");
        allocation_size = std::max(size_t(page), size_t{4096});
        allocation = mmap(nullptr, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED) throw std::runtime_error("allocate page-aligned guest code fixture");
        bytes = {static_cast<u8*>(allocation), 4096};
        std::memset(bytes.data(), 0, bytes.size());
    }
    ~CodeMemory() { munmap(allocation, allocation_size); }
    bool Read(void* destination, size_t address, size_t size) override {
        if (address >= bytes.size() || size > bytes.size() - address) return false;
        std::memcpy(destination, bytes.data() + address, size);
        return true;
    }
    bool Write(void* source, size_t address, size_t size) override {
        if (address >= bytes.size() || size > bytes.size() - address) return false;
        std::memcpy(bytes.data() + address, source, size);
        return true;
    }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address < bytes.size() ? bytes.data() + address : nullptr;
    }
    CodeFetchResult ReadCode(void* destination, size_t address, size_t size) override {
        return Read(destination, address, size) ? CodeFetchResult::Success : CodeFetchResult::Fault;
    }
    bool MatchesCodeAddressSpace(std::uintptr_t bias, std::uint64_t mask) const override {
        return bias == reinterpret_cast<std::uintptr_t>(bytes.data()) && mask == bytes.size() - 1;
    }
private:
    void* allocation{};
    size_t allocation_size{};
};

}  // namespace

void Arm64Guest() {
    const std::array<u64, 10> edges{0, 1, 2, UINT64_MAX, 0x7fffffffffffffffULL,
        0x8000000000000000ULL, 0x8000000080000000ULL, 0xffffffff, 0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    u64 seed = 0x616a586e53564dULL;
    for (u32 operation = 0; operation < 3; ++operation)
    for (u32 run = 0; run < 48; ++run) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const u64 a = run < edges.size() ? edges[run] : seed;
        const u64 b = run < edges.size() ? edges[(run + 3) % edges.size()] : std::rotl(seed, 17);
        const bool carry_in = (run & 1) != 0;
        // ADDS/SUBS/ADCS x2,x0,x1; B.EQ skips MOV x3,#7; exit syscall.
        const u32 arithmetic = std::array{0xab010002u, 0xeb010002u, 0xba010002u}[operation];
        const std::array<u32, 5> program{arithmetic, 0x54000040, 0xd28000e3, 0xd2800ba8, 0xd4000001};
        CodeMemory memory;
        std::memcpy(memory.bytes.data() + 0x100, program.data(), sizeof(program));
        using Instance = translator::arm64::Arm64Instance;
        using Core = translator::arm64::Arm64Core;
        std::unique_ptr<Instance, decltype(&Instance::Destroy)> instance{
            Instance::Make(memory.bytes.data(), memory.bytes.size() - 1, &memory), Instance::Destroy};
        Check(instance->GetAddressSpace()->GetConfig().backend_isa == kLoongArch &&
              instance->GetAddressSpace()->GetConfig().enable_jit, "ARM64 guest selects the native LoongArch JIT");
        std::unique_ptr<Core, decltype(&Core::Destroy)> core{Core::Make(instance.get()), Core::Destroy};
        auto& context = core->GetContext();
        context.r[0] = a; context.r[1] = b; context.r[3] = 0x102;
        constexpr u32 other_pstate = 0x05a50a00;
        context.pstate = other_pstate | (carry_in ? 0x20000000 : 0);
        context.pc = 0x100;
        const u64 result = operation == 1 ? a - b : a + b + (operation == 2 && carry_in);
        const bool carry = operation == 1 ? a >= b :
            (static_cast<unsigned __int128>(a) + b + (operation == 2 && carry_in)) >> 64;
        const auto left = static_cast<__int128>(std::bit_cast<s64>(a));
        const auto right = static_cast<__int128>(std::bit_cast<s64>(b));
        const auto signed_result = operation == 1 ? left - right : left + right + (operation == 2 && carry_in);
        const bool overflow = signed_result < -(__int128{1} << 63) || signed_result > (__int128{1} << 63) - 1;
        const u32 flags = (u32(result >> 63) << 31) | (u32(result == 0) << 30) |
                          (u32(carry) << 29) | (u32(overflow) << 28);
        Check(core->Run() == translator::Syscall, "ARM64 frontend -> LoongArch Runtime -> syscall");
        Check(context.r[2] == result && context.r[3] == (result == 0 ? 0x102 : 7),
              "independent ARM64 arithmetic and condition branch outcome");
        Check((context.pstate & 0xf0000000) == flags && core->GetSyscallNumber() == 93,
              "independent guest NZCV publication and syscall number op=" + std::to_string(operation) +
              " run=" + std::to_string(run) + " expectedflags=" + std::to_string(flags) +
              " actualflags=" + std::to_string(context.pstate) + " syscall=" + std::to_string(core->GetSyscallNumber()) +
              " pc=" + std::to_string(context.pc));
        Check(context.pc == 0x114, "ARM64 guest publishes PC after the syscall");
        Check((context.pstate & ~u32{0xf0000000}) == other_pstate,
              "ARM64 guest preserves every non-NZCV PSTATE bit across host flag synchronization");
    }
    std::cout << "PASS 144 ARM64 frontend arithmetic/flags/branch/Runtime syscall programs\n";
}

}  // namespace swift::tests::loongarch64
