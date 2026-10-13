#include "single_instruction_code.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/cache_clear.h"
#include "runtime/backend/loongarch64/defines.h"

#if !defined(__linux__) || !defined(__loongarch64)
#error "Private LoongArch single-instruction execution requires Linux LP64D"
#endif

namespace swift::runtime::backend::loongarch64 {
namespace {
constexpr u32 kBridgeFrameSize = 176;
constexpr u32 kMaxHomes = 8192 * kValueStride;
static_assert(kBlockSavedFrameSize == 176 && kValueStride == 16);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uintptr_t>) == sizeof(std::uintptr_t));

bool Contains(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t address) noexcept {
    return address >= begin && address < end;
}

bool AddDisplacement(std::uintptr_t base, s32 displacement, std::uintptr_t& address) noexcept {
    if (displacement < 0) {
        const auto magnitude = static_cast<std::uintptr_t>(-s64(displacement));
        if (base < magnitude) return false;
        address = base - magnitude;
    } else {
        const auto magnitude = static_cast<std::uintptr_t>(displacement);
        if (base > UINTPTR_MAX - magnitude) return false;
        address = base + magnitude;
    }
    return true;
}
} // namespace

extern "C" u32 SwiftLoongArchRunPrivateSingle(void* entry, State* state,
        std::atomic<std::uintptr_t>* expected_frame);

// LP64D CALL does not push a return address. After this bridge's independent
// complete ABI save, generated entry subtracts exactly 176 bytes before setting
// r22=frame and subtracting home_bytes for r24=values. Publish that exact frame
// with release ordering before entry, independently of the generated State slots.
// Recovery can abandon a helper's saves; both generated code and this bridge
// retain the entire nonvolatile GP/FPR set and FCSR0. No bridge PC is a guest site.
asm(R"(
    .text
    .p2align 4
    .globl SwiftLoongArchRunPrivateSingle
    .hidden SwiftLoongArchRunPrivateSingle
    .type SwiftLoongArchRunPrivateSingle,@function
SwiftLoongArchRunPrivateSingle:
    addi.d $r3,$r3,-176
    st.d $r1,$r3,0
    st.d $r22,$r3,8
    st.d $r23,$r3,16
    st.d $r24,$r3,24
    st.d $r25,$r3,32
    st.d $r26,$r3,40
    st.d $r27,$r3,48
    st.d $r28,$r3,56
    st.d $r29,$r3,64
    st.d $r30,$r3,72
    st.d $r31,$r3,80
    fst.d $f24,$r3,88
    fst.d $f25,$r3,96
    fst.d $f26,$r3,104
    fst.d $f27,$r3,112
    fst.d $f28,$r3,120
    fst.d $f29,$r3,128
    fst.d $f30,$r3,136
    fst.d $f31,$r3,144
    movfcsr2gr $r12,$fcsr0
    st.d $r12,$r3,152
    st.d $r6,$r3,160
    move $r25,$r6
    move $r12,$r4
    move $r4,$r5
    addi.d $r13,$r3,-176
    dbar 0
    st.d $r13,$r25,0
    jirl $r1,$r12,0
    st.d $r4,$r3,168
    ld.d $r12,$r3,160
    dbar 0
    st.d $r0,$r12,0
    ld.d $r12,$r3,152
    movgr2fcsr $fcsr0,$r12
    fld.d $f24,$r3,88
    fld.d $f25,$r3,96
    fld.d $f26,$r3,104
    fld.d $f27,$r3,112
    fld.d $f28,$r3,120
    fld.d $f29,$r3,128
    fld.d $f30,$r3,136
    fld.d $f31,$r3,144
    ld.d $r1,$r3,0
    ld.d $r22,$r3,8
    ld.d $r23,$r3,16
    ld.d $r24,$r3,24
    ld.d $r25,$r3,32
    ld.d $r26,$r3,40
    ld.d $r27,$r3,48
    ld.d $r28,$r3,56
    ld.d $r29,$r3,64
    ld.d $r30,$r3,72
    ld.d $r31,$r3,80
    ld.d $r4,$r3,168
    addi.d $r3,$r3,176
    jirl $r0,$r1,0
    .size SwiftLoongArchRunPrivateSingle,.-SwiftLoongArchRunPrivateSingle
)");

SingleInstructionFaultDescriptor::SingleInstructionFaultDescriptor(State* state,
        u32 homes, u64 original, std::span<const GuestAccessRange> accesses, bool typed)
        : owning_state(state), guest_bias(state ? reinterpret_cast<std::uintptr_t>(state->pt) : 0),
          guest_mask(state ? state->guest_addr_mask : 0),
          guest_limit(state ? state->guest_addr_limit : 0), home_bytes(homes),
          original_pc(original), sites(accesses.begin(), accesses.end()),
          audited_typed_guest_accesses(typed) {}

GuestFaultKind SingleInstructionFaultDescriptor::ClassifyFault(
        const SingleFaultContext& context) const noexcept {
    const auto frame = expected_frame.load(std::memory_order_acquire);
    if (!owning_state || context.active_state != owning_state || !frame ||
        (frame & 15) || frame < home_bytes ||
        frame > UINTPTR_MAX - kBlockSavedFrameSize - kBridgeFrameSize ||
        context.frame_slot != frame || context.recovery_slot != recovery_pc ||
        !Contains(rx_begin, rx_end, recovery_pc) || (recovery_pc & 3) ||
        !context.host_pc || (context.host_pc & 3)) return GuestFaultKind::None;
    const auto values = frame - home_bytes;
    const auto kind = GetGuestFaultKind(owning_state);
    if (kind == GuestFaultKind::None) return kind;
    const GuestFaultRange first{owning_state->spill_area[kGuestFaultBaseSlot],
                               owning_state->spill_area[kGuestFaultSizeSlot]};
    const GuestFaultRange second{owning_state->spill_area[kGuestFaultSecondBaseSlot],
                                owning_state->spill_area[kGuestFaultSecondSizeSlot]};
    const auto valid_range = [&](GuestFaultRange range) {
        if (!range.size || range.base < guest_bias || range.size - 1 > UINTPTR_MAX - range.base)
            return false;
        const u64 guest = range.base - guest_bias;
        return guest <= guest_mask && range.size - 1 <= guest_mask - guest &&
               guest < guest_limit && range.size <= guest_limit - guest;
    };
    const auto contains = [&](GuestFaultRange range) {
        return context.fault_address >= range.base &&
               context.fault_address - range.base < range.size;
    };
    if (!valid_range(first) || (second.size && !valid_range(second)) ||
        (!contains(first) && !contains(second))) return GuestFaultKind::None;
    if (kind == GuestFaultKind::TypedKernel) {
        // A typed kernel can spill/reuse r22..r24. Its fully checked scope is
        // published only for the audited memcpy/atomic, after callbacks return.
        // It cannot authorize a private prologue, epilogue or native body PC.
        return audited_typed_guest_accesses && !Contains(rx_begin, rx_end, context.host_pc) &&
               context.gprs[3] && !(context.gprs[3] & 15) && context.gprs[3] <= values
                ? kind : GuestFaultKind::None;
    }
    if (second.size || context.gprs[22] != frame ||
        context.gprs[23] != reinterpret_cast<std::uintptr_t>(owning_state) ||
        context.gprs[24] != values || context.gprs[3] != values ||
        !Contains(rx_begin, rx_end, context.host_pc)) return GuestFaultKind::None;
    const auto offset = context.host_pc - rx_begin;
    for (const auto& site : sites) {
        if (offset < site.begin_offset || offset >= site.end_offset) continue;
        std::uintptr_t address{};
        if (!AddDisplacement(context.gprs[site.address_gpr], site.displacement, address) ||
            (address & (site.size - 1)) || address < first.base ||
            address - first.base >= first.size ||
            site.size > first.size - (address - first.base)) return GuestFaultKind::None;
        return context.fault_address >= address && context.fault_address - address < site.size
                ? GuestFaultKind::Native : GuestFaultKind::None;
    }
    return GuestFaultKind::None;
}

PrivateSingleInstructionCode::PrivateSingleInstructionCode(State* state,
        std::span<const u8> finished_code, u32 recovery_offset, u32 home_bytes,
        u64 original_pc, std::span<const GuestAccessRange> sites, GuestAccessCoverage coverage,
        IntrusivePtr<ir::Block> ir_owner, bool typed)
        : retained_ir(std::move(ir_owner)), descriptor(state, home_bytes, original_pc, sites, typed) {
    if (!state || !retained_ir || !retained_ir->HasTerminal() ||
        retained_ir->GetStartLocation().Value() != original_pc ||
        finished_code.empty() || finished_code.size() > UINT32_MAX || (finished_code.size() & 3) ||
        recovery_offset >= finished_code.size() || (recovery_offset & 3) ||
        !home_bytes || (home_bytes & 15) || home_bytes > kMaxHomes ||
        coverage != GuestAccessCoverage::Precise)
        throw std::invalid_argument("Invalid private LoongArch single-instruction contract");
    u32 ordinal{};
    for (const auto& inst : retained_ir->GetInstList()) {
        if (ordinal >= 8192 || inst.Id() != ordinal++ || inst.ReturnType() == ir::ValueType::V256)
            throw std::invalid_argument("Private LoongArch code requires dense retained 128-bit IR");
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst.ArgAt(slot);
            if ((arg.IsValue() && arg.Get<ir::Value>().Type() == ir::ValueType::V256) ||
                (arg.GetType() == ir::ArgType::Uniform &&
                 arg.Get<ir::Uniform>().GetType() == ir::ValueType::V256))
                throw std::invalid_argument("Private LoongArch code cannot retain a V256 clone");
        }
    }
    if (home_bytes != std::max(1u, ordinal) * kValueStride)
        throw std::invalid_argument("Private LoongArch home size does not match retained IR");
    u32 previous_end{};
    for (const auto& site : descriptor.sites) {
        if (site.begin_offset < previous_end || site.end_offset > recovery_offset ||
            !GuestAccessEncodingValid(finished_code, site))
            throw std::invalid_argument("Invalid private LoongArch guest-access metadata");
        previous_end = site.end_offset;
    }
    const long native_page = sysconf(_SC_PAGESIZE);
    if (native_page <= 0) throw std::runtime_error("Cannot determine private code page size");
    const auto page = static_cast<size_t>(native_page);
    if (finished_code.size() > SIZE_MAX - (page - 1))
        throw std::overflow_error("Private LoongArch code mapping size overflow");
    mapping_size = ((finished_code.size() + page - 1) / page) * page;
    mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        mapping = nullptr;
        throw std::system_error(errno, std::generic_category(), "Private LoongArch code mmap");
    }
    try {
        const auto begin = reinterpret_cast<std::uintptr_t>(mapping);
        if (finished_code.size() > UINTPTR_MAX - begin)
            throw std::overflow_error("Private LoongArch code address overflow");
        std::memcpy(mapping, finished_code.data(), finished_code.size());
        ClearDCache(mapping, finished_code.size());
        if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0)
            throw std::system_error(errno, std::generic_category(), "Private LoongArch code mprotect RX");
        ClearICache(mapping, finished_code.size());
        descriptor.rx_begin = begin;
        descriptor.rx_end = begin + finished_code.size();
        descriptor.recovery_pc = begin + recovery_offset;
    } catch (...) {
        munmap(mapping, mapping_size);
        mapping = nullptr;
        throw;
    }
}

PrivateSingleInstructionCode::~PrivateSingleInstructionCode() noexcept {
    if (mapping) munmap(mapping, mapping_size);
}

HaltReason PrivateSingleInstructionCode::Run() {
    if (descriptor.expected_frame.load(std::memory_order_acquire) != 0)
        throw std::logic_error("Private LoongArch single-instruction code already active");
    return static_cast<HaltReason>(SwiftLoongArchRunPrivateSingle(mapping,
            descriptor.owning_state, &descriptor.expected_frame));
}

} // namespace swift::runtime::backend::loongarch64
