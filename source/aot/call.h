#pragma once

#include "artifact.h"
#include "guest_call.h"
#include "runtime/execution/mem_map.h"
#include "runtime/execution/runtime.h"

#include <memory>

namespace swift::aot {
// One prepared guest/VM. Calls share guest globals and mappings, while each
// SysV frame restores the caller's architectural register snapshot.
class AotGuestEnv final : public guest_call::GuestCallEnv {
public:
    AotGuestEnv();
    ~AotGuestEnv() override;
    AotGuestEnv(const AotGuestEnv&) = delete;
    AotGuestEnv& operator=(const AotGuestEnv&) = delete;

    bool Init(const std::string& artifact_path,
              std::string& error,
              runtime::execution::RuntimeOptions = {});
    bool Init(const std::string& guest_path,
              const std::string& artifact_path,
              std::string& error,
              runtime::execution::RuntimeOptions = {});
    // Run the original entry/initial stack to a pre-patched function. The
    // stop byte must already belong to the embedded guest compiled by AOT.
    bool RunStartupUntil(const std::string& symbol, std::string& error, bool patch_stop = false);
    runtime::x86::GuestState& Context() override;
    void* HostPointer(std::uint64_t, std::uint64_t, bool write = false) override;
    std::uint64_t SentinelAddress() const override;
    guest_call::GuestRunReport RunToSentinel() override;
    std::uint64_t CallStackTop() override;
    std::uint64_t ScratchAlloc(std::uint64_t, std::uint64_t) override;
    void ScratchFree(std::uint64_t, std::uint64_t) noexcept override;
    std::uint64_t LookupSymbol(const std::string&) override;

    const Artifact& Image() const;
    const runtime::execution::RuntimeStats& Stats() const;
    const swift::Failure& LastFailure() const;
    // Explicit close reports release failures; the destructor still owns any
    // remaining mappings and retries through the ordinary VM owner.
    runtime::execution::MappingRelease Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}  // namespace swift::aot
