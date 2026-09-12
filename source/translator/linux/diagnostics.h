#include "base/logging.h"
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif
#include "guest_memory.h"
#include "syscalls.h"

namespace swift::linux {

// Diagnostic reads must tolerate PROT_NONE and mappings disappearing. The OS
// copies the bytes without turning an inaccessible watched address into a host
// fault. Callers serialize guest remaps with SyscallProcessState::memory_mutex.
inline bool ReadGuestBytes(GuestMemory& memory, VAddr address, std::span<u8> out) {
    if (out.empty()) return true;
    if (!memory.RangeIsMapped(address, out.size())) return false;
    const auto* source = memory.ToHostConst(address);
#if defined(__APPLE__)
    mach_vm_size_t copied{};
    return mach_vm_read_overwrite(mach_task_self(),
                                  reinterpret_cast<mach_vm_address_t>(source),
                                  out.size(),
                                  reinterpret_cast<mach_vm_address_t>(out.data()),
                                  &copied) == KERN_SUCCESS && copied == out.size();
#elif defined(__linux__)
    iovec local{out.data(), out.size()};
    iovec remote{const_cast<void*>(source), out.size()};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(out.size());
#else
    return false;
#endif
}

class GuestMemoryWatch final {
public:
    GuestMemoryWatch(GuestMemory& memory,
                     std::shared_ptr<SyscallProcessState> process,
                     VAddr address, s64 tid, VAddr rip)
            : memory(memory), process(std::move(process)), address(address),
              tid(tid), last_rip(rip), worker([this](std::stop_token stop) { Run(stop); }) {}

    ~GuestMemoryWatch() { Stop(); }

    void SetLocation(VAddr rip) { last_rip.store(rip, std::memory_order_relaxed); }

    void Stop() {
        worker.request_stop();
        if (worker.joinable()) worker.join();
    }

private:
    void Run(std::stop_token stop) {
        std::array<u64, 8> previous{};
        bool have_previous = false;
        while (!stop.stop_requested()) {
            std::array<u64, 8> now{};
            bool readable;
            {
                std::lock_guard guard(process->memory_mutex);
                readable = ReadGuestBytes(memory, address,
                        {reinterpret_cast<u8*>(now.data()), sizeof(now)});
            }
            if (readable && have_previous) {
                for (size_t i = 0; i < now.size(); ++i) {
                    if (now[i] == previous[i]) continue;
                    SVM_DIAG_PRINT(Syscall,
                                 "[watch] tid=%lld +%zu: %#llx -> %#llx last_rip=%#llx\n",
                                 static_cast<long long>(tid), i * sizeof(u64),
                                 static_cast<unsigned long long>(previous[i]),
                                 static_cast<unsigned long long>(now[i]),
                                 static_cast<unsigned long long>(
                                         last_rip.load(std::memory_order_relaxed)));
                }
            }
            previous = now;
            have_previous = readable;
            std::this_thread::sleep_for(std::chrono::microseconds(80));
        }
    }

    GuestMemory& memory;
    std::shared_ptr<SyscallProcessState> process;
    VAddr address;
    s64 tid;
    std::atomic<VAddr> last_rip;
    std::jthread worker;
};

}  // namespace swift::linux
