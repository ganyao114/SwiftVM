IntrusivePtr<ir::Block> CopyProgram(u64 size, bool tso, u32 lambda_mode, u64 destination, u64 source, u64 pc = 0xf000) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{pc})}; ir::Assembler as{block.get()};
    auto dst = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto src = as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    auto marker = as.LoadImm(ir::Imm{u64{0x72134786}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{16, ValueType::U64}, marker);
    as.AdvancePC(ir::Imm{u32{4}}); as.AdvancePC(ir::Imm{u32{4}});
    auto dst_lambda = lambda_mode & 1 ? ir::Lambda{dst} : ir::Lambda{ir::Imm{destination}};
    auto src_lambda = lambda_mode & 2 ? ir::Lambda{src} : ir::Lambda{ir::Imm{source}};
    if (tso) as.MemoryCopyTSO(dst_lambda, src_lambda, ir::Imm{size}); else as.MemoryCopy(dst_lambda, src_lambda, ir::Imm{size});
    auto tail = as.LoadImm(ir::Imm{u64{0x83145697}}).SetType(ValueType::U64);
    as.StoreUniform(ir::Uniform{24, ValueType::U64}, tail);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}

void MemoryCopyOverlapAndLambdas() {
    constexpr std::array<u64, 24> sizes{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,31,32,63,64,127,129};
    for (bool tso : {false, true}) for (u32 mode = 0; mode < 4; ++mode) for (u64 size : sizes)
    for (s32 delta : {-33,-1,0,1,7,33,193}) {
        constexpr u64 source = 193;
        const u64 destination = u64(s64(source) + delta);
        const u64 poisoned_source = source | 0xFEDCBA9800000000ULL;
        const u64 poisoned_destination = destination | 0xABCD567800000000ULL;
        auto block = CopyProgram(size, tso, mode, poisoned_destination, poisoned_source);
        Compiled compiled{block.get()};
        Storage actual, interpreted;
        std::array<u8, 1024> memory{}, reference_memory{}, mathematical{};
        for (u32 i = 0; i < memory.size(); ++i) memory[i] = u8(i * 37 + i / 7 + 13);
        reference_memory = mathematical = memory;
        const auto snapshot = mathematical;
        for (u32 i = 0; i < size; ++i) mathematical[destination + i] = snapshot[source + i];
        for (auto* storage : {&actual, &interpreted}) {
            storage->Put(0, poisoned_destination); storage->Put(8, poisoned_source);
            storage->state->guest_addr_mask = 1023; storage->state->guest_addr_limit = 1024;
            storage->state->host_cpu_flags = 0xb00a5510;
        }
        actual.state->pt = memory.data(); interpreted.state->pt = reference_memory.data();
        backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
        Check(compiled.fn(actual.state) == wanted && wanted == HaltReason::CallHost, "copy immediate/value lambda and overlap halt");
        Check(memory == mathematical && reference_memory == mathematical, "copy snapshot model full memory, all small widths and REP overlap directions");
        Check(actual.Get(16) == 0x72134786 && actual.Get(24) == 0x83145697, "copy surrounding architectural stores");
        Check(actual.state->host_cpu_flags == 0xb00a5510, "copy guest flags preserved");
        Check(actual.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && backend::riscv64::anonymous_memory_readers.load() == 0,
              "copy completes exactly one memory reader lease");
    }
    std::cout << "PASS memory copy all lambda forms, odd small sizes, mask pollution and memmove overlap\n";
}

struct CopyRangeProbe {
    u64 reject{};
    u32 calls{};
    bool entered_with_lease{};
};
bool CopyRangeCheck(void* pointer, u64 address, u64) {
    auto& probe = *static_cast<CopyRangeProbe*>(pointer);
    ++probe.calls; probe.entered_with_lease |= backend::riscv64::anonymous_memory_readers.load() != 0;
    return address != probe.reject;
}
void MemoryCopyPreflightAndZero() {
    for (bool tso : {false, true}) for (u32 mode = 0; mode < 4; ++mode) {
        for (const auto& item : std::array<std::array<u64, 3>, 8>{{
                 {20,1024,8}, {1024,20,8}, {20,1020,8}, {1020,20,8},
                 {20,250,8}, {250,20,8}, {20,10,UINT64_MAX}, {UINT64_MAX,UINT64_MAX,0}}}) {
            const auto destination = item[0], source = item[1], size = item[2];
            auto block = CopyProgram(size, tso, mode, destination, source); Compiled compiled{block.get()};
            Storage actual, interpreted;
            std::array<u8, 1024> memory{}, other{};
            memory.fill(0x5a); other = memory; const auto original = memory;
            const bool crossing_mask = size == 8 && (source == 250 || destination == 250);
            for (auto* storage : {&actual, &interpreted}) {
                storage->Put(0, destination); storage->Put(8, source);
                storage->state->guest_addr_mask = crossing_mask ? 255 : UINT64_MAX;
                storage->state->guest_addr_limit = 1024;
            }
            actual.state->pt = memory.data(); interpreted.state->pt = other.data();
            backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
            Check(compiled.fn(actual.state) == wanted && wanted == (size ? HaltReason::PageFatal : HaltReason::CallHost), "copy whole-range and mask-wrap rejection/zero no access");
            Check(memory == original && other == original, "copy both ranges validate before mutation");
            Check(actual.state->current_loc.Value() == (size ? 0xf008 : 0xf000), "copy preflight precise third instruction PC");
            Check(actual.Get(16) == 0x72134786 && actual.Get(24) == (size ? 0 : 0x83145697), "copy rejection skips following store");
            Check(actual.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && backend::riscv64::anonymous_memory_readers.load() == 0,
                  "copy failed preflight or zero size admits no memory reader");
        }
        for (bool reject_source : {false, true}) {
            auto block = CopyProgram(33, tso, mode, 100, 20); Compiled compiled{block.get()};
            Storage actual, interpreted; std::array<u8, 256> memory{}, other{}; memory.fill(0x6b); other = memory;
            CopyRangeProbe probe{reject_source ? 20u : 100u}, reference_probe{probe};
            for (auto* storage : {&actual, &interpreted}) {
                storage->Put(0, 100); storage->Put(8, 20); storage->state->guest_addr_limit = 256;
                storage->state->interp_range_check = &CopyRangeCheck;
            }
            actual.state->pt = memory.data(); interpreted.state->pt = other.data();
            actual.state->interp_range_check_ctx = &probe; interpreted.state->interp_range_check_ctx = &reference_probe;
            backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
            Check(compiled.fn(actual.state) == wanted && wanted == HaltReason::PageFatal, "copy source/destination range callback rejection");
            Check(probe.calls == (reject_source ? 2u : 1u) && !probe.entered_with_lease, "copy callbacks precede reader admission");
            Check(memory == other && memory[100] == 0x6b && actual.state->current_loc.Value() == 0xf008, "copy callback rejection leaves destination and PC intact");
        }
    }
    std::cout << "PASS memory copy bounds, mask wrap, callbacks and zero-length preflight\n";
}

void MemoryCopyRealFaultRecovery() {
    auto config = TestConfig(); backend::AddressSpace space{config}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
    auto* allocation = static_cast<u8*>(mmap(nullptr, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(allocation != MAP_FAILED, "copy allocate actual fault fixture");
    std::fill(allocation, allocation + 3 * 4096, u8{0x7c});
    Check(mprotect(allocation + 4096, 4096, PROT_NONE) == 0, "copy protect fault page");
    runtime.GetState()->guest_addr_limit = UINT64_MAX;
    for (bool tso : {false, true}) for (u64 size : {1u,3u,9u,16u,17u,64u}) for (bool write_fault : {false, true}) {
        const u64 destination = reinterpret_cast<u64>(allocation + (write_fault ? 4096 : 8192));
        const u64 source = reinterpret_cast<u64>(allocation + (write_fault ? 0 : 4096 - (size <= 16 ? size / 2 : 0)));
        const u64 pc = 0xf100 + (tso ? 0x100 : 0) + size * 8 + write_fault * 4;
        auto block = CopyProgram(size, tso, 3, destination, source, pc);
        Check(module->Push(block.get()) && backend::TranslateIR(module, block), "copy compile registered native read/write fault block");
        std::memcpy(runtime.GetUniformBuffer().data(), &destination, 8); std::memcpy(runtime.GetUniformBuffer().data() + 8, &source, 8);
        std::memset(runtime.GetUniformBuffer().data() + 16, 0, 16);
        runtime.SetLocation(pc);
        const auto old_mxcsr = _mm_getcsr(), changed = (old_mxcsr & ~0x6000u) | 0x6000u;
        _mm_setcsr(changed); const auto halt = runtime.Run(); const auto restored = _mm_getcsr(); _mm_setcsr(old_mxcsr);
        Check(halt == HaltReason::PageFatal && runtime.GetLocation() == pc + 8, "copy actual SIGSEGV retains precise PC");
        Check(restored == changed, "copy actual fault restores host MXCSR");
        u64 marker{}, skipped{}; std::memcpy(&marker, runtime.GetUniformBuffer().data() + 16, 8); std::memcpy(&skipped, runtime.GetUniformBuffer().data() + 24, 8);
        Check(marker == 0x72134786 && skipped == 0, "copy actual fault preserves committed state and skips tail");
        Check(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && backend::riscv64::anonymous_memory_readers.load() == 0,
              "copy SIGSEGV releases its single reader lease");
        if (!write_fault && size <= 16) Check(std::all_of(allocation + 8192, allocation + 8192 + 32, [](u8 byte) { return byte == 0x7c; }),
                                            "copy small source snapshot faults before any destination mutation");
    }
    // A backwards overlapping REP faults after STD. Recovery must restore DF
    // before its C++ epilogue call and before returning to ordinary host code.
    for (bool write_fault : {false, true}) {
        Check(mprotect(allocation + 4096, 4096, write_fault ? PROT_READ : PROT_NONE) == 0, "copy backwards fault protection");
        const u64 source = reinterpret_cast<u64>(allocation + 4096 - 32), destination = source + 1;
        const u64 pc = 0xf700 + write_fault * 0x20;
        auto block = CopyProgram(64, true, 3, destination, source, pc);
        Check(module->Push(block.get()) && backend::TranslateIR(module, block), "copy register backwards REP fault block");
        std::memcpy(runtime.GetUniformBuffer().data(), &destination, 8); std::memcpy(runtime.GetUniformBuffer().data() + 8, &source, 8);
        runtime.SetLocation(pc); const auto halt = runtime.Run();
        u64 flags{}; asm volatile("pushfq; popq %0" : "=r"(flags));
        Check(halt == HaltReason::PageFatal && runtime.GetLocation() == pc + 8 && !(flags & (1u << 10)), "copy backwards fault clears host direction flag");
        Check(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && backend::riscv64::anonymous_memory_readers.load() == 0,
              "backwards copy faults release reader lease");
    }
    Check(mprotect(allocation + 4096, 4096, PROT_READ | PROT_WRITE) == 0, "copy restore fixture protection");
    const u64 source = reinterpret_cast<u64>(allocation), destination = source + 8192;
    auto recovery = CopyProgram(64, false, 3, destination, source, 0xf800);
    Check(module->Push(recovery.get()) && backend::TranslateIR(module, recovery), "copy register post-fault reuse block");
    std::memcpy(runtime.GetUniformBuffer().data(), &destination, 8); std::memcpy(runtime.GetUniformBuffer().data() + 8, &source, 8);
    runtime.SetLocation(0xf800); Check(runtime.Run() == HaltReason::CallHost, "copy Runtime reusable after read/write/backwards faults");
    munmap(allocation, 3 * 4096);
    std::cout << "PASS copy real source/destination SIGSEGV, precise PC, MXCSR, reader recovery and backwards DF\n";
}

void MemoryCopyWriterGate() {
    auto block = CopyProgram(64, true, 3, 128, 0); Compiled compiled{block.get()};
    Storage actual; std::array<u8, 256> memory{}; for (u32 i = 0; i < memory.size(); ++i) memory[i] = u8(i);
    actual.Put(0, 128); actual.Put(8, 0); actual.state->pt = memory.data(); actual.state->guest_addr_limit = 256;
    std::atomic<bool> started{}, finished{}; HaltReason reason{};
    std::thread reader; bool blocked{};
    {
        backend::UnalignedAtomicGuard writer;
        reader = std::thread([&] { started.store(true, std::memory_order_release); reason = compiled.fn(actual.state); finished.store(true, std::memory_order_release); });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        blocked = !finished.load(std::memory_order_acquire);
    }
    reader.join();
    Check(blocked, "copy admission waits behind unaligned atomic writer");
    Check(reason == HaltReason::CallHost && std::equal(memory.begin(), memory.begin() + 64, memory.begin() + 128), "copy executes after writer releases shared gate");
    Check(backend::riscv64::anonymous_memory_readers.load() == 0 && actual.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
          "copy writer gate leaves no readers");
    std::cout << "PASS copy participates in shared unaligned atomic writer gate\n";
}
