//
// See aot_guest.h.
//

#include "aot/aot_guest.h"

#include <algorithm>
#include <cstdlib>
#include <climits>
#include <elfio/elfio.hpp>
#include "fmt/format.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/common/svm_config.h"

namespace swift::aot {

namespace {

void InstallProbes(swift::linux::GuestMemory* memory) {
    // Same wiring as source/translator/linux/main.cpp: without it a wild guest
    // pointer kills the host instead of the guest, and the JIT/AOT comparison
    // would be comparing two different failure modes.
    runtime::backend::SignalHandler::SetGuestMapProbe(
            [](void* ctx, std::uintptr_t fault_host_addr) -> bool {
                auto* mem = static_cast<swift::linux::GuestMemory*>(ctx);
                const VAddr guest =
                        mem->ToGuest(reinterpret_cast<const void*>(fault_host_addr));
                if (mem->Windowed() && guest > mem->Mask()) {
                    return false;
                }
                return mem->RangeIsMapped(guest, 1);
            },
            memory);
    runtime::backend::SignalHandler::SetGuestRangeProbe(
            [](void* ctx, std::uintptr_t host_addr, u64 length) -> u64 {
                auto* mem = static_cast<swift::linux::GuestMemory*>(ctx);
                const VAddr guest = mem->ToGuest(reinterpret_cast<const void*>(host_addr));
                if (mem->Windowed() && guest > mem->Mask()) {
                    return 0;
                }
                return mem->MappedBytesFrom(guest, length);
            },
            memory);
}

}  // namespace

bool GuestImage::ReserveWindow(std::string& error) {
    if (window_ready) {
        return true;
    }
    u32 window_bits = swift::linux::GuestMemory::kDefaultWindowBits;
    const auto& svm_config = runtime::GetSvmConfig();
    if (svm_config.guest_bits_is_set) {
        const auto& env = svm_config.guest_bits;
        const long v = std::strtol(env.c_str(), nullptr, 0);
        if (v >= 20 && v <= 47) {
            window_bits = static_cast<u32>(v);
        } else {
            // Unbounded mode is compile-gated in the CLI and has no place in
            // an AOT pipeline: with no window, a guest address and a host
            // address are indistinguishable to the relocation scanner.
            error = fmt::format("SVM_GUEST_BITS={} out of range (20..47)", env);
            return false;
        }
    }
    if (!memory.ReserveWindow(window_bits)) {
        error = fmt::format("failed to reserve the {}-bit guest window", window_bits);
        return false;
    }
    InstallProbes(&memory);
    window_ready = true;
    return true;
}

bool GuestImage::Load(const std::string& elf_path, std::string& error) {
    if (!ReserveWindow(error)) {
        return false;
    }
    // Harvest the PT_LOAD bytes first: ElfLoader consumes its own reader and
    // hands back only the derived LoadedImage.
    {
        ELFIO::elfio reader;
        if (!reader.load(elf_path)) {
            error = "cannot open guest ELF " + elf_path;
            return false;
        }
        if (reader.get_type() != ELFIO::ET_EXEC) {
            // ET_DYN guests are placed at a runtime-chosen guest base, so the
            // addresses baked into compiled code would not survive into
            // another process. Refuse rather than emit an artifact whose code
            // is valid only for one accidental placement.
            error = "AOT supports statically linked ET_EXEC guests only";
            return false;
        }
        for (ELFIO::Elf_Half i = 0; i < reader.segments.size(); ++i) {
            const auto* seg = reader.segments[i];
            if (seg->get_type() != ELFIO::PT_LOAD) {
                continue;
            }
            AotGuestSegment s{};
            s.vaddr = seg->get_virtual_address();
            s.memsz = seg->get_memory_size();
            s.flags = seg->get_flags();
            const auto filesz = static_cast<std::size_t>(seg->get_file_size());
            s.data.assign(reinterpret_cast<const u8*>(seg->get_data()),
                          reinterpret_cast<const u8*>(seg->get_data()) + filesz);
            segments.push_back(std::move(s));
        }
    }
    swift::linux::ElfLoader loader{&memory};
    image = loader.Load(elf_path);
    return true;
}

bool GuestImage::LoadFromImage(const AotImage& artifact, std::string& error) {
    if (!ReserveWindow(error)) {
        return false;
    }
    if (artifact.segments.empty()) {
        error = "artifact carries no guest segments";
        return false;
    }
    // Same span arithmetic as loader.cpp, so brk_start and the bias land in
    // the same place they would have.
    VAddr min_vaddr = UINT64_MAX;
    VAddr max_end = 0;
    for (const auto& s : artifact.segments) {
        min_vaddr = std::min(min_vaddr, s.vaddr);
        max_end = std::max(max_end, s.vaddr + s.memsz);
    }
    const VAddr span_start = swift::linux::GuestMemory::RoundDownHostPage(min_vaddr);
    const VAddr span_end = swift::linux::GuestMemory::RoundHostPage(max_end);
    if (!memory.MapImageAnywhere(span_start, span_end - span_start)) {
        error = "failed to reserve the guest image span";
        return false;
    }
    for (const auto& s : artifact.segments) {
        if (!s.data.empty()) {
            memory.WriteBytes(s.vaddr, {s.data.data(), s.data.size()});
        }
    }
    segments = artifact.segments;

    image = swift::linux::LoadedImage{};
    image.path = artifact.guest.path;
    image.isa = swift::linux::GuestISA::kX86_64;
    image.entry = artifact.guest.entry;
    image.load_bias = 0;
    image.phdr = artifact.guest.phdr;
    image.phentsize = artifact.guest.phentsize;
    image.phnum = artifact.guest.phnum;
    image.brk_start = artifact.guest.brk_start;
    if (image.brk_start != span_end) {
        error = fmt::format("artifact brk {:#x} disagrees with the segment span end {:#x}",
                            image.brk_start, span_end);
        return false;
    }
    return true;
}

}  // namespace swift::aot
