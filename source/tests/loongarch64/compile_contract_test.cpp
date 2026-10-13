#include "test_support.h"

#include <memory>
#include <vector>
#include <sys/mman.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/loongarch64/jit/compile.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {

using T = ir::ValueType;

class SmcEnabledScope {
public:
    explicit SmcEnabledScope(bool enabled) : previous(backend::SmcTracker::IsEnabled()) {
        backend::SmcTracker::SetEnabled(enabled);
    }
    ~SmcEnabledScope() { backend::SmcTracker::SetEnabled(previous); }
private:
    bool previous;
};

class GuestMapping {
public:
    static constexpr size_t size = 65536;
    GuestMapping() : address(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {
        if (address == MAP_FAILED) throw std::runtime_error("compile-contract mmap failed");
    }
    ~GuestMapping() { if (address != MAP_FAILED) (void)munmap(address, size); }
    void* Data() const { return address; }
    bool Unmap() {
        if (munmap(address, size) != 0) return false;
        address = MAP_FAILED;
        return true;
    }
private:
    void* address;
};

struct CloneShape {
    ir::Value input, result, flags, logical_not;
    ir::Inst* save{};
    std::vector<ir::Inst*> siblings;

    explicit CloneShape(ir::Block* block) {
        ir::Assembler as{block};
        input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        result = as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        as.SaveFlags(result, ir::Flags::All);
        flags = as.GetFlags(result, ir::Flags::All).SetType(T::U64);
        logical_not = as.Not(input).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, result);
        as.StoreUniform(ir::Uniform{16, T::U64}, flags);
        as.StoreUniform(ir::Uniform{24, T::U64}, logical_not);
        for (auto& inst : block->GetInstList())
            if (inst.GetOp() == ir::OpCode::SaveFlags) save = &inst;
        for (auto* pseudo : result.Def()->GetPseudoOperations()) siblings.push_back(pseudo);
        Check(save && siblings.size() == 2, "clone shape contains two legal producer siblings");
    }

    void CheckLinks() const {
        const auto current = result.Def()->GetPseudoOperations();
        Check(std::vector<ir::Inst*>{current.begin(), current.end()} == siblings &&
              save->GetArg<ir::Value>(0).Def() == result.Def() &&
              flags.Def()->GetArg<ir::Value>(0).Def() == result.Def(),
              "private compilation preserves source pseudo siblings and producer inputs");
        Check(result.Def()->GetArg<ir::Value>(0).Def() == input.Def() &&
              logical_not.Def()->GetArg<ir::Value>(0).Def() == input.Def() &&
              logical_not.Def()->ArgAt(1).IsVoid(),
              "private compilation preserves the unary Not source shape");
    }

    static void Execute(void* entry) {
        for (const u64 value : {u64{0}, UINT64_MAX}) {
            StateStorage state;
            state.Put(0, value);
            for (u32 offset : {8u, 16u, 24u}) state.Put(offset, 0xdeadbeef);
            Check(CheckABI(reinterpret_cast<BlockFn>(entry), state.state) == 1,
                  "published sibling/unary Not code preserves LP64D ABI");
            Check(state.Get(8) == value + 1 && state.Get(24) == (value == 0) &&
                  (state.Get(16) & 0xf0000000) == (value == UINT64_MAX ? 0x60000000 : 0) &&
                  state.Get(16) == state.state->host_cpu_flags,
                  "published producer siblings and unary Not execute both logical outcomes");
        }
    }
};

std::vector<u16> InstructionIDs(ir::Block* block) {
    std::vector<u16> ids;
    for (auto& inst : block->GetInstList()) ids.push_back(inst.Id());
    return ids;
}

void CloneShapeContracts() {
    GuestMapping guest;
    SmcEnabledScope enabled{true};
    auto config = TestConfig();
    config.loc_end = GuestMapping::size;
    config.memory_base = guest.Data();
    config.static_program = false;
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    for (const bool missing_input : {true, false}) {
        const u64 location = missing_input ? 0x500 : 0x600;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{location})};
        ir::Assembler as{block.get()};
        const auto input = as.Zero().SetType(T::U64);
        auto* malformed = new ir::Inst(ir::OpCode::Not);
        malformed->SetReturn(T::U64);
        if (!missing_input) {
            malformed->SetArg(0, input);
            malformed->SetArg(2, ir::Imm{u64{1}});
        }
        block->AppendInstUnchecked(malformed);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        la::JitContext context;
        la::JitTranslator translator{context};
        bool rejected{};
        try { translator.Translate(block.get()); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && context.CurrentBufferSize() == 0,
              "missing-input/tail-argument unary Not fails private cloning before emission");
        rejected = false;
        try { auto lock = block->LockWrite(); (void)la::CompileBlock(module, block.get()); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && block->GetJitCache().jit_state != backend::JitState::Cached,
              "malformed unary Not cannot acquire executable cache state");
        Check(backend::IsEmpty(module->GetNode(ir::Location{location})) &&
              module->GetJitCache(ir::Location{location}) == nullptr && space.GetCodeCache(location) == nullptr &&
              space.GetCallCodeCacheTable().Lookup(location) == 0 &&
              space.GetPendingCallCodeCacheTable().Lookup(location) == 0 &&
              !space.GetSmcTracker().HasProtectedPages(),
              "malformed unary Not publishes no Module, dispatch, or SMC metadata");
    }
    {
        constexpr u64 location = 0x700;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{location})};
        CloneShape shape{block.get()};
        block->SetTerminal(ir::terminal::ReturnToHost{});
        u16 id = 50000;
        for (auto& inst : block->GetInstList()) inst.SetId(id++);
        const auto high_ids = InstructionIDs(block.get());
        for (u32 round = 0; round < 2; ++round) {
            Compiled private_code{block.get()};
            Check(private_code.translator.EmittedIR() != block.get(), "sparse homes uses a private clone");
            Check(InstructionIDs(block.get()) == high_ids, "repeated private clone preserves high source IDs");
            shape.CheckLinks();
            CloneShape::Execute(reinterpret_cast<void*>(private_code.fn));
        }
        Check(module->Push(block.get()), "legal sibling block publishes its source owner");
        void* entry{};
        { auto lock = block->LockWrite(); entry = la::CompileBlock(module, block.get()); }
        Check(entry && block->GetJitCache().jit_state == backend::JitState::Cached &&
              space.GetCodeCache(location) == entry && module->GetJitCache(ir::Location{location}) == entry,
              "CompileBlock publishes a legal sibling/unary Not unit");
        // The public block Prepare path already renumbers source IR. Only the
        // private Translate path promises to preserve the incoming high IDs.
        Check(InstructionIDs(block.get()) != high_ids, "CompileBlock retains its established source ReId behavior");
        shape.CheckLinks();
        CloneShape::Execute(entry);
    }
    {
        constexpr u64 location = 0x800;
        ir::HIRBuilder builder{1, true};
        auto* function = builder.AppendFunction(ir::Location{location});
        CloneShape shape{function->GetCurrentBlock()->GetBlock()};
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        void* entry = la::CompileFunctions(module, std::span{&function, 1});
        Check(entry && function->GetFunction()->GetJitCache().jit_state == backend::JitState::Cached &&
              backend::IsFunction(module->GetNode(ir::Location{location})) && space.GetCodeCache(location) == entry,
              "CompileFunctions publishes a legal sibling/unary Not unit");
        shape.CheckLinks();
        CloneShape::Execute(entry);
    }
    Check(space.GetSmcTracker().HasProtectedPages(), "legal clone units register SMC after publication");
}

void ForeignPseudoRejected(const std::shared_ptr<backend::Module>& module,
                          backend::AddressSpace& space, void* existing_entry) {
    const auto unpublished = [&](u64 location) {
        Check(backend::IsEmpty(module->GetNode(ir::Location{location})), "foreign pseudo rejection publishes no Module owner");
        Check(module->GetJitCache(ir::Location{location}) == nullptr && space.GetCodeCache(location) == nullptr &&
              space.GetCallCodeCacheTable().Lookup(location) == 0 &&
              space.GetPendingCallCodeCacheTable().Lookup(location) == 0,
              "foreign pseudo rejection publishes no executable cache or dispatch metadata");
        Check(!space.GetSmcTracker().HasProtectedPages(), "foreign pseudo rejection registers no SMC protection");
    };
    backend::FaultEntry old_metadata{};
    Check(module->LookupFault(static_cast<const u8*>(existing_entry), old_metadata),
          "existing entry has fault metadata before malformed compilation");
    {
        constexpr u64 location = 0x300;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{location})};
        ir::Assembler as{block.get()};
        auto producer = as.Zero().SetType(T::U64);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        producer.Def()->SetId(50000);
        auto outside = std::unique_ptr<ir::Inst>(ir::Inst::Create(ir::OpCode::Div128Remainder, producer));
        outside->SetReturn(T::U64); outside->SetId(50001);
        bool rejected{};
        try { auto lock = block->LockWrite(); (void)la::CompileBlock(module, block.get()); }
        catch (const std::runtime_error& error) {
            rejected = std::string{error.what()} == "LoongArch unit has a foreign pseudo operation";
        }
        Check(rejected && block->GetJitCache().jit_state != backend::JitState::Cached,
              "CompileBlock rejects source foreign pseudo before code and recovery publication");
        Check(producer.Def()->Id() == 50000 && outside->Id() == 50001,
              "CompileBlock foreign pseudo refusal precedes source renumbering");
        unpublished(location);
    }
    {
        constexpr u64 location = 0x400;
        ir::HIRBuilder builder{1, true};
        auto* function = builder.AppendFunction(ir::Location{location});
        auto producer = function->LoadImm<ir::U64>(ir::Imm{u64{23}});
        function->StoreUniform(ir::Uniform{0, T::U64}, producer);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        auto outside = std::unique_ptr<ir::Inst>(ir::Inst::Create(ir::OpCode::Div128Remainder, producer));
        outside->SetReturn(T::U64);
        bool rejected{};
        try { (void)la::CompileFunctions(module, std::span{&function, 1}); }
        catch (const std::runtime_error& error) {
            rejected = std::string{error.what()} == "LoongArch unit has a foreign pseudo operation";
        }
        Check(rejected && function->GetFunction()->GetJitCache().jit_state != backend::JitState::Cached,
              "CompileFunctions rejects foreign pseudo before shared cloning and metadata publication");
        unpublished(location);
    }
    backend::FaultEntry after_metadata{};
    Check(module->LookupFault(static_cast<const u8*>(existing_entry), after_metadata) &&
          after_metadata.host_start == old_metadata.host_start && after_metadata.host_end == old_metadata.host_end &&
          after_metadata.owner_start == old_metadata.owner_start && after_metadata.recovery == old_metadata.recovery &&
          after_metadata.guest_loc == old_metadata.guest_loc && after_metadata.recovery_kind == old_metadata.recovery_kind,
          "malformed block/function refusal preserves existing recovery metadata");
}

}  // namespace

void CompileContracts() {
    const auto initial_checks = checks;
    const auto previous_smc = backend::SmcTracker::IsEnabled();
    CloneShapeContracts();
    constexpr u64 rejected_location = 0x100, accepted_location = 0x200;
    {
        GuestMapping guest;
        SmcEnabledScope enabled{true};
        {
            auto config = TestConfig();
            config.loc_end = GuestMapping::size;
            config.memory_base = guest.Data();
            config.static_program = false;
            backend::AddressSpace space{config};
            auto module = space.GetDefaultModule();
            IntrusivePtr<ir::Block> existing{new ir::Block(ir::Location{rejected_location})};
            existing->SetTerminal(ir::terminal::ReturnToHost{});
            Check(module->Push(existing.get()), "compile contract publishes the existing owner");
            void* old_entry{};
            {
                // Keep the old target executable without adding an SMC page:
                // any later registration can then only come from the new owner.
                SmcEnabledScope baseline{false};
                auto write_lock = existing->LockWrite();
                old_entry = la::CompileBlock(module, existing.get());
            }
            Check(old_entry != nullptr, "compile contract has an executable old target");
            Check(!space.GetSmcTracker().HasProtectedPages(), "existing fixture has no SMC registration");
            ForeignPseudoRejected(module, space, old_entry);
            for (u32 round = 0; round < 32; ++round) {
                {
                    ir::HIRBuilder builder{1, true};
                    auto* function = builder.AppendFunction(ir::Location{rejected_location});
                    auto value = function->LoadImm<ir::U64>(ir::Imm{u64{99}});
                    function->StoreUniform(ir::Uniform{0, T::U64}, value);
                    function->EndBlock(ir::terminal::ReturnToHost{});
                    function->EndFunction();
                    bool rejected{};
                    try { (void)la::CompileFunctions(module, std::span{&function, 1}); }
                    catch (const std::runtime_error& error) {
                        rejected = std::string{error.what()} == "failed to publish LoongArch function";
                    }
                    Check(rejected, "duplicate guest entry rejects function publication");
                    Check(function->GetFunction()->GetJitCache().jit_state != backend::JitState::Cached,
                          "rejected function stays uncached");
                    Check(!space.GetSmcTracker().HasProtectedPages(), "rejected function has no SMC registration");
                }  // Rejected HIR ownership is destroyed before checking the old entry.
                Check(space.GetCodeCache(ir::Location{rejected_location}) == old_entry,
                      "rejected publication preserves the old dispatcher target");
                const auto old_node = module->GetNode(ir::Location{rejected_location});
                Check(backend::IsBlock(old_node) && backend::GetBlock(old_node).get() == existing.get(),
                      "rejected publication preserves the old module owner");
                StateStorage state;
                Check(reinterpret_cast<BlockFn>(old_entry)(state.state) == HaltReason::CallHost,
                      "old target remains executable after rejected HIR destruction");
            }
            void* entry{};
            {
                ir::HIRBuilder builder{1, true};
                auto* function = builder.AppendFunction(ir::Location{accepted_location});
                auto value = function->LoadImm<ir::U64>(ir::Imm{u64{42}});
                function->StoreUniform(ir::Uniform{0, T::U64}, value);
                function->EndBlock(ir::terminal::ReturnToHost{});
                function->EndFunction();
                entry = la::CompileFunctions(module, std::span{&function, 1});
                Check(entry != nullptr, "fresh function publication succeeds");
            }
            backend::FaultEntry fault{};
            Check(module->LookupFault(static_cast<u8*>(entry), fault) && fault.recovery,
                  "published function has recovery metadata");
            Check(space.GetCodeCache(ir::Location{accepted_location}) == entry,
                  "accepted function dispatcher target is visible");
            Check(space.GetSmcTracker().HasProtectedPages(), "accepted function is registered with SMC");
            StateStorage state;
            Check(reinterpret_cast<BlockFn>(entry)(state.state) == HaltReason::CallHost && state.Get(0) == 42,
                  "accepted function IR survives HIR builder destruction");
        }
        Check(guest.Unmap(), "compile-contract mapping is released after AddressSpace teardown");
    }
    Check(backend::SmcTracker::IsEnabled() == previous_smc, "compile contract restores the SMC mode");
    std::cout << "PASS function publication rejection/SMC/IR lifetime " << checks - initial_checks << " checks\n";
}

}  // namespace swift::tests::loongarch64
