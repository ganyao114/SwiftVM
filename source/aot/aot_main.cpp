//
// svm_aot -- AOT pre-compiler and runner.
//
//   svm_aot compile <guest.elf> -o <artifact> [--verbose] [--max N]
//   svm_aot run [--aot <artifact>] [--guest <guest.elf>] [-- <guest args...>]
//   svm_aot info <artifact>
//
// `compile` and `run` deliberately live in ONE executable. The validity key
// includes the identity of the SwiftVM binary that produced the code
// (backend::ComputeBuildId), because every absolute host-helper address the
// backend bakes in is an offset into that image -- two separately linked
// executables are two different code generators. A `svm_aot compile` /
// `svm_translator_linux run` split would therefore be rejected at load, and
// rightly so.
//
// `run` without --aot is the JIT baseline. It is the same runner, the same
// guest mapping and the same syscall layer as the AOT path, so a difference
// between the two runs is attributable to AOT and nothing else.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "aot/aot_compile.h"
#include "aot/aot_guest.h"
#include "aot/aot_load.h"
#include "base/logging.h"
#include "runtime/backend/address_space.h"
#include "syscalls.h"
#include "translator/x86/translator.h"

using namespace swift;

namespace {

bool InterpRangeCheckThunk(void* ctx, u64 addr, u64 size) {
    return static_cast<linux::GuestMemory*>(ctx)->RangeIsMapped(addr, size);
}

int Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  svm_aot compile <guest.elf> -o <artifact>\n"
                 "        [--verbose] [--max N] [--eager|--decode-budget N] [--sweep]\n"
                 "  svm_aot run [--aot <artifact>] [--guest <guest.elf>]\n"
                 "        [--stats] [--dump-compiles <file>] [-- <args...>]\n"
                 "  svm_aot info <artifact>\n");
    return 2;
}

// --------------------------------------------------------------------------
// run
// --------------------------------------------------------------------------
// `--dump-compiles`: every guest address the JIT still had to translate while
// the guest ran. With an artifact installed this is exactly the set AOT did
// *not* cover, which is the only honest way to measure "does the artifact
// replace run-time JIT".  A command-line flag rather than an SVM_* variable,
// for the reason in CmdRun below.
std::vector<u64>* g_compile_log = nullptr;
void CompileObserver(void*, u64 pc) {
    if (g_compile_log) {
        g_compile_log->push_back(pc);
    }
}

int RunGuest(aot::GuestImage& guest,
             const std::vector<std::string>& args,
             const std::string& artifact_path,
             const aot::AotImage* artifact,
             bool print_stats,
             const std::string& dump_compiles) {
    const VAddr guest_sp = linux::SetupInitialStack(
            guest.memory, guest.image, args, {"PATH=/usr/bin:/bin", "HOME=/root"});

    auto* instance = translator::x86::X86Instance::Make(
            reinterpret_cast<void*>(guest.memory.GetBias()),
            guest.memory.Windowed() ? guest.memory.Mask() : 0);
    instance->SetInterpRangeCheck(InterpRangeCheckThunk, &guest.memory);
    instance->GetAddressSpace()->LoadJitCache();

    std::vector<u64> compile_log;
    if (!dump_compiles.empty()) {
        g_compile_log = &compile_log;
        instance->SetCompileObserver(CompileObserver, nullptr);
    }

    if (artifact) {
        SwiftAotRuntime rt{};
        rt.struct_size = sizeof(SwiftAotRuntime);
        rt.abi_version = kSwiftAotAbiVersion;
        rt.address_space = instance->GetAddressSpace();
        rt.guest_memory_base = reinterpret_cast<void*>(guest.memory.GetBias());
        rt.guest_addr_mask = guest.memory.Windowed() ? guest.memory.Mask() : 0;
        rt.artifact_path = artifact_path.c_str();
        const int status = swift_aot_init(&rt);
        if (status != aot::kAotOk) {
            // docs/aot-design.md §2: the caller must abandon the artifact, not
            // continue on a best-effort basis. Abandoning it here means
            // failing the run: a silent fall-back to JIT would make every
            // rejection test pass for the wrong reason.
            std::fprintf(stderr,
                         "[aot] rejected artifact (status %d): %s\n",
                         status,
                         swift_aot_last_error());
            translator::x86::X86Instance::Destroy(instance);
            return 70;
        }
        if (print_stats) {
            std::fprintf(stderr, "[aot] installed %lu units\n", swift_aot_installed_units());
        }
    }

    auto* core = translator::x86::X86Core::Make(instance);
    auto& ctx = core->GetContext();
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.rip.qword = guest.image.entry;
    ctx.rsp.qword = guest_sp;
    ctx.ef.flags = 0x202;

    auto process = std::make_shared<linux::SyscallProcessState>(&guest.memory,
                                                                guest.image.brk_start);
    linux::SyscallHandler syscalls{&guest.memory,
                                   guest.image.brk_start,
                                   linux::GuestISA::kX86_64,
                                   guest.image.path,
                                   process,
                                   1000};
    syscalls.SetX86Context(&ctx);
    syscalls.SetSmcInvalidate(
            [instance](VAddr s, VAddr e) { instance->InvalidateCodeRange(s, e); });

    int exit_code = 0;
    for (;;) {
        const auto reason = core->Run();
        if (reason == translator::ExitReason::Syscall) {
            auto result = syscalls.Handle(ctx.rax.qword,
                                          ctx.rdi.qword,
                                          ctx.rsi.qword,
                                          ctx.rdx.qword,
                                          ctx.r10.qword,
                                          ctx.r8.qword,
                                          ctx.r9.qword);
            ctx.rax.qword = static_cast<u64>(result.ret);
            if (result.exited) {
                exit_code = result.exit_code;
                break;
            }
        } else if (reason == translator::ExitReason::None) {
            break;
        } else {
            std::fprintf(stderr,
                         "guest halted: reason %u rip=%#llx rax=%#llx\n",
                         static_cast<unsigned>(reason),
                         static_cast<unsigned long long>(ctx.rip.qword),
                         static_cast<unsigned long long>(ctx.rax.qword));
            exit_code = 1;
            break;
        }
    }
    if (!dump_compiles.empty()) {
        instance->SetCompileObserver(nullptr, nullptr);
        g_compile_log = nullptr;
        if (FILE* f = std::fopen(dump_compiles.c_str(), "w")) {
            for (const u64 pc : compile_log) {
                std::fprintf(f, "%llx\n", static_cast<unsigned long long>(pc));
            }
            std::fclose(f);
        }
        std::fprintf(stderr, "[aot] run-time translations: %zu\n", compile_log.size());
    }
    translator::x86::X86Core::Destroy(core);
    translator::x86::X86Instance::Destroy(instance);
    return exit_code;
}

int CmdRun(int argc, char** argv) {
    std::string artifact_path;
    std::string guest_path;
    std::vector<std::string> guest_args;
    // Every AOT knob is a command-line flag, never an environment variable:
    // backend::ComputeEnvHash() hashes every SVM_*/SWIFT_* variable by raw
    // value, so an `SVM_AOT_STATS=1` would change the validity key and reject
    // the artifact it was meant to describe.
    bool print_stats = false;
    std::string dump_compiles;
    int i = 2;
    for (; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--aot" && i + 1 < argc) {
            artifact_path = argv[++i];
        } else if (a == "--guest" && i + 1 < argc) {
            guest_path = argv[++i];
        } else if (a == "--stats") {
            print_stats = true;
        } else if (a == "--dump-compiles" && i + 1 < argc) {
            dump_compiles = argv[++i];
        } else if (a == "--") {
            ++i;
            break;
        } else {
            break;
        }
    }
    for (; i < argc; ++i) {
        guest_args.emplace_back(argv[i]);
    }
    if (artifact_path.empty() && guest_path.empty()) {
        return Usage();
    }

    std::string error;
    aot::AotImage artifact{};
    const aot::AotImage* artifact_ptr = nullptr;
    if (!artifact_path.empty()) {
        if (!aot::ReadArtifact(artifact_path, artifact, error)) {
            std::fprintf(stderr, "[aot] %s\n", error.c_str());
            return 70;
        }
        artifact_ptr = &artifact;
    }

    aot::GuestImage guest;
    if (!guest_path.empty()) {
        // Guest memory comes from the ELF. With --aot this is the strict
        // check: the artifact must agree with the image actually loaded.
        if (!guest.Load(guest_path, error)) {
            std::fprintf(stderr, "[aot] %s\n", error.c_str());
            return 70;
        }
        if (artifact_ptr) {
            const u64 live = aot::HashGuestImage(guest.segments, guest.image.entry);
            if (live != artifact.key.guest_id) {
                std::fprintf(stderr,
                             "[aot] rejected artifact: the guest image does not match the "
                             "one it was compiled from\n");
                return 70;
            }
        }
    } else {
        // Self-contained path: guest memory comes from the artifact's own
        // PT_LOAD copies, at their original guest virtual addresses.
        if (!guest.LoadFromImage(artifact, error)) {
            std::fprintf(stderr, "[aot] %s\n", error.c_str());
            return 70;
        }
    }
    if (guest_args.empty()) {
        guest_args.push_back(guest.image.path);
    }
    return RunGuest(guest, guest_args, artifact_path, artifact_ptr, print_stats, dump_compiles);
}

// --------------------------------------------------------------------------
// compile
// --------------------------------------------------------------------------
int CmdCompile(int argc, char** argv) {
    aot::CompileOptions options{};
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            options.out_path = argv[++i];
        } else if (a == "--verbose") {
            options.verbose = true;
        } else if (a == "--max" && i + 1 < argc) {
            options.max_functions = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--eager") {
            // Whole-function decoding (the pre-c0b5861 default). A flag, not
            // SVM_FUNC_LAZY=0: the validity key hashes raw SVM_* strings, so
            // the env-var route would yield an artifact that only loads in a
            // process that exports the same variable.
            options.decode_budget = 128;
        } else if (a == "--decode-budget" && i + 1 < argc) {
            options.decode_budget = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        } else if (a == "--sweep") {
            options.sweep = true;
        } else if (options.guest_elf.empty()) {
            options.guest_elf = a;
        } else {
            return Usage();
        }
    }
    if (options.guest_elf.empty() || options.out_path.empty()) {
        return Usage();
    }
    aot::AotStats stats{};
    std::string error;
    if (!aot::CompileArtifact(options, stats, error)) {
        std::fprintf(stderr, "[aot] compile failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("[aot] %s -> %s\n", options.guest_elf.c_str(), options.out_path.c_str());
    std::printf("[aot]   STT_FUNC symbols       : %u (%u with st_size == 0)\n",
                stats.symbols_seen, stats.symbols_zero_size);
    std::printf("[aot]   entry addresses tried  : %u\n", stats.addrs_attempted);
    std::printf("[aot]   units emitted          : %u\n", stats.units_emitted);
    std::printf("[aot]   not compiled           : translate=%u block-mode=%u scan=%u "
                "guest-hash=%u\n",
                stats.fail_translate, stats.fail_block_mode, stats.fail_scan,
                stats.fail_guest_hash);
    std::printf("[aot]   symbols -> failure stub: %u\n", stats.symbols_stubbed);
    if (options.sweep) {
        std::printf("[aot]   successor sweep        : %u rounds, %u addresses, %u units\n",
                    stats.sweep_rounds, stats.sweep_addrs, stats.sweep_units);
    }
    return 0;
}

// --------------------------------------------------------------------------
// info
// --------------------------------------------------------------------------
int CmdInfo(int argc, char** argv) {
    if (argc < 3) {
        return Usage();
    }
    aot::AotImage image{};
    std::string error;
    if (!aot::ReadArtifact(argv[2], image, error)) {
        std::fprintf(stderr, "[aot] %s\n", error.c_str());
        return 1;
    }
    std::printf("artifact        : %s\n", argv[2]);
    std::printf("format          : %llu (unit codec %llu)\n",
                static_cast<unsigned long long>(aot::kAotFormatVersion),
                static_cast<unsigned long long>(image.key.format_version));
    std::printf("build id        : %016llx\n",
                static_cast<unsigned long long>(image.key.build_id));
    std::printf("config hash     : %016llx\n",
                static_cast<unsigned long long>(image.key.config_hash));
    std::printf("env hash        : %016llx\n",
                static_cast<unsigned long long>(image.key.env_hash));
    std::printf("guest hash      : %016llx\n",
                static_cast<unsigned long long>(image.key.guest_id));
    std::printf("guest path      : %s\n", image.guest.path.c_str());
    std::printf("guest entry     : %#llx  brk %#llx\n",
                static_cast<unsigned long long>(image.guest.entry),
                static_cast<unsigned long long>(image.guest.brk_start));
    std::printf("guest segments  : %zu\n", image.segments.size());
    for (const auto& s : image.segments) {
        std::printf("   vaddr %#llx file %#zx mem %#llx flags %u\n",
                    static_cast<unsigned long long>(s.vaddr),
                    s.data.size(),
                    static_cast<unsigned long long>(s.memsz),
                    s.flags);
    }
    std::printf("units           : %zu  code %zu bytes  stub at +%u\n",
                image.units.size(), image.code.size(), image.stub_offset);
    std::size_t relocs = 0;
    std::size_t blocks = 0;
    for (const auto& u : image.units) {
        relocs += u.relocs.size();
        blocks += u.blocks.size();
    }
    std::printf("blocks          : %zu\nrelocations     : %zu\ndispatch slots  : %zu\n",
                blocks, relocs, image.dispatch_slots.size());
    std::printf("compile census  : symbols=%u zero-size=%u tried=%u emitted=%u "
                "translate-fail=%u block-mode=%u scan-fail=%u guest-hash-fail=%u "
                "stubbed=%u\n",
                image.stats.symbols_seen, image.stats.symbols_zero_size,
                image.stats.addrs_attempted, image.stats.units_emitted,
                image.stats.fail_translate, image.stats.fail_block_mode,
                image.stats.fail_scan, image.stats.fail_guest_hash,
                image.stats.symbols_stubbed);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "compile") {
        return CmdCompile(argc, argv);
    }
    if (cmd == "run") {
        return CmdRun(argc, argv);
    }
    if (cmd == "info") {
        return CmdInfo(argc, argv);
    }
    return Usage();
}
