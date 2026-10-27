#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/compile/builder/ir_builder.h"

namespace swift::tests::riscv {
namespace {
using T = ir::ValueType;
constexpr u64 initial_flags = (u64{1} << 53) | 0x83;
constexpr u64 published_flags = initial_flags | (u64{1} << 29);
constexpr u64 marker = 0x7169;

bool OriginalAccepts(u64 raw, u64 mask, u64 limit, u64 size) {
    const u64 a = raw & mask;
    return a < limit && size <= limit - a && size - 1 <= mask - a;
}

struct Pages {
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    u8* mapping = static_cast<u8*>(mmap(nullptr, page * 6, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    u8* data{};
    Pages() {
        Check(mapping != MAP_FAILED, "allocate guard pages");
        data = mapping + page;
        Check(mprotect(data, page * 4, PROT_READ | PROT_WRITE) == 0, "enable test backing between guard pages");
    }
    ~Pages() { if (mapping != MAP_FAILED) munmap(mapping, page * 6); }
};

struct Probe {
    backend::State* state{};
    u32 mode{}; // 0 accept, 1 reject, 2 throw.
    std::vector<std::pair<u64, u64>> calls;
    bool observations_ok{true};
};
bool Range(void* opaque, u64 guest, u64 size) {
    auto& probe = *static_cast<Probe*>(opaque);
    probe.calls.emplace_back(guest, size);
    auto* participant = reinterpret_cast<rv::MemoryParticipant*>(probe.state->spill_area[backend::kRiscvMemoryParticipantSlot]);
    probe.observations_ok &= probe.state->host_cpu_flags == published_flags &&
        probe.state->current_loc.Value() == 0x7100 && probe.state->prev_loc.Value() == 0x7090 &&
        probe.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 &&
        (participant ? participant->active.load() == 0 : rv::anonymous_memory_readers.load() == 0);
    if (probe.mode == 2) throw std::runtime_error("injected bounds oracle throw");
    return probe.mode == 0;
}
void Flags(compile::IrBuilder& as) {
    auto one = as.LoadImm(T::U8, ir::Imm{u64{1}});
    as.SetCarry(one);
}
void Mark(compile::IrBuilder& as) {
    auto value = as.LoadImm(T::U64, ir::Imm{marker});
    as.StoreUniform(ir::Uniform{144, T::U64}, value);
}
void Setup(StateStorage& state, bool anonymous, Probe& probe) {
    state.state->host_cpu_flags = initial_flags;
    state.state->current_loc = ir::Location{0x7100}; state.state->prev_loc = ir::Location{0x7090};
    if (anonymous) state.state->spill_area[backend::kRiscvMemoryParticipantSlot] = 0;
    probe.state = state.state; probe.calls.reserve(4);
}
void Restored(StateStorage& state) {
    Check(state.memory_participant.active.load() == 0 && state.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 &&
          rv::anonymous_memory_readers.load() == 0 && rv::memory_kernel_depth.load() == 0,
          "bounds halt restores registered/anonymous lease and kernel depth");
    Check(state.state->host_cpu_flags == published_flags && state.state->current_loc.Value() == 0x7100 &&
          state.state->prev_loc.Value() == 0x7090, "bounds/oracle halt preserves published flags and location");
}

void Encoding(Compiled& code, u64 effective_size, u32 address_calls) {
    struct Instruction { u32 offset, word; bool full; };
    std::vector<Instruction> instructions;
    u32 subtraction_count{}, end_count{};
    const auto length = code.context.CurrentBufferSize();
    const auto* generated = code.context.GetMasm().GetCodeBuffer().GetOffsetPointer(0);
    const auto* bytes = reinterpret_cast<const u8*>(code.fn);
    Check(std::memcmp(generated, bytes, length) == 0, "complete published RX matches generated guard bytes");
    for (u32 at = 0; at + 2 <= code.context.CurrentBufferSize();) {
        u16 half{}; std::memcpy(&half, bytes + at, 2);
        if ((half & 3) != 3) { instructions.push_back({at, half, false}); at += 2; continue; }
        Check(at + 4 <= code.context.CurrentBufferSize(), "complete RV instruction in guard encoding");
        u32 word{}; std::memcpy(&word, bytes + at, 4);
        instructions.push_back({at, word, true}); at += 4;
        const u32 rd = (word >> 7) & 31, rs1 = (word >> 15) & 31, rs2 = (word >> 20) & 31;
        if ((word & 0x7f) == 0x33 && ((word >> 12) & 7) == 0 && (word >> 25) == 0x20 &&
            rs2 == 11 && ((rd == 5 && rs1 == 5) || (rd == 6 && rs1 == 6))) ++subtraction_count;
        if ((word & 0x7f) == 0x13 && ((word >> 12) & 7) == 0 && rd == 7 && rs1 == 11 &&
            (word >> 20) == effective_size - 1) ++end_count;
    }
    const bool small = effective_size >= 1 && effective_size <= 2048;
    Check(subtraction_count == (small ? 0 : address_calls * 2), "exact current/fallback bounds SUB encoding");
    Check(end_count == (small && effective_size > 1 ? address_calls : 0), "exact inclusive-end ADDI encoding");

    const auto load = [](const Instruction& inst, u32 rd, u32 offset) {
        const auto word = inst.word;
        return inst.full && (word & 0x7f) == 3 && ((word >> 12) & 7) == 3 &&
            ((word >> 7) & 31) == rd && ((word >> 15) & 31) == 9 && (word >> 20) == offset;
    };
    const auto branch = [](const Instruction& inst, u32 condition, u32 left, u32 right) {
        const auto word = inst.word;
        return inst.full && (word & 0x7f) == 0x63 && ((word >> 12) & 7) == condition &&
            ((word >> 15) & 31) == left && ((word >> 20) & 31) == right;
    };
    const auto target = [](const Instruction& inst) {
        const auto word = inst.word;
        const u32 immediate = ((word >> 31) << 12) | (((word >> 7) & 1) << 11) |
            (((word >> 25) & 63) << 5) | (((word >> 8) & 15) << 1);
        const s32 displacement = s32(immediate & 0xfff) - s32(immediate & 0x1000);
        return s64(inst.offset) + displacement;
    };
    const auto boundary = [&](s64 offset) {
        return std::any_of(instructions.begin(), instructions.end(),
                           [&](const Instruction& inst) { return inst.offset == offset; });
    };
    const auto immediate12 = [](u32 word) { return s32(word >> 20) - s32((word >> 31) << 12); };
    const auto constant = [&](size_t begin, size_t end, u32 reg) {
        Check(begin < end, "nonempty constant materialization");
        u64 value{};
        bool initialized{};
        for (auto i = begin; i < end; ++i) {
            const auto& inst = instructions[i];
            const auto word = inst.word, opcode = word & 0x7f, rd = (word >> 7) & 31;
            const auto rs = (word >> 15) & 31, funct = (word >> 12) & 7;
            Check(inst.full && rd == reg, "constant materialization writes only its scratch register");
            if (opcode == 0x37) {
                value = u64(s64(s32(word & 0xfffff000))); initialized = true;
            } else if ((opcode == 0x13 || opcode == 0x1b) && funct == 0) {
                Check(rs == 0 || (rs == reg && initialized), "constant ADDI/W reads zero or initialized scratch");
                value = (rs == 0 ? 0 : value) + u64(s64(immediate12(word)));
                if (opcode == 0x1b) value = u64(s64(s32(u32(value))));
                initialized = true;
            } else if (opcode == 0x13 && funct == 1 && (word >> 26) == 0) {
                Check(rs == reg && initialized, "constant SLLI reads initialized scratch");
                value <<= (word >> 20) & 63;
            } else {
                Check(false, "constant materialization has a supported exact RV64 immediate instruction");
            }
        }
        Check(initialized, "constant materialization initializes its scratch");
        return value;
    };
    const auto jump = [&](size_t begin) {
        Check(begin + 2 < instructions.size(), "complete three-instruction epilogue jump");
        const auto& upper = instructions[begin];
        const auto& lower = instructions[begin + 1];
        const auto& indirect = instructions[begin + 2];
        Check(upper.full && (upper.word & 0xfff) == ((30u << 7) | 0x17) &&
              lower.full && (lower.word & 0xfffff) == ((30u << 15) | (30u << 7) | 0x13) &&
              indirect.full && indirect.word == ((30u << 15) | 0x67),
              "return jumps through exact AUIPC/ADDI/JALR scratch sequence");
        const s64 destination = s64(upper.offset) + s64(s32(upper.word & 0xfffff000)) + immediate12(lower.word);
        Check(destination > indirect.offset && destination < length && boundary(destination),
              "return jump targets a forward in-buffer instruction boundary");
        return destination;
    };
    const u32 halt_offset = offsetof(backend::State, halt_reason);
    const u32 halt_store = ((halt_offset >> 5) << 25) | (5u << 20) | (9u << 15) | (2u << 12) |
        ((halt_offset & 31) << 7) | 0x23;
    const u32 host_reason = (u32(HaltReason::CallHost) << 20) | (5u << 7) | 0x1b;
    s64 return_epilogue{-1};
    for (size_t i = 1; i + 3 < instructions.size(); ++i)
        if (instructions[i].full && instructions[i].word == halt_store &&
            instructions[i - 1].full && instructions[i - 1].word == host_reason) {
            const auto destination = jump(i + 1);
            Check(return_epilogue == -1 || return_epilogue == destination, "normal host returns share one epilogue");
            return_epilogue = destination;
        }
    Check(return_epilogue >= 0, "normal CallHost return establishes the shared epilogue");
    u32 guards{};
    for (size_t i = 0; i + 2 < instructions.size(); ++i) {
        if (!load(instructions[i], 5, offsetof(backend::State, guest_addr_mask))) continue;
        const auto& mask = instructions[i + 1];
        Check(mask.full && mask.word == 0x0055f5b3, "each bounds guard masks a1 with the loaded guest mask");
        Check(load(instructions[i + 2], 6, offsetof(backend::State, guest_addr_limit)),
              "each bounds guard loads the exclusive limit into t1");
        const auto begin = i + 3;
        auto end = begin;
        while (end < instructions.size() && !load(instructions[end], 5, offsetof(backend::State, interp_range_check))) ++end;
        Check(end < instructions.size(), "each complete bounds guard precedes its oracle load");
        std::vector<const Instruction*> branches;
        for (auto j = begin; j < end; ++j)
            if (instructions[j].full && (instructions[j].word & 0x7f) == 0x63) branches.push_back(&instructions[j]);
        Check(branches.size() == (effective_size == 1 ? 1u : 3u), "exact rejection branch count per access guard");
        const auto fault = target(*branches.front());
        for (const auto* instruction : branches)
            Check(target(*instruction) == fault && fault > instructions[end].offset && fault < length && boundary(fault),
                  "all rejection branches share a forward in-buffer instruction boundary");
        const auto fault_index = size_t(std::find_if(instructions.begin(), instructions.end(),
            [&](const Instruction& inst) { return inst.offset == fault; }) - instructions.begin());
        Check(fault_index + 1 < instructions.size() &&
              constant(fault_index, fault_index + 1, 5) == u32(HaltReason::PageFatal) &&
              instructions[fault_index + 1].full && instructions[fault_index + 1].word == halt_store,
              "shared fault target materializes and stores PageFatal to State");
        Check(jump(fault_index + 2) == return_epilogue, "PageFatal returns through the normal shared epilogue");
        if (effective_size == 1) {
            Check(end == begin + 1 && branch(instructions[begin], 7, 11, 6),
                  "single-byte guard consists only of a1 >= limit rejection");
        } else if (small) {
            const u32 addi = (u32(effective_size - 1) << 20) | (11u << 15) | (7u << 7) | 0x13;
            Check(end == begin + 4 && instructions[begin].full && instructions[begin].word == addi,
                  "small guard computes exact inclusive end in t2");
            Check(branch(*branches[0], 6, 7, 11) && branch(*branches[1], 7, 7, 6) && branch(*branches[2], 6, 5, 7),
                  "small guard rejects end wrap, exclusive-limit crossing, and mask crossing in order");
        } else {
            Check(branches[0] == &instructions[begin] && branch(*branches[0], 7, 11, 6) &&
                  branch(*branches[1], 6, 6, 7) && branch(*branches[2], 6, 5, 7) && branches[2] == &instructions[end - 1],
                  "large fallback rejects start, insufficient limit span, and insufficient mask span in order");
            const auto subtract = [](const Instruction& inst, u32 reg) {
                return inst.full && inst.word == ((0x20u << 25) | (11u << 20) | (reg << 15) | (reg << 7) | 0x33);
            };
            Check(begin + 1 < end && subtract(instructions[begin + 1], 6), "large fallback subtracts start from limit");
            const auto limit_branch = size_t(branches[1] - instructions.data());
            const auto mask_branch = size_t(branches[2] - instructions.data());
            Check(constant(begin + 2, limit_branch, 7) == effective_size,
                  "large fallback materializes full size before the limit-span comparison");
            Check(limit_branch + 1 < mask_branch && subtract(instructions[limit_branch + 1], 5),
                  "large fallback subtracts start from mask immediately after the limit comparison");
            Check(constant(limit_branch + 2, mask_branch, 7) == effective_size - 1,
                  "large fallback materializes size minus one before the mask-span comparison");
        }
        ++guards;
    }
    Check(guards == address_calls, "all load/store/copy address guards are verified exactly once");
    std::cout << "bounds_encoding size=" << effective_size << " address_calls=" << address_calls
              << " guards=" << guards << " sub=" << subtraction_count << " end_addi=" << end_count << '\n';
}

void Accesses(Pages& pages) {
    rv::HostFeatures features{}; // RVV disabled; scalar cache both modes below.
    u64 cases{};
    for (T type : {T::U8, T::U16, T::U32, T::U64, T::V128, T::V256})
    for (bool store : {false, true}) for (bool cached : {false, true}) {
        const u64 size = ir::GetValueSizeByte(type);
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7100})};
        compile::IrBuilder as{block.get()};
        Flags(as);
        auto address = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
        if (store) {
            auto value = as.LoadUniform(type, ir::Uniform{8, type});
            as.StoreMemoryTSO(ir::Operand{address}, value);
        } else {
            auto value = as.LoadMemoryTSO(type, ir::Operand{address});
            as.StoreUniform(ir::Uniform{80, type}, value);
        }
        Mark(as); block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled code{block.get(), cached, features};
        std::cout << "bounds_group size=" << size << " store=" << store << " cache=" << cached << '\n';
        Encoding(code, size, 1);
        for (u64 mask : {u64{0}, u64{1}, u64{7}, u64{15}, u64{31}, u64{63}, u64{255}, u64{0x55}, UINT64_MAX})
        for (u64 limit : std::set<u64>{0, 1, size - 1, size, size + 1, 64, 256, UINT64_MAX}) {
            const std::set<u64> raw_values{0, 1, 7, 15, 31, 63, 64, mask, mask - 1, limit, limit - 1,
                                         UINT64_MAX, UINT64_MAX - size + 1, UINT64_MAX - size + 2};
            for (u64 raw : raw_values) for (bool anonymous : {false, true}) {
                const u64 guest = raw & mask;
                const bool accepted = OriginalAccepts(raw, mask, limit, size);
                for (u32 mode : {0u, 1u, 2u, 3u}) { // mode3 omits oracle.
                    if (!accepted && mode != 0) continue;
                    StateStorage state; Probe probe{}; probe.mode = mode;
                    Setup(state, anonymous, probe);
                    auto* physical = pages.data + 128 + (guest & 7); // Includes aligned and unaligned physical accesses.
                    std::fill(pages.data + 96, pages.data + 208, 0xa5);
                    std::array<u8, 32> source{}; for (u32 i = 0; i < size; ++i) source[i] = u8(0x40 + i);
                    std::memcpy(state.state->uniform_buffer_begin + 8, source.data(), size);
                    std::memset(state.state->uniform_buffer_begin + 80, 0xcc, size);
                    state.Put(0, raw);
                    // Bias is an integer offset, never itself dereferenced. Modular ADD maps even huge accepted guest cases to safe backing.
                    state.state->pt = reinterpret_cast<void*>(reinterpret_cast<u64>(physical) - guest);
                    state.state->guest_addr_mask = mask; state.state->guest_addr_limit = limit;
                    if (mode != 3) { state.state->interp_range_check = &Range; state.state->interp_range_check_ctx = &probe; }
                    const bool success = accepted && (mode == 0 || mode == 3);
                    const auto wanted = success ? HaltReason::CallHost : accepted && mode == 2 ? HaltReason::IllegalCode : HaltReason::PageFatal;
                    Check(code.fn(state.state) == wanted, "actual JIT mask/limit/end-wrap/oracle halt");
                    Check(probe.calls.size() == (accepted && mode != 3 ? 1u : 0u), "guard rejection suppresses oracle; effective extent has one call");
                    if (!probe.calls.empty()) Check(probe.calls.front() == std::pair{guest, size} && probe.observations_ok,
                        "oracle sees masked guest/full extent and already-published flags/location, released lease");
                    Check(state.Get(144) == (success ? marker : 0), "no post-access store after guard/oracle failure");
                    if (store) {
                        Check(std::memcmp(physical, success ? source.data() : pages.data + 96, size) == 0,
                              "store writes exactly accepted span; rejection has no partial store");
                    } else {
                        std::array<u8, 32> expected{}; expected.fill(success ? 0xa5 : 0xcc);
                        Check(std::memcmp(state.state->uniform_buffer_begin + 80, expected.data(), size) == 0,
                              "full scalar/vector/extent32 load or untouched rejected result");
                    }
                    Check(physical[-1] == 0xa5 && physical[size] == 0xa5, "memory neighbors remain unchanged");
                    Restored(state); ++cases;
                }
            }
        }
    }
    std::cout << "PASS actual JIT bounds cases=" << cases << " RVV=0 cache=0/1 widths=1/2/4/8/16/32\n";
}

void Copies(Pages& pages) {
    rv::HostFeatures features{};
    for (u64 size : {u64{0}, u64{1}, u64{8}, u64{16}, u64{32}, u64{2048}, u64{2049}, u64{4096}})
    for (bool cached : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7100})};
        compile::IrBuilder as{block.get()};
        Flags(as);
        auto destination = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
        auto source = as.LoadUniform(T::U64, ir::Uniform{8, T::U64});
        as.MemoryCopyTSO(ir::Lambda{destination}, ir::Lambda{source}, ir::Imm{size});
        Mark(as);
        block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled code{block.get(), cached, features};
        Encoding(code, size, size ? 2 : 0);
        for (u32 failure : {0u, 1u, 2u}) for (u32 mode : {0u, 1u, 2u}) for (bool anonymous : {false, true}) {
            StateStorage state; Probe probe{}; probe.mode = mode; Setup(state, anonymous, probe);
            std::fill(pages.data, pages.data + pages.page * 4, 0xa5);
            for (u32 i = 0; i < 4096; ++i) pages.data[128 + i] = u8(i ^ 0x53);
            // Keep the mask above the exclusive limit: even a one-byte invalid
            // span must remain invalid after masking instead of aliasing to zero.
            const u64 mask = pages.page * 4 - 1, limit = mask, bad = size ? limit - size + 1 : UINT64_MAX;
            const u64 dst = failure == 1 ? bad : 8192, src = failure == 2 ? bad : 128;
            state.Put(0, dst); state.Put(8, src); state.state->pt = pages.data;
            state.state->guest_addr_limit = limit; state.state->guest_addr_mask = mask;
            Check(!size || (OriginalAccepts(dst, mask, limit, size) == (failure != 1) &&
                            OriginalAccepts(src, mask, limit, size) == (failure != 2)),
                  "copy fixture addresses have the intended validity after masking");
            state.state->interp_range_check = &Range; state.state->interp_range_check_ctx = &probe;
            const bool success = !size || (!failure && mode == 0);
            const auto wanted = success ? HaltReason::CallHost : failure == 1 || mode == 1 || failure == 2 && mode == 0 ?
                HaltReason::PageFatal : HaltReason::IllegalCode;
            Check(code.fn(state.state) == wanted, "copy destination/source guards and oracle failures");
            const u32 calls = !size || failure == 1 ? 0 : mode ? 1 : failure == 2 ? 1 : 2;
            Check(probe.calls.size() == calls && probe.observations_ok, "copy checks destination then source without partial copy");
            if (calls) Check(probe.calls[0] == std::pair{dst, size}, "copy first oracle destination/full size");
            if (calls == 2) Check(probe.calls[1] == std::pair{src, size}, "copy second oracle source/full size");
            for (u32 i = 0; i < size; ++i) Check(pages.data[8192 + i] == (success ? u8(i ^ 0x53) : 0xa5), "copy bytes/no partial destination write");
            Check(state.Get(144) == (success ? marker : 0), "copy failure suppresses subsequent store"); Restored(state);
        }
    }
    std::cout << "PASS copy0/2048/2049/large source+destination and oracle failures\n";
}

struct FaultProbe {
    backend::State* state{};
    std::atomic<u64> pc{}, address{}, owned{}, reader{};
};
bool RecordFault(void* opaque, ucontext_t* context, int, siginfo_t* info) {
    auto& probe = *static_cast<FaultProbe*>(opaque);
    probe.pc.store(backend::SignalHandler::GetContextPC(context), std::memory_order_relaxed);
    probe.address.store(reinterpret_cast<u64>(info->si_addr), std::memory_order_relaxed);
    probe.owned.store(probe.state->spill_area[backend::kRiscvMemoryOwnedSlot], std::memory_order_relaxed);
    auto* part = reinterpret_cast<rv::MemoryParticipant*>(probe.state->spill_area[backend::kRiscvMemoryParticipantSlot]);
    probe.reader.store(part ? part->active.load(std::memory_order_relaxed) : rv::anonymous_memory_readers.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    return false; // Observe only; retain normal runtime recovery.
}
void NativeFaultPC(Pages& pages) {
    static_assert(std::atomic<u64>::is_always_lock_free);
    auto config = TestConfig(); backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7300})};
    compile::IrBuilder as{block.get()};
    Flags(as);
    auto address = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
    auto value = as.LoadMemoryTSO(T::U64, ir::Operand{address});
    as.StoreUniform(ir::Uniform{80, T::U64}, value);
    Mark(as);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(block.get()), "publish guardpage block");
    auto* entry = backend::TranslateIR(module, block); Check(entry != nullptr, "compile guardpage block");
    Runtime runtime{&space}; FaultProbe probe{}; probe.state = runtime.GetState();
    backend::SignalHandler::RegisterHandler(&RecordFault, &probe, 50);
    const u64 participant = runtime.GetState()->spill_area[backend::kRiscvMemoryParticipantSlot];
    for (bool anonymous : {false, true}) {
        auto* state = runtime.GetState(); state->spill_area[backend::kRiscvMemoryParticipantSlot] = anonymous ? 0 : participant;
        state->pt = pages.mapping; state->guest_addr_limit = 64; state->guest_addr_mask = 63; state->host_cpu_flags = initial_flags;
        std::memset(runtime.GetUniformBuffer().data(), 0, 160); runtime.SetLocation(0x7300);
        probe.pc.store(0); probe.owned.store(0); probe.reader.store(0);
        Check(runtime.Run() == HaltReason::PageFatal, "actual PROT_NONE native access recovers PageFatal");
        backend::FaultEntry fault{}; auto* pc = reinterpret_cast<u8*>(probe.pc.load());
        Check(pc != nullptr && space.LookupFault(pc, fault) && fault.Contains(pc) && fault.guest_loc == 0x7300 &&
              fault.owner_start == reinterpret_cast<u8*>(entry) && fault.recovery != nullptr, "observed actual fault PC resolves correct owner/guest/recovery metadata");
        Check(probe.address.load() == reinterpret_cast<u64>(pages.mapping), "observed protected-page address");
        Check(probe.owned.load() == 1 && probe.reader.load() == 1, "native fault observed while actual registered/anonymous guest lease active");
        u64 after{}; std::memcpy(&after, runtime.GetUniformBuffer().data() + 144, 8);
        Check(after == 0 && state->host_cpu_flags == published_flags && state->current_loc.Value() == 0x7300,
              "native fault preserves flags/location and suppresses post-fault store");
        auto* part = reinterpret_cast<rv::MemoryParticipant*>(participant);
        Check(part->active.load() == 0 && state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && rv::anonymous_memory_readers.load() == 0 &&
              state->spill_area[backend::kRiscvRecoveryPcSlot] == 0 && state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0,
              "native fault restores lease/counters and borrowed recovery PC/frame");
    }
    runtime.GetState()->spill_area[backend::kRiscvMemoryParticipantSlot] = participant;
    backend::SignalHandler::UnregisterHandler(&probe);
    std::cout << "PASS actual native guardpage fault PC and held lease recovery\n";
}
} // namespace
} // namespace swift::tests::riscv

int main() {
    using namespace swift::tests::riscv;
    try {
        std::cout << std::unitbuf; std::cerr << std::unitbuf;
        struct rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit); std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        Pages pages; Accesses(pages); Copies(pages); NativeFaultPC(pages);
        std::cout << "OK " << checks << " checks; host model is not ISA execution evidence\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
