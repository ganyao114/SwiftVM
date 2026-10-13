#include "test_support.h"
#include "runtime/backend/loongarch64/single_instruction_code.h"
#include "runtime/backend/signal_handler.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace swift::tests::loongarch64 {
namespace {
constexpr u64 pc = 0xc100;
struct Page {
    size_t size{static_cast<size_t>(getpagesize())};
    void* data{mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)};
    Page() { if (data == MAP_FAILED) throw std::runtime_error("private Step guest mmap"); }
    ~Page() { if (data != MAP_FAILED) munmap(data, size); }
};
struct Probe {
    backend::State* state{};
    const la::SingleInstructionFaultDescriptor* descriptor{};
    const std::vector<la::GuestAccessRange>* sites{};
    la::PrivateSingleInstructionCode* code{};
    ir::Block* retained{};
    bool typed{}, all_native{}, native_subextent{}, all_negative{}, typed_exact{}, typed_negative{};
    bool frame_matches{}, owner_live{}, reentry_rejected{};
    u32 calls{}, negative_count{};
};

u64 ProbeCode(u64 opaque, u64) noexcept {
    auto& probe = *reinterpret_cast<Probe*>(opaque);
    ++probe.calls;
    auto* state = probe.state;
    const auto& descriptor = *probe.descriptor;
    const auto frame = state->spill_area[backend::kRiscvRecoveryFrameSlot];
    const auto recovery = state->spill_area[backend::kRiscvRecoveryPcSlot];
    const auto base = reinterpret_cast<std::uintptr_t>(state->pt) + 24;
    probe.frame_matches = frame == descriptor.ExpectedFrame() && recovery == descriptor.RecoveryPc();
    probe.owner_live = probe.retained && probe.retained->use_count() != 0 &&
                      probe.retained->GetInstList().front().GetOp() == ir::OpCode::Nop;
    la::SingleFaultContext context{};
    context.active_state = state; context.recovery_slot = recovery; context.frame_slot = frame;
    context.gprs[3] = context.gprs[24] = frame - descriptor.HomeBytes();
    context.gprs[22] = frame; context.gprs[23] = reinterpret_cast<std::uintptr_t>(state);
    context.gprs[16] = context.gprs[18] = base;
    la::SetGuestFaultScope(state, {base, 32});
    probe.all_native = true;
    for (const auto& site : *probe.sites) {
        context.host_pc = descriptor.RxBegin() + site.begin_offset;
        context.fault_address = base + site.displacement;
        probe.all_native &= descriptor.ClassifyFault(context) == la::GuestFaultKind::Native;
        context.fault_address += site.size - 1;
        probe.all_native &= descriptor.ClassifyFault(context) == la::GuestFaultKind::Native;
    }
    context.host_pc = descriptor.RxBegin() + probe.sites->front().begin_offset;
    context.fault_address = base + 8;
    probe.native_subextent = descriptor.ClassifyFault(context) == la::GuestFaultKind::None;
    context.fault_address = base;
    probe.all_negative = true;
    for (u32 mutation = 0; mutation < 20; ++mutation) {
        auto invalid = context;
        switch (mutation) {
            case 0: invalid.active_state = nullptr; break;
            case 1: ++invalid.recovery_slot; break;
            case 2: invalid.frame_slot += 16; break;
            case 3: invalid.gprs[22] += 16; break;
            case 4: invalid.gprs[23] += 16; break;
            case 5: invalid.gprs[24] += 16; break;
            case 6: invalid.gprs[3] -= 16; break;
            case 7: invalid.gprs[18] += 8; break;
            case 8: invalid.gprs[18] = UINTPTR_MAX - 3; break;
            case 9: invalid.host_pc = descriptor.RxBegin(); break;
            case 10: invalid.host_pc = descriptor.RecoveryPc(); break;
            case 11: invalid.host_pc = descriptor.RxEnd(); break;
            case 12: invalid.host_pc = reinterpret_cast<std::uintptr_t>(&ProbeCode); break;
            case 13: ++invalid.host_pc; break;
            case 14: invalid.fault_address = base - 1; break;
            case 15: invalid.fault_address = base + 32; break;
            case 16: invalid.recovery_slot = 0; break;
            case 17: invalid.frame_slot = 0; break;
            case 18: invalid.gprs[3] = invalid.gprs[24] = frame + 16 - descriptor.HomeBytes();
                     invalid.frame_slot = invalid.gprs[22] = frame + 16; break;
            case 19: invalid.gprs[3] = invalid.gprs[24] = 0; break;
        }
        probe.all_negative &= descriptor.ClassifyFault(invalid) == la::GuestFaultKind::None;
        ++probe.negative_count;
    }
    la::SetGuestFaultScope(state, {base, 8}, la::GuestFaultKind::TypedKernel, {base + 40, 8});
    context.host_pc = reinterpret_cast<std::uintptr_t>(&ProbeCode);
    context.gprs[3] -= 32; context.gprs[22] = 0; context.gprs[23] = 0; context.gprs[24] = 0;
    const auto wanted = probe.typed ? la::GuestFaultKind::TypedKernel : la::GuestFaultKind::None;
    probe.typed_exact = descriptor.ClassifyFault(context) == wanted;
    context.fault_address = base + 47;
    probe.typed_exact &= descriptor.ClassifyFault(context) == wanted;
    probe.typed_negative = true;
    for (u32 mutation = 0; mutation < 5; ++mutation) {
        auto invalid = context;
        switch (mutation) {
            case 0: invalid.fault_address = base + 39; break;
            case 1: invalid.host_pc = descriptor.RxBegin() + probe.sites->front().begin_offset; break;
            case 2: invalid.frame_slot += 16; break;
            case 3: invalid.gprs[3] = frame; break;
            case 4: ++invalid.gprs[3]; break;
        }
        probe.typed_negative &= descriptor.ClassifyFault(invalid) == la::GuestFaultKind::None;
    }
    try { (void)probe.code->Run(); } catch (const std::logic_error&) { probe.reentry_rejected = true; }
    // Resume only the exact native access scope; the probe itself is not an
    // authorized guest memory helper and no synthetic classification is a signal.
    la::SetGuestFaultScope(state, {base, 32});
    return 0;
}

u64 PreparePhysicalHelper(backend::State* state, u64 scoped) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(state->pt) + 24;
    la::ClearGuestFaultScope(state);
    if (scoped) la::SetGuestFaultScope(state, {address, 8}, la::GuestFaultKind::TypedKernel);
    return address;
}

bool ReadExecuteOnly(std::uintptr_t address) {
    std::ifstream maps{"/proc/self/maps"};
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin{}, end{};
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, permissions) == 3 &&
            address >= begin && address < end)
            return permissions[0] == 'r' && permissions[1] == '-' && permissions[2] == 'x';
    }
    return false;
}

struct Program {
    IntrusivePtr<ir::Block> ir{new ir::Block{ir::Location{pc}}};
    la::Assembler as;
    std::vector<la::GuestAccessRange> sites;
    u32 recovery_offset{};
    explicit Program(Probe& probe, bool physical_helper = false, bool scoped_helper = true) {
        ir->AppendInst(ir::OpCode::Nop); ir->SetTerminal(ir::terminal::ReturnToDispatch{}); ir->ReIdInstr();
        la::Label recovery, exit;
        as.addi_d(la::sp, la::sp, -s32(la::kBlockSavedFrameSize));
        for (u32 i = 0; i < la::saved_gprs.size(); ++i)
            as.st_d(la::saved_gprs[i], la::sp, la::kGPRSaveOffset + i * 8);
        for (u32 i = 0; i < la::saved_fprs.size(); ++i)
            as.fst_d(la::saved_fprs[i], la::sp, la::kFPRSaveOffset + i * 8);
        as.movfcsr2gr(la::t0, la::fcsr0); as.st_d(la::t0, la::sp, la::kFCSRSaveOffset);
        as.MV(la::frame, la::sp); as.MV(la::state, la::a0);
        as.addi_d(la::sp, la::sp, -16); as.MV(la::values, la::sp); as.st_d(la::values, la::frame, 160);
        as.LILabel(la::t0, &recovery); as.st_d(la::t0, la::state, backend::state_offset_riscv_recovery_pc);
        as.st_d(la::frame, la::state, backend::state_offset_riscv_recovery_frame);
        // Deliberately perturb nonvolatile machine state inside the private
        // body; normal and recovery epilogues must preserve the caller's ABI.
        for (u32 index = 25; index < 32; ++index) as.LI(la::GPR{index}, 0x76540000 + index);
        as.LI(la::t0, 2u << 8); as.movgr2fcsr(la::fcsr0, la::t0);
        for (u32 index = 24; index < 32; ++index) as.movgr2fr_d(la::FPR{index}, la::t0);
        as.LI(la::a0, reinterpret_cast<u64>(&probe)); as.MV(la::a1, la::state);
        as.LI(la::t0, reinterpret_cast<u64>(&ProbeCode)); as.jirl(la::ra, la::t0, 0);
        if (physical_helper) {
            as.MV(la::a0, la::state); as.LI(la::a1, scoped_helper);
            as.LI(la::t0, reinterpret_cast<u64>(&PreparePhysicalHelper)); as.jirl(la::ra, la::t0, 0);
            as.LI(la::t0, reinterpret_cast<u64>(&SwiftLoongArchFaultHelper)); as.jirl(la::ra, la::t0, 0);
        }
        as.ld_d(la::t6, la::state, backend::state_offset_pt); as.addi_d(la::t6, la::t6, 24);
        auto begin = static_cast<u32>(as.Size()); as.ld_d(la::t0, la::t6, 0);
        sites.push_back({begin, static_cast<u32>(as.Size()), 8, la::GuestAccessKind::Read, 18, 0});
        begin = static_cast<u32>(as.Size()); as.ld_d(la::t0, la::t6, 8);
        sites.push_back({begin, static_cast<u32>(as.Size()), 8, la::GuestAccessKind::Read, 18, 8});
        begin = static_cast<u32>(as.Size()); as.st_h(la::t0, la::t6, 16);
        sites.push_back({begin, static_cast<u32>(as.Size()), 2, la::GuestAccessKind::Write, 18, 16});
        as.LI(la::a0, static_cast<u32>(HaltReason::Step)); as.J(&exit);
        recovery_offset = static_cast<u32>(as.Size()); as.Bind(&recovery);
        as.ld_d(la::values, la::frame, 160); as.ld_wu(la::a0, la::state, backend::state_offset_halt_reason);
        as.Bind(&exit);
        as.st_d(la::zero, la::state, la::state_offset_loongarch_guest_fault_cookie); as.dbar(0);
        as.st_d(la::zero, la::state, backend::state_offset_riscv_recovery_pc);
        as.st_d(la::zero, la::state, backend::state_offset_riscv_recovery_frame);
        as.MV(la::sp, la::frame); as.ld_d(la::t0, la::sp, la::kFCSRSaveOffset);
        as.movgr2fcsr(la::fcsr0, la::t0);
        for (u32 i = 0; i < la::saved_fprs.size(); ++i)
            as.fld_d(la::saved_fprs[i], la::sp, la::kFPRSaveOffset + i * 8);
        for (u32 i = 0; i < la::saved_gprs.size(); ++i)
            as.ld_d(la::saved_gprs[i], la::sp, la::kGPRSaveOffset + i * 8);
        as.addi_d(la::sp, la::sp, la::kBlockSavedFrameSize); as.jirl(la::zero, la::ra, 0); as.Finalize();
    }
};

la::PrivateSingleInstructionCode* abi_owner{};
HaltReason RunPrivate(backend::State*) { return abi_owner->Run(); }
const la::SingleInstructionFaultDescriptor* fault_descriptor{};
backend::State* fault_state{};
volatile sig_atomic_t physical_faults{};
la::GuestFaultKind physical_kind{};
void PhysicalFault(int, siginfo_t* info, void* raw) {
    auto* context = static_cast<ucontext_t*>(raw);
    la::SingleFaultContext interrupted{};
    interrupted.host_pc = backend::SignalHandler::GetContextPC(context);
    interrupted.fault_address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    interrupted.active_state = fault_state;
    interrupted.recovery_slot = fault_state->spill_area[backend::kRiscvRecoveryPcSlot];
    interrupted.frame_slot = fault_state->spill_area[backend::kRiscvRecoveryFrameSlot];
    for (u32 index = 0; index < 32; ++index)
        interrupted.gprs[index] = backend::SignalHandler::GetContextGPR(context, index);
    const auto kind = fault_descriptor->ClassifyFault(interrupted);
    if (kind == la::GuestFaultKind::None) _exit(90);
    if (kind != physical_kind) _exit(93);
    physical_faults = 1;
    fault_state->halt_reason = HaltReason::PageFatal;
    fault_state->current_loc = ir::Location{fault_descriptor->OriginalPc()};
    la::ClearGuestFaultScope(fault_state);
    backend::SignalHandler::SetContextGPR(context, 22, fault_descriptor->ExpectedFrame());
    backend::SignalHandler::SetContextGPR(context, 23, reinterpret_cast<std::uintptr_t>(fault_state));
    backend::SignalHandler::SetContextGPR(context, 3, fault_descriptor->ExpectedFrame());
    backend::SignalHandler::SetContextPC(context, fault_descriptor->RecoveryPc());
}

void RejectedConstruction() {
    StateStorage machine; Page guest;
    machine.state->pt = guest.data; machine.state->guest_addr_mask = 127; machine.state->guest_addr_limit = 128;
    Probe probe; probe.state = machine.state; Program program{probe};
    const auto state_before = machine.bytes;
    for (u32 mutation = 0; mutation < 24; ++mutation) {
        bool rejected{};
        {
            auto* state = machine.state;
            auto bytes = std::span<const u8>{program.as.Data(), program.as.Size()};
            auto retained = program.ir;
            auto sites = program.sites;
            auto recovery = program.recovery_offset;
            u32 homes = 16; u64 original = pc;
            auto coverage = la::GuestAccessCoverage::Precise;
            switch (mutation) {
                case 0: state = nullptr; break;
                case 1: retained.reset(); break;
                case 2: ++original; break;
                case 3: bytes = {}; break;
                case 4: bytes = bytes.first(bytes.size() - 1); break;
                case 5: recovery = bytes.size(); break;
                case 6: ++recovery; break;
                case 7: homes = 0; break;
                case 8: homes = 17; break;
                case 9: homes = 32; break;
                case 10: homes = 8193 * 16; break;
                case 11: coverage = la::GuestAccessCoverage::Unsupported; break;
                case 12: sites.front().end_offset += 4; break;
                case 13: ++sites.front().begin_offset; break;
                case 14: sites.front().size = 4; break;
                case 15: sites.front().kind = la::GuestAccessKind::Write; break;
                case 16: sites.front().address_gpr = 23; break;
                case 17: ++sites.front().displacement; break;
                case 18: sites.front().begin_offset = 0; sites.front().end_offset = 4; break;
                case 19: sites.insert(sites.begin(), sites.front()); break;
                case 20: recovery = sites.back().begin_offset; break;
                case 21: retained.reset(new ir::Block{ir::Location{pc}}); break;
                case 22: program.ir->GetInstList().front().SetId(17); break;
                case 23: program.ir->GetInstList().front().SetReturn(ir::ValueType::V256); break;
            }
            try {
                la::PrivateSingleInstructionCode invalid{state, bytes, recovery, homes, original,
                        sites, coverage, retained};
            } catch (const std::invalid_argument&) { rejected = true; }
            program.ir->GetInstList().front().SetId(0);
            program.ir->GetInstList().front().SetReturn(ir::ValueType::VOID);
        }
        Check(rejected && machine.bytes == state_before && program.ir->use_count() == 1,
              "invalid private contract rolls back construction without State effects or retained IR leaks");
    }
}
}

void PrivateSingleInstructionCodes() {
    const auto before = checks;
    RejectedConstruction();
    for (bool typed : {false, true}) {
        StateStorage machine; Page guest;
        machine.state->pt = guest.data; machine.state->guest_addr_mask = 127; machine.state->guest_addr_limit = 128;
        u32 callback_calls{};
        machine.state->interp_range_check_ctx = &callback_calls;
        machine.state->interp_range_check = [](void* opaque, u64, u64) {
            ++*static_cast<u32*>(opaque); return false;
        };
        const auto state_before = machine.bytes;
        Probe probe; probe.state = machine.state; probe.typed = typed;
        Program program{probe}; probe.retained = program.ir.get(); probe.sites = &program.sites;
        std::uintptr_t mapping{};
        {
            la::PrivateSingleInstructionCode code{machine.state, {program.as.Data(), program.as.Size()},
                    program.recovery_offset, 16, pc, program.sites, la::GuestAccessCoverage::Precise,
                    program.ir, typed};
            probe.code = &code; probe.descriptor = &code.Descriptor(); abi_owner = &code;
            mapping = code.Descriptor().RxBegin();
            Check(ReadExecuteOnly(mapping), "published private code has RX permissions without writable pages");
            Check(code.Descriptor().ExpectedFrame() == 0 && machine.bytes == state_before,
                  "private construction does not mutate State or acquire recovery ownership");
            Check(program.ir->use_count() == 2, "private code independently retains every embedded IR pointer");
            program.ir.reset();
            Check(la::GetGuestFaultKind(machine.state) == la::GuestFaultKind::None,
                  "inactive private owner leaves the guest fault scope untouched");
            la::SingleFaultContext inactive{};
            inactive.host_pc = mapping + program.sites.front().begin_offset;
            inactive.fault_address = reinterpret_cast<std::uintptr_t>(guest.data) + 24;
            inactive.active_state = machine.state; inactive.recovery_slot = code.Descriptor().RecoveryPc();
            inactive.frame_slot = reinterpret_cast<std::uintptr_t>(guest.data) + 256;
            inactive.gprs[22] = inactive.frame_slot;
            inactive.gprs[23] = reinterpret_cast<std::uintptr_t>(machine.state);
            inactive.gprs[24] = inactive.gprs[3] = inactive.frame_slot - 16;
            inactive.gprs[18] = inactive.fault_address;
            la::SetGuestFaultScope(machine.state, {inactive.fault_address, 32});
            Check(code.Descriptor().ClassifyFault(inactive) == la::GuestFaultKind::None,
                  "forged State slots and exact native PC cannot activate an inactive independent frame");
            la::ClearGuestFaultScope(machine.state);
            Check(code.Run() == HaltReason::Step, "private LP64D bridge returns a complete retirement reason");
            Check(probe.calls == 1 && probe.frame_matches && probe.owner_live && probe.reentry_rejected,
                  "real bridge anchor, retained IR and recursive private refusal stay live through entry");
            Check(probe.all_native && probe.native_subextent && probe.all_negative && probe.negative_count == 20,
                  "exact native PC, base, displacement and subextent reject forged interrupted contexts");
            Check(probe.typed_exact && probe.typed_negative, "typed scopes require explicit capability and exact independent frame");
            Check(callback_calls == 0, "descriptor classification never invokes a guest range callback");
            Check(code.Descriptor().ExpectedFrame() == 0 &&
                  !machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] &&
                  !machine.state->spill_area[backend::kRiscvRecoveryPcSlot] &&
                  la::GetGuestFaultKind(machine.state) == la::GuestFaultKind::None,
                  "normal return releases both independent and generated recovery ownership");
            Check(CheckABI(&RunPrivate, machine.state) == 1, "private body and bridge preserve complete GP/FPR/FCSR LP64D ABI");
            abi_owner = nullptr;
        }
        unsigned char resident{}; errno = 0;
        Check(mincore(reinterpret_cast<void*>(mapping), guest.size, &resident) == -1 && errno == ENOMEM,
              "private RX mapping is reclaimed immediately after descriptor lifetime ends");
    }
    // Real protection faults test native recovery, a typed helper abandoning
    // every nonvolatile bank, and refusal of inactive/unaudited host callbacks.
    for (u32 mode = 0; mode < 4; ++mode) {
      const auto child = fork();
      if (child < 0) throw std::runtime_error("fork private recovery contract");
      if (!child) {
        StateStorage machine; Page guest;
        machine.state->pt = guest.data; machine.state->guest_addr_mask = 127; machine.state->guest_addr_limit = 128;
        const bool helper = mode != 0, audited = mode == 1, scoped = mode != 2;
        Probe probe; probe.state = machine.state; Program program{probe, helper, scoped};
        probe.retained = program.ir.get(); probe.sites = &program.sites;
        la::PrivateSingleInstructionCode code{machine.state, {program.as.Data(), program.as.Size()},
                program.recovery_offset, 16, pc, program.sites, la::GuestAccessCoverage::Precise, program.ir, audited};
        probe.code = &code; probe.descriptor = &code.Descriptor(); fault_descriptor = &code.Descriptor();
        fault_state = machine.state; abi_owner = &code; physical_faults = 0;
        physical_kind = mode == 0 ? la::GuestFaultKind::Native : la::GuestFaultKind::TypedKernel;
        struct sigaction action{}; action.sa_sigaction = &PhysicalFault; action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, nullptr) || sigaction(SIGBUS, &action, nullptr) ||
            mprotect(guest.data, guest.size, PROT_NONE)) _exit(91);
        const auto reason = code.Run();
        const auto abi = CheckABI(&RunPrivate, machine.state);
        _exit(reason == HaltReason::PageFatal && physical_faults && abi == 1 &&
              machine.state->current_loc.Value() == pc && code.Descriptor().ExpectedFrame() == 0 &&
              !machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] &&
              !machine.state->spill_area[backend::kRiscvRecoveryPcSlot] ? 0 : 92);
      }
      int status{};
      const int expected_exit = mode < 2 ? 0 : 90;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == expected_exit,
            mode < 2 ? "actual native/typed private fault restores exact PC, complete ABI and recovery anchors" :
                       "actual unscoped or unaudited host callback fault cannot acquire private recovery rights");
    }
    std::cout << "PASS private single-instruction code " << checks - before << " checks\n";
}

} // namespace swift::tests::loongarch64
