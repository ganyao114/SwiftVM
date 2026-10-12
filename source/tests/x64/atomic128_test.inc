IntrusivePtr<ir::Block> WideCasProgram(u64 pc = 0xf900, bool alignment_check = false) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{pc})}; ir::Assembler as{block.get()};
    std::array<ir::Value, 5> args;
    for (u32 i = 0; i < args.size(); ++i) args[i] = as.LoadUniform(ir::Uniform{i * 8, ValueType::U64}).SetType(ValueType::U64);
    as.AdvancePC(ir::Imm{u32{4}}); as.AdvancePC(ir::Imm{u32{4}});
    if (alignment_check) as.CheckMemoryAlignment(args[0], ir::Imm{u32{15}});
    auto old = as.CompareAndSwap128(args[0], args[1], args[2], args[3], args[4]).SetType(ValueType::V128);
    as.StoreUniform(ir::Uniform{48, ValueType::V128}, old);
    as.StoreUniform(ir::Uniform{64, ValueType::U64}, args[1]);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}

void Cas128ObservedAndRanges() {
    auto block = WideCasProgram(); Compiled compiled{block.get()};
    u64 seed = 0x1f83d9abfb41bd6bULL;
    for (u32 offset = 0; offset < 16; ++offset) for (u32 iteration = 0; iteration < 128; ++iteration) {
        std::array<u8, 64> memory{}, interpreted_memory{}; memory.fill(0xa5);
        const std::array<u64, 2> original{iteration < 4 ? u64(iteration & 1 ? UINT64_MAX : 0) : Random(seed), Random(seed)};
        const std::array<u64, 2> desired{Random(seed), Random(seed)};
        std::memcpy(memory.data() + offset, original.data(), 16); interpreted_memory = memory;
        auto mathematical = memory;
        const u32 mismatch = iteration % 3; // mismatch in either half or full match.
        if (!mismatch) std::memcpy(mathematical.data() + offset, desired.data(), 16);
        Storage actual, interpreted;
        for (auto* storage : {&actual, &interpreted}) {
            storage->Put(0, offset | 0xabcdef9800000000ULL);
            storage->Put(8, original[0] ^ (mismatch == 1)); storage->Put(16, original[1] ^ (mismatch == 2));
            storage->Put(24, desired[0]); storage->Put(32, desired[1]);
            storage->state->guest_addr_mask = 63; storage->state->guest_addr_limit = 64; storage->state->host_cpu_flags = 0xd00a5510;
        }
        actual.state->pt = memory.data(); interpreted.state->pt = interpreted_memory.data();
        backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
        Check(compiled.fn(actual.state) == wanted && wanted == HaltReason::CallHost, "CAS128 aligned/unaligned match/mismatch halt");
        Check(memory == mathematical && interpreted_memory == mathematical && actual.Get(48) == original[0] && actual.Get(56) == original[1],
              "CAS128 complete observed pair and independent snapshot compare/store");
        Check(actual.Get(64) == (original[0] ^ (mismatch == 1)) && actual.state->host_cpu_flags == 0xd00a5510, "CAS128 live alias and guest flags");
        Check(backend::unaligned_atomic_lock.load() == 0 && !backend::owns_unaligned_atomic_guard.load() &&
              actual.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && backend::riscv64::anonymous_memory_readers.load() == 0,
              "CAS128 writer release without self-reader lease");
    }
    for (u32 scenario = 0; scenario < 4; ++scenario) {
        Storage actual, interpreted; std::array<u8, 64> memory{}, other{}; memory.fill(0xa5); other = memory;
        CopyRangeProbe probe{8}, reference_probe{probe};
        const u64 address = scenario == 0 ? 64 : scenario == 1 ? 60 : scenario == 2 ? 24 : 8;
        for (auto* storage : {&actual, &interpreted}) {
            storage->Put(0, address); storage->state->guest_addr_limit = 64;
            storage->state->guest_addr_mask = scenario == 2 ? 31 : UINT64_MAX;
            storage->Put(48, 0xabc123); storage->Put(56, 0xdef456);
            if (scenario == 3) storage->state->interp_range_check = &CopyRangeCheck;
        }
        actual.state->pt = memory.data(); interpreted.state->pt = other.data();
        actual.state->interp_range_check_ctx = &probe; interpreted.state->interp_range_check_ctx = &reference_probe;
        backend::interp::Interpreter reference{*interpreted.state, block.get()}; const auto wanted = reference.Run();
        Check(compiled.fn(actual.state) == wanted && wanted == HaltReason::PageFatal, "CAS128 complete range/mask/callback preflight");
        Check(memory == other && actual.Get(48) == 0xabc123 && actual.Get(56) == 0xdef456 && actual.state->current_loc.Value() == 0xf908,
              "CAS128 preflight leaves output and memory intact with precise PC");
        Check(backend::unaligned_atomic_lock.load() == 0 && !probe.entered_with_lease, "CAS128 invalid range never owns writer/reader");
    }
    auto checked = WideCasProgram(0xf920, true); Compiled checked_native{checked.get()}; Storage misaligned; misaligned.Put(0, 1);
    Check(checked_native.fn(misaligned.state) == HaltReason::PageFatal, "CAS128 frontend alignment requirement remains explicit");
    std::cout << "PASS software CAS128 full observed pair, unaligned aliases, flags and range preflight\n";
}

// Call a function with sentinels in every SysV nonvolatile. A fault may abandon
// several nested C++ spill frames, so check the host-visible register values.
extern "C" u64 SwiftTestNonvolatile(HaltReason (*function)(void*), void* argument);
asm(R"(
.text
.global SwiftTestNonvolatile
.type SwiftTestNonvolatile,@function
SwiftTestNonvolatile:
 push %rbx
 push %rbp
 push %r12
 push %r13
 push %r14
 push %r15
 sub $8,%rsp
 mov %rdi,%rax
 mov %rsi,%rdi
 mov $0x12345671,%ebx
 mov $0x12345672,%ebp
 mov $0x12345673,%r12d
 mov $0x12345674,%r13d
 mov $0x12345675,%r14d
 mov $0x12345676,%r15d
 call *%rax
 mov %eax,%r10d
 cmp $0x12345671,%rbx
 jne 1f
 cmp $0x12345672,%rbp
 jne 1f
 cmp $0x12345673,%r12
 jne 1f
 cmp $0x12345674,%r13
 jne 1f
 cmp $0x12345675,%r14
 jne 1f
 cmp $0x12345676,%r15
 jne 1f
 mov $1,%eax
 shl $32,%rax
 or %r10,%rax
 jmp 2f
1:
 mov %r10,%rax
2:
 add $8,%rsp
 pop %r15
 pop %r14
 pop %r13
 pop %r12
 pop %rbp
 pop %rbx
 ret
.size SwiftTestNonvolatile,.-SwiftTestNonvolatile
)");
HaltReason CasRuntimeCall(void* pointer) { return static_cast<Runtime*>(pointer)->Run(); }

void Cas128ActualFaultsAndReadonlyMismatch() {
    auto config = TestConfig(); backend::AddressSpace space{config}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
    auto block = WideCasProgram(0xfa00); Check(module->Push(block.get()) && backend::TranslateIR(module, block), "CAS128 publish faultable helper block");
    auto* allocation = static_cast<u8*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(allocation != MAP_FAILED, "CAS128 allocate actual fault pages");
    runtime.GetState()->guest_addr_limit = UINT64_MAX;
    const std::array<u64, 2> original{0x8877665544332211,0xfedcba9876543210};
    std::memcpy(allocation, original.data(), 16); std::memcpy(allocation + 4096 - 8, original.data(), 8);
    for (u32 scenario = 0; scenario < 3; ++scenario) {
        Check(mprotect(allocation + 4096, 4096, PROT_NONE) == 0 && mprotect(allocation, 4096, scenario == 2 ? PROT_READ : PROT_READ | PROT_WRITE) == 0,
              "CAS128 source/read-only protection");
        const u64 address = reinterpret_cast<u64>(allocation + (scenario == 0 ? 4096 : scenario == 1 ? 4096 - 8 : 0));
        const std::array<u64, 5> args{address,original[0],original[1],0x1111222233334444,0x9999aaaabbbbcccc};
        std::memcpy(runtime.GetUniformBuffer().data(), args.data(), sizeof(args));
        const std::array<u64, 2> old_output{0xabc123,0xdef456}; std::memcpy(runtime.GetUniformBuffer().data() + 48, old_output.data(), 16);
        runtime.GetState()->host_cpu_flags = 0xd00a5510; runtime.SetLocation(0xfa00);
        const auto old_mxcsr = _mm_getcsr(), changed = (old_mxcsr & ~0x6000u) | 0x6000u;
        _mm_setcsr(changed); const u64 outcome = SwiftTestNonvolatile(&CasRuntimeCall, &runtime); const auto restored = _mm_getcsr(); _mm_setcsr(old_mxcsr);
        Check(outcome == (u64{1} << 32) + u32(HaltReason::PageFatal), "CAS128 abandoned writer helper restores every SysV nonvolatile");
        Check(runtime.GetLocation() == 0xfa08 && restored == changed && runtime.GetState()->host_cpu_flags == 0xd00a5510, "CAS128 actual fault precise PC, FP environment and guest flags");
        std::array<u64, 2> output{}; std::memcpy(output.data(), runtime.GetUniformBuffer().data() + 48, 16);
        Check(output == old_output, "CAS128 partial read/write fault does not publish observed result");
        Check(backend::unaligned_atomic_lock.load() == 0 && !backend::owns_unaligned_atomic_guard.load() && backend::riscv64::anonymous_memory_readers.load() == 0,
              "CAS128 abandoned writer releases lock without reader leak");
    }
    // Shared IR mismatch is read-only: it must succeed on read-only memory.
    const u64 address = reinterpret_cast<u64>(allocation);
    const std::array<u64, 5> mismatch_args{address,original[0] ^ 1,original[1],0,0};
    std::memcpy(runtime.GetUniformBuffer().data(), mismatch_args.data(), sizeof(mismatch_args)); runtime.SetLocation(0xfa00);
    Check(SwiftTestNonvolatile(&CasRuntimeCall, &runtime) == (u64{1} << 32) + u32(HaltReason::CallHost), "CAS128 readonly mismatch does not attempt store");
    Check(std::memcmp(allocation, original.data(), 16) == 0, "CAS128 readonly mismatch leaves memory unchanged");
    Check(mprotect(allocation, 8192, PROT_READ | PROT_WRITE) == 0, "CAS128 restore pages");
    const std::array<u64, 5> match_args{address,original[0],original[1],17,19};
    std::memcpy(runtime.GetUniformBuffer().data(), match_args.data(), sizeof(match_args)); runtime.SetLocation(0xfa00);
    Check(runtime.Run() == HaltReason::CallHost, "CAS128 Runtime and writer protocol reusable after real faults");
    munmap(allocation, 8192);
    std::cout << "PASS CAS128 real read/write faults, readonly mismatch, full SysV ABI and writer recovery\n";
}

void Cas128CrossPageMatchingFaults() {
    const auto page = size_t(sysconf(_SC_PAGESIZE));
    auto* allocation = static_cast<u8*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(allocation != MAP_FAILED, "CAS128 cross-page allocate native fault fixture");
    const std::array<u64, 2> original{0x8877665544332211ULL, 0xfedcba9876543210ULL};
    const std::array<u64, 2> desired{0x1020304050607080ULL, 0x123456789abcdef0ULL};
    for (bool biased : {false, true}) {
        auto config = TestConfig(); config.memory_base = biased ? allocation + 1 : nullptr;
        backend::AddressSpace space{config}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        auto block = WideCasProgram(0xfd00);
        Check(module->Push(block.get()) && backend::TranslateIR(module, block), "CAS128 cross-page publish native block");
        runtime.GetState()->guest_addr_limit = biased ? 2 * page - 1 : UINT64_MAX;
        CopyRangeProbe probe{UINT64_MAX};
        runtime.GetState()->interp_range_check = &CopyRangeCheck;
        runtime.GetState()->interp_range_check_ctx = &probe;
        for (u32 overlap = 1; overlap < 16; ++overlap) {
            Check(mprotect(allocation, 2 * page, PROT_READ | PROT_WRITE) == 0, "CAS128 cross-page reset pages");
            auto* address = allocation + page - overlap;
            std::memset(address - 8, 0xa5, 32); std::memcpy(address, original.data(), 16);
            std::array<u8, 32> before{}; std::memcpy(before.data(), address - 8, before.size());
            const u64 guest = biased ? page - overlap - 1 : reinterpret_cast<u64>(address);
            const std::array<u64, 5> args{guest, original[0], original[1], desired[0], desired[1]};
            std::memcpy(runtime.GetUniformBuffer().data(), args.data(), sizeof(args));
            const std::array<u64, 2> old_output{0xabc123, 0xdef456};
            std::memcpy(runtime.GetUniformBuffer().data() + 48, old_output.data(), 16);
            const u64 old_flags = 0x9abcfed0d00a5510ULL;
            runtime.GetState()->host_cpu_flags = old_flags;
            probe.calls = 0; probe.entered_with_lease = false; runtime.SetLocation(0xfd00);
            Check(mprotect(allocation + page, page, PROT_READ) == 0, "CAS128 cross-page protect second page read-only");
            const u32 old_mxcsr = _mm_getcsr(), changed = (old_mxcsr | 0x9fc1u) & ~0x6000u;
            const u32 preset = changed | ((overlap & 3) << 13);
            _mm_setcsr(preset);
            const u64 outcome = SwiftTestNonvolatile(&CasRuntimeCall, &runtime);
            const u32 after = _mm_getcsr(); _mm_setcsr(old_mxcsr);
            const auto detail = "CAS128 matching cross-page/" + std::to_string(biased) + "/" + std::to_string(overlap);
            Check(outcome == (u64{1} << 32) + u32(HaltReason::PageFatal), detail + " actual fault and full SysV ABI");
            Check(std::memcmp(before.data(), address - 8, before.size()) == 0, detail + " all 16 bytes and guards intact");
            Check(runtime.GetLocation() == 0xfd08 && after == preset && runtime.GetState()->host_cpu_flags == old_flags,
                  detail + " precise PC/MXCSR/guest flags");
            Check(std::memcmp(runtime.GetUniformBuffer().data() + 48, old_output.data(), 16) == 0,
                  detail + " observed result not published on fault");
            Check(probe.calls == 1 && !probe.entered_with_lease, detail + " mapped-range callback does not imply write permission");
            Check(backend::unaligned_atomic_lock.load() == 0 && !backend::owns_unaligned_atomic_guard.load() &&
                  backend::riscv64::anonymous_memory_readers.load() == 0 &&
                  runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
                  detail + " abandoned writer/readers released");
            Check(mprotect(allocation + page, page, PROT_READ | PROT_WRITE) == 0, detail + " restore writable second page");
            runtime.SetLocation(0xfd00);
            Check(SwiftTestNonvolatile(&CasRuntimeCall, &runtime) == (u64{1} << 32) + u32(HaltReason::CallHost),
                  detail + " runtime/writer reusable after fault");
            Check(std::memcmp(address, desired.data(), 16) == 0 &&
                  std::memcmp(runtime.GetUniformBuffer().data() + 48, original.data(), 16) == 0,
                  detail + " successful retry publishes complete pair");
            // The same original expected pair now mismatches. No write intent
            // is permitted, even though the host address crosses a RO page.
            Check(mprotect(allocation + page, page, PROT_READ) == 0, detail + " protect mismatch page");
            runtime.SetLocation(0xfd00);
            Check(runtime.Run() == HaltReason::CallHost && std::memcmp(address, desired.data(), 16) == 0 &&
                  std::memcmp(runtime.GetUniformBuffer().data() + 48, desired.data(), 16) == 0,
                  detail + " read-only cross-page mismatch remains read-only");
        }
    }
    munmap(allocation, 2 * page);
    std::cout << "PASS CAS128 matching cross-page protection faults: all 15 offsets / direct and unaligned bias / intact memory / ABI and reuse\n";
}

void Cas128InterpreterCrossPageMatchingFaults() {
    // Interpreter::Run has no generated recovery frame. Test its real store
    // fault in a subprocess with shared pages, allowing the parent to inspect
    // memory without inventing an unsupported interpreter signal recovery.
    const auto page = size_t(sysconf(_SC_PAGESIZE));
    auto* allocation = static_cast<u8*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    Check(allocation != MAP_FAILED, "CAS128 interpreter allocate shared cross-page probe");
    auto block = WideCasProgram(0xfe00);
    const std::array<u64, 2> original{0x8877665544332211ULL, 0xfedcba9876543210ULL};
    const std::array<u64, 2> desired{0x1020304050607080ULL, 0x123456789abcdef0ULL};
    for (u32 overlap = 1; overlap < 16; ++overlap) {
        auto* address = allocation + page - overlap;
        std::memset(address - 8, 0x5a, 32); std::memcpy(address, original.data(), 16);
        std::array<u8, 32> before{}; std::memcpy(before.data(), address - 8, before.size());
        Storage storage;
        const std::array<u64, 5> args{reinterpret_cast<u64>(address), original[0], original[1], desired[0], desired[1]};
        std::memcpy(storage.state->uniform_buffer_begin, args.data(), sizeof(args));
        const auto child = fork(); Check(child >= 0, "CAS128 interpreter fork actual fault probe");
        if (child == 0) {
            const rlimit no_core{0, 0}; setrlimit(RLIMIT_CORE, &no_core);
            std::signal(SIGSEGV, SIG_DFL); std::signal(SIGBUS, SIG_DFL);
            if (mprotect(allocation + page, page, PROT_READ) != 0) _exit(20);
            backend::interp::Interpreter reference{*storage.state, block.get()};
            (void)reference.Run();
            _exit(21); // A matching store must actually fault.
        }
        int status{};
        Check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
              "CAS128 interpreter matching cross-page store takes actual SIGSEGV");
        Check(std::memcmp(before.data(), address - 8, before.size()) == 0,
              "CAS128 interpreter fault leaves whole pair and adjacent bytes intact/" + std::to_string(overlap));
        // Parent permissions stayed writable; this is a fresh ordinary call,
        // not a claim of recovering the faulted child interpreter frame.
        backend::interp::Interpreter reference{*storage.state, block.get()};
        Check(reference.Run() == HaltReason::CallHost && std::memcmp(address, desired.data(), 16) == 0,
              "CAS128 interpreter normal matching write after subprocess probe");
        Check(backend::unaligned_atomic_lock.load() == 0 && !backend::owns_unaligned_atomic_guard.load(),
              "CAS128 interpreter normal writer releases guard");
    }
    munmap(allocation, 2 * page);
    std::cout << "PASS x64-host interpreter CAS128 cross-page fault memory integrity: all 15 offsets in subprocesses (no interpreter recovery claim)\n";
}

void Cas128ContentionAndReaders() {
    auto block = WideCasProgram(); Compiled cas{block.get()};
    IntrusivePtr<ir::Block> add_block{new ir::Block(ir::Location{0xfb00})}; ir::Assembler add_as{add_block.get()};
    auto address = add_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto one = add_as.LoadImm(ir::Imm{u64{1}}).SetType(ValueType::U64);
    (void)add_as.AtomicFetchAdd(address, one).SetType(ValueType::U64); add_block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled increment{add_block.get()};
    alignas(16) std::array<u64, 2> memory{};
    constexpr u32 count = 1000; std::atomic<bool> start{}, failed{};
    std::thread writer([&] {
        Storage storage; storage.state->pt = memory.data(); storage.state->guest_addr_limit = 16; storage.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        u64 low{}, high{};
        for (u32 iteration = 0; iteration < count;) {
            storage.Put(8, low); storage.Put(16, high); storage.Put(24, low + 1); storage.Put(32, high + 1);
            storage.state->halt_reason = HaltReason::None;
            if (cas.fn(storage.state) != HaltReason::CallHost) { failed = true; break; }
            const auto seen_low = storage.Get(48), seen_high = storage.Get(56);
            if (seen_low == low && seen_high == high) { ++iteration; ++low; ++high; } else { low = seen_low; high = seen_high; }
        }
    });
    std::thread adder([&] {
        Storage storage; storage.state->pt = memory.data(); storage.state->guest_addr_limit = 16; storage.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (u32 i = 0; i < count; ++i) { storage.state->halt_reason = HaltReason::None; if (increment.fn(storage.state) != HaltReason::CallHost) failed = true; }
    });
    start.store(true, std::memory_order_release); writer.join(); adder.join();
    Check(!failed && memory[0] == 2 * count && memory[1] == count, "software CAS128 and native LOCK XADD no lost updates");
    IntrusivePtr<ir::Block> read_block{new ir::Block(ir::Location{0xfc00})}; ir::Assembler read_as{read_block.get()};
    auto read_address = read_as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto pair = read_as.LoadMemory(ir::Operand{read_address}).SetType(ValueType::V128);
    read_as.StoreUniform(ir::Uniform{48, ValueType::V128}, pair); read_block->SetTerminal(ir::terminal::ReturnToHost{}); Compiled read{read_block.get()};
    memory = {}; start = false; failed = false; std::atomic<bool> done{};
    std::thread publisher([&] {
        Storage storage; storage.state->pt = memory.data(); storage.state->guest_addr_limit = 16; storage.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (u32 i = 0; i < count; ++i) {
            storage.Put(8, i); storage.Put(16, i); storage.Put(24, i + 1); storage.Put(32, i + 1); storage.state->halt_reason = HaltReason::None;
            if (cas.fn(storage.state) != HaltReason::CallHost || storage.Get(48) != i || storage.Get(56) != i) failed = true;
        }
        done.store(true, std::memory_order_release);
    });
    std::thread observer([&] {
        Storage storage; storage.state->pt = memory.data(); storage.state->guest_addr_limit = 16; storage.Put(0, 0);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        u32 samples{};
        do {
            storage.state->halt_reason = HaltReason::None;
            const auto halt = samples % 2 ? backend::interp::Interpreter{*storage.state, read_block.get()}.Run() : read.fn(storage.state);
            if (halt != HaltReason::CallHost || storage.Get(48) != storage.Get(56)) failed = true;
            ++samples;
        } while (!done.load(std::memory_order_acquire) || samples < count);
    });
    start.store(true, std::memory_order_release); publisher.join(); observer.join();
    Check(!failed, "native/interpreter V128 readers never see torn software CAS128 publication");
    Check(backend::unaligned_atomic_lock.load() == 0 && backend::riscv64::anonymous_memory_readers.load() == 0, "CAS128 contention leaves writer/readers clear");
    std::cout << "PASS software CAS128 overlaps native RMW and protects native/interpreter vector readers\n";
}
