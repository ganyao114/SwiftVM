#include "single_instruction_code.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/context.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "Private single-instruction execution requires Linux x64 SysV"
#endif

namespace swift::runtime::backend::x64 {
namespace {
constexpr std::uintptr_t kNativeSaveBytes = 6 * sizeof(u64);
constexpr std::uintptr_t kCallReturnBytes = sizeof(u64);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uintptr_t>) == sizeof(std::uintptr_t));

[[nodiscard]] bool Contains(std::uintptr_t begin, std::uintptr_t end,
                            std::uintptr_t address) noexcept {
    return address >= begin && address < end;
}
}

extern "C" u32 SwiftX64RunPrivateSingle(void* entry, State* state,
        std::atomic<std::uintptr_t>* expected_frame, u32 frame_size);

// Contract with Translator's direct-State prologue: six nonvolatile pushes
// followed by frame_size subtraction. RSP is 16-byte aligned before CALL;
// generated entry receives RDI=State and RSP=8 mod 16. RBX retains the anchor
// address across both successful execution and the generated recovery epilogue.
// x64 aligned MOV is an atomic release publication; the handler uses acquire
// loads. This bridge has no guest-access sites and can never recover its faults.
asm(R"(
    .text
    .p2align 4
    .globl SwiftX64RunPrivateSingle
    .hidden SwiftX64RunPrivateSingle
    .type SwiftX64RunPrivateSingle,@function
SwiftX64RunPrivateSingle:
    .cfi_startproc
    pushq %rbx
    .cfi_def_cfa_offset 16
    .cfi_offset %rbx,-16
    movq %rdx,%rbx
    movq %rdi,%r11
    movq %rsi,%rdi
    movl %ecx,%ecx
    movq %rsp,%rax
    subq $56,%rax
    subq %rcx,%rax
    movq %rax,(%rbx)
    call *%r11
    movq $0,(%rbx)
    popq %rbx
    .cfi_def_cfa_offset 8
    .cfi_restore %rbx
    ret
    .cfi_endproc
    .size SwiftX64RunPrivateSingle,.-SwiftX64RunPrivateSingle
)");

SingleInstructionFaultDescriptor::SingleInstructionFaultDescriptor(State* state,
        u32 frame_bytes, u64 original, std::span<const GuestAccessRange> accesses)
        : owning_state(state), guest_bias(state ? reinterpret_cast<std::uintptr_t>(state->pt) : 0),
          guest_mask(state ? state->guest_addr_mask : 0),
          guest_limit(state ? state->guest_addr_limit : 0), frame_size(frame_bytes),
          original_pc(original), sites(accesses.begin(), accesses.end()) {}

bool SingleInstructionFaultDescriptor::ClassifyFault(std::uintptr_t host_pc,
        std::uintptr_t fault_address, std::uintptr_t recovery_slot,
        std::uintptr_t frame_slot, const State* active_state,
        std::uintptr_t r14, std::uintptr_t r13, std::uintptr_t rsp) const noexcept {
    const auto frame = expected_frame.load(std::memory_order_acquire);
    constexpr auto limit = std::numeric_limits<std::uintptr_t>::max();
    if (!owning_state || active_state != owning_state || !frame ||
        recovery_slot != recovery_pc || !Contains(rx_begin, rx_end, recovery_pc) ||
        frame_slot != frame || r13 != frame || rsp != frame || (frame & 15) ||
        frame_size > limit - frame ||
        kNativeSaveBytes + kCallReturnBytes > limit - frame - frame_size ||
        !Contains(rx_begin, rx_end, host_pc)) return false;

    // These are immutable integers and a preallocated vector; no State
    // dereference, map oracle, allocation, lock, or semantic helper runs here.
    const auto offset = host_pc - rx_begin;
    for (const auto& site : sites) {
        if (offset < site.begin_offset || offset >= site.end_offset) continue;
        if (!site.size || r14 < guest_bias || site.size - 1 > limit - r14)
            return false;
        const auto guest = static_cast<u64>(r14 - guest_bias);
        if (guest > guest_mask || site.size - 1 > guest_mask - guest ||
            guest >= guest_limit || site.size > guest_limit - guest) return false;
        return fault_address >= r14 && fault_address - r14 < site.size;
    }
    return false;
}

PrivateSingleInstructionCode::PrivateSingleInstructionCode(State* state,
        std::span<const u8> finished_code, u32 recovery_offset, u32 frame_size,
        u64 original_pc, std::span<const GuestAccessRange> sites,
        GuestAccessCoverage coverage)
        : descriptor(state, frame_size, original_pc, sites) {
    constexpr auto limit = std::numeric_limits<std::size_t>::max();
    if (!state || finished_code.empty() || finished_code.size() > UINT32_MAX ||
        recovery_offset >= finished_code.size() || !frame_size || frame_size % 16 != 8 ||
        coverage != GuestAccessCoverage::Precise)
        throw std::invalid_argument("Invalid private single-instruction code contract");
    u32 previous_end{};
    for (const auto& site : descriptor.sites) {
        const bool width = site.size == 1 || site.size == 2 || site.size == 4 ||
                           site.size == 8 || site.size == 16;
        const bool kind = site.kind == GuestAccessKind::Read ||
                          site.kind == GuestAccessKind::Write ||
                          site.kind == GuestAccessKind::ReadWrite;
        if (!width || !kind || site.begin_offset < previous_end ||
            site.begin_offset >= site.end_offset || site.end_offset > finished_code.size() ||
            site.end_offset > recovery_offset)
            throw std::invalid_argument("Invalid private guest access range");
        previous_end = site.end_offset;
    }
    const long native_page = sysconf(_SC_PAGESIZE);
    if (native_page <= 0) throw std::runtime_error("Cannot determine private code page size");
    const auto page = static_cast<std::size_t>(native_page);
    if (finished_code.size() > limit - (page - 1))
        throw std::overflow_error("Private code mapping size overflow");
    mapping_size = ((finished_code.size() + page - 1) / page) * page;
    mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        mapping = nullptr;
        throw std::system_error(errno, std::generic_category(), "Private code mmap");
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(mapping);
    if (finished_code.size() > limit - begin) {
        munmap(mapping, mapping_size);
        mapping = nullptr;
        throw std::overflow_error("Private code address overflow");
    }
    std::memcpy(mapping, finished_code.data(), finished_code.size());
    if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        const int error = errno;
        munmap(mapping, mapping_size);
        mapping = nullptr;
        throw std::system_error(error, std::generic_category(), "Private code mprotect RX");
    }
    descriptor.rx_begin = begin;
    descriptor.rx_end = begin + finished_code.size();
    descriptor.recovery_pc = begin + recovery_offset;
}

PrivateSingleInstructionCode::~PrivateSingleInstructionCode() noexcept {
    if (mapping) munmap(mapping, mapping_size);
}

HaltReason PrivateSingleInstructionCode::Run() {
    if (descriptor.expected_frame.load(std::memory_order_acquire) != 0)
        throw std::logic_error("Private single-instruction code already active");
    return static_cast<HaltReason>(SwiftX64RunPrivateSingle(mapping,
            const_cast<State*>(descriptor.owning_state), &descriptor.expected_frame,
            descriptor.frame_size));
}

} // namespace swift::runtime::backend::x64
