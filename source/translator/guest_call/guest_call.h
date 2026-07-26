//
// host -> guest function calls.  docs/aot-design.md §6.
//
//     auto strlen_g = env.Lookup<std::uint64_t(GuestPtr<const char>)>("strlen");
//     std::uint64_t n = strlen_g(guest_string);
//
// The signature is parsed at COMPILE time (abi_sysv.h): which argument goes in
// which register, how many bytes of stack argument area, whether the return
// value needs a hidden buffer pointer in %rdi -- all of it is a constant by the
// time operator() runs.
//
// WHERE THE CALL ACTUALLY GOES is the one runtime decision, and it is behind
// GuestCallEnv.  Today the only implementation drives the existing JIT
// (jit_env.h: set up ThreadContext64, X86Core::Run()).  An AOT artifact would
// supply a different GuestCallEnv; nothing in the marshalling changes.
//
// FAILURE IS NOT IGNORABLE.  A guest function can fault, execute an illegal
// instruction, make a syscall, or corrupt its own stack.  None of those may be
// reported as "here is your return value":
//   - operator() throws GuestCallError;
//   - Try() returns a [[nodiscard]] GuestCallResult whose value() is only
//     reachable after checking ok().
//

#pragma once

#include <cstring>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "abi_sysv.h"
#include "guest_ptr.h"
#include "translator/x86/cpu.h"

namespace swift::guest_call {

enum class GuestCallStatus : std::uint8_t {
    Ok = 0,
    // The guest stopped for a reason that is not "returned to the sentinel":
    // an illegal instruction, an unmapped access, a host signal.
    GuestFault,
    // The guest executed `syscall` and no syscall handler was installed.
    UnexpectedSyscall,
    // The guest halted, but not at the sentinel return address: it either ran
    // off into a `hlt` of its own or the return address was corrupted.
    BadReturnAddress,
    // The guest returned, but %rsp is not where the ABI says it must be.  The
    // return value is not trustworthy after a stack imbalance.
    StackImbalance,
    // Setup failed before the guest ran (no room for the argument area, the
    // MEMORY return buffer could not be allocated, ...).
    SetupFailed,
};

const char* ToString(GuestCallStatus status);

// Everything the caller might want to know about an abnormal call, kept
// separate from the return value so the two can never be confused.
struct GuestCallDiagnostic {
    GuestCallStatus status{GuestCallStatus::Ok};
    std::uint64_t entry{};
    std::uint64_t rip{};    // where the guest actually stopped
    std::uint64_t rsp{};
    std::uint32_t exit_reason{};
    std::uint32_t interrupt{};
};

class GuestCallError : public std::runtime_error {
public:
    explicit GuestCallError(const GuestCallDiagnostic& d);
    [[nodiscard]] const GuestCallDiagnostic& diagnostic() const { return diag_; }
    [[nodiscard]] GuestCallStatus status() const { return diag_.status; }

private:
    GuestCallDiagnostic diag_;
};

// Result of a non-throwing call.  [[nodiscard]] plus a value() that throws
// unless ok(): there is no path from a faulted call to a plausible-looking
// return value.
template <typename Ret>
class [[nodiscard]] GuestCallResult {
public:
    GuestCallResult() = default;
    explicit GuestCallResult(const GuestCallDiagnostic& d) : diag_(d) {}

    [[nodiscard]] bool ok() const { return diag_.status == GuestCallStatus::Ok; }
    explicit operator bool() const { return ok(); }
    [[nodiscard]] GuestCallStatus status() const { return diag_.status; }
    [[nodiscard]] const GuestCallDiagnostic& diagnostic() const { return diag_; }

    [[nodiscard]] Ret value() const {
        if (!ok()) {
            throw GuestCallError(diag_);
        }
        return std::bit_cast<Ret>(raw_);
    }

    struct Raw {
        std::byte bytes[sizeof(Ret)];
    };
    Raw raw_{};

private:
    GuestCallDiagnostic diag_{};
};

template <>
class [[nodiscard]] GuestCallResult<void> {
public:
    GuestCallResult() = default;
    explicit GuestCallResult(const GuestCallDiagnostic& d) : diag_(d) {}
    [[nodiscard]] bool ok() const { return diag_.status == GuestCallStatus::Ok; }
    explicit operator bool() const { return ok(); }
    [[nodiscard]] GuestCallStatus status() const { return diag_.status; }
    [[nodiscard]] const GuestCallDiagnostic& diagnostic() const { return diag_; }
    void value() const {
        if (!ok()) {
            throw GuestCallError(diag_);
        }
    }

private:
    GuestCallDiagnostic diag_{};
};

// ---------------------------------------------------------------------------
// The environment: "what does it mean to run the guest from here"
// ---------------------------------------------------------------------------

enum class GuestRunOutcome : std::uint8_t {
    ReturnedToSentinel,
    Faulted,
    UnexpectedSyscall,
    HaltedElsewhere,
};

struct GuestRunReport {
    GuestRunOutcome outcome{};
    std::uint32_t exit_reason{};
    std::uint32_t interrupt{};
};

class GuestCallEnv {
public:
    virtual ~GuestCallEnv() = default;

    // The register file the call marshals into.
    virtual x86::ThreadContext64& Context() = 0;

    // Guest address -> host pointer, or nullptr if [addr, addr+size) is not
    // mapped.  Used for the stack image and the MEMORY return buffer.
    virtual void* HostPointer(std::uint64_t guest_addr, std::uint64_t size) = 0;

    // The return address pushed before entry.  Reaching it must stop the run.
    [[nodiscard]] virtual std::uint64_t SentinelAddress() const = 0;

    // Runs from Context().rip until the sentinel is reached or something goes
    // wrong.  This is the AOT injection point: an AOT environment would jump
    // straight into compiled host code here.
    virtual GuestRunReport RunToSentinel() = 0;

    // Highest usable address of the guest stack for a call (16-byte aligned).
    [[nodiscard]] virtual std::uint64_t CallStackTop() = 0;

    // Scratch guest memory, used for MEMORY return buffers and
    // ScopedGuestBuffer.  Returns 0 on failure.
    virtual std::uint64_t ScratchAlloc(std::uint64_t size, std::uint64_t align) = 0;
    virtual void ScratchFree(std::uint64_t addr, std::uint64_t size) = 0;

    // Symbol lookup, when the environment has a symbol table.  Returns 0 if
    // unknown.
    virtual std::uint64_t LookupSymbol(const std::string& name) { (void)name; return 0; }

    template <typename Sig>
    auto Lookup(const std::string& name);
    template <typename Sig>
    auto At(std::uint64_t entry);
};

// ---------------------------------------------------------------------------
// The marshalling core (non-template; the templates only collect raw bytes)
// ---------------------------------------------------------------------------

struct RawArg {
    const std::byte* data;
    unsigned size;
};

// Marshals `args` into env per `plan`, runs, and writes the raw return bytes
// into `ret_out` (plan.ret_size bytes, untouched unless the status is Ok).
GuestCallStatus PerformGuestCall(GuestCallEnv& env,
                                 std::uint64_t entry,
                                 const CallPlan& plan,
                                 const RawArg* args,
                                 unsigned nargs,
                                 std::byte* ret_out,
                                 GuestCallDiagnostic& diag);

// ---------------------------------------------------------------------------
// GuestFn
// ---------------------------------------------------------------------------

namespace detail {

template <typename T>
constexpr void CheckMarshalable() {
    static_assert(kIsMarshalable<T>,
                  "this type cannot cross the host/guest boundary. Raw host pointers are "
                  "rejected on purpose (a host address is meaningless to guest code) -- use "
                  "GuestPtr<T>, or ScopedGuestBuffer to copy host data into the guest window. "
                  "Non-trivially-copyable types, references and long double are also refused.");
}

template <typename Ret>
Ret Finish(GuestCallStatus status, const GuestCallDiagnostic& diag, const std::byte* raw) {
    if (status != GuestCallStatus::Ok) {
        throw GuestCallError(diag);
    }
    if constexpr (std::is_void_v<Ret>) {
        (void)raw;
        return;
    } else {
        typename GuestCallResult<Ret>::Raw storage{};
        std::memcpy(storage.bytes, raw, sizeof(Ret));
        return std::bit_cast<Ret>(storage);
    }
}

template <typename Ret>
GuestCallResult<Ret> FinishTry(GuestCallStatus status,
                               const GuestCallDiagnostic& diag,
                               const std::byte* raw) {
    GuestCallResult<Ret> r{diag};
    if constexpr (!std::is_void_v<Ret>) {
        if (status == GuestCallStatus::Ok) {
            std::memcpy(r.raw_.bytes, raw, sizeof(Ret));
        }
    } else {
        (void)raw;
    }
    return r;
}

// Return-value scratch big enough for any supported return type.
inline constexpr unsigned kRetScratch = 512;

}  // namespace detail

template <typename Sig>
class GuestFn;

// --- fixed-arity guest function --------------------------------------------
template <typename Ret, typename... Args>
class GuestFn<Ret(Args...)> {
public:
    static constexpr CallPlan kPlan = MakeCallPlan<Ret, Args...>();

    GuestFn() = default;
    GuestFn(GuestCallEnv* env, std::uint64_t entry) : env_(env), entry_(entry) {}

    [[nodiscard]] std::uint64_t entry() const { return entry_; }
    [[nodiscard]] bool valid() const { return env_ != nullptr && entry_ != 0; }
    [[nodiscard]] static constexpr const CallPlan& plan() { return kPlan; }

    Ret operator()(Args... args) const {
        std::byte ret[detail::kRetScratch];
        GuestCallDiagnostic diag{};
        const auto status = Invoke(args..., ret, diag);
        return detail::Finish<Ret>(status, diag, ret);
    }

    [[nodiscard]] GuestCallResult<Ret> Try(Args... args) const {
        std::byte ret[detail::kRetScratch];
        GuestCallDiagnostic diag{};
        const auto status = Invoke(args..., ret, diag);
        return detail::FinishTry<Ret>(status, diag, ret);
    }

private:
    GuestCallStatus Invoke(Args... args, std::byte* ret, GuestCallDiagnostic& diag) const {
        (detail::CheckMarshalable<Args>(), ...);
        detail::CheckMarshalable<Ret>();
        const RawArg raw[] = {
                RawArg{reinterpret_cast<const std::byte*>(&args),
                       static_cast<unsigned>(sizeof(Args))}...,
                RawArg{nullptr, 0}};
        return PerformGuestCall(*env_, entry_, kPlan, raw, sizeof...(Args), ret, diag);
    }

    GuestCallEnv* env_{};
    std::uint64_t entry_{};
};

// --- variadic guest function ------------------------------------------------
//
// `GuestFn<int(GuestPtr<char>, std::uint64_t, GuestPtr<const char>, ...)>`
// spells a guest `snprintf`.  The variadic tail is still classified at compile
// time -- once per call site, from the types actually passed -- so %al gets the
// exact number of vector registers used, as the psABI requires for any call to
// a function declared with an ellipsis.  C default argument promotions
// (float -> double, char/short/bool -> int) are applied first, exactly as a C
// compiler would at the call site.
template <typename Ret, typename... Args>
class GuestFn<Ret(Args......)> {
public:
    GuestFn() = default;
    GuestFn(GuestCallEnv* env, std::uint64_t entry) : env_(env), entry_(entry) {}

    [[nodiscard]] std::uint64_t entry() const { return entry_; }
    [[nodiscard]] bool valid() const { return env_ != nullptr && entry_ != 0; }

    template <typename... VArgs>
    static constexpr CallPlan PlanFor() {
        return MakeCallPlan<Ret, Args..., VarargPromote<VArgs>...>();
    }

    template <typename... VArgs>
    Ret operator()(Args... args, VArgs... vargs) const {
        std::byte ret[detail::kRetScratch];
        GuestCallDiagnostic diag{};
        const auto status = Invoke<VArgs...>(args..., diag, ret, vargs...);
        return detail::Finish<Ret>(status, diag, ret);
    }

    template <typename... VArgs>
    [[nodiscard]] GuestCallResult<Ret> Try(Args... args, VArgs... vargs) const {
        std::byte ret[detail::kRetScratch];
        GuestCallDiagnostic diag{};
        const auto status = Invoke<VArgs...>(args..., diag, ret, vargs...);
        return detail::FinishTry<Ret>(status, diag, ret);
    }

private:
    template <typename... VArgs>
    GuestCallStatus Invoke(Args... args,
                           GuestCallDiagnostic& diag,
                           std::byte* ret,
                           VArgs... vargs) const {
        (detail::CheckMarshalable<Args>(), ...);
        (detail::CheckMarshalable<VarargPromote<VArgs>>(), ...);
        detail::CheckMarshalable<Ret>();
        static constexpr CallPlan kPlan = MakeCallPlan<Ret, Args..., VarargPromote<VArgs>...>();
        std::tuple<VarargPromote<VArgs>...> promoted{
                static_cast<VarargPromote<VArgs>>(vargs)...};
        return std::apply(
                [&](auto&... pv) {
                    const RawArg raw[] = {
                            RawArg{reinterpret_cast<const std::byte*>(&args),
                                   static_cast<unsigned>(sizeof(Args))}...,
                            RawArg{reinterpret_cast<const std::byte*>(&pv),
                                   static_cast<unsigned>(sizeof(pv))}...,
                            RawArg{nullptr, 0}};
                    return PerformGuestCall(*env_, entry_, kPlan, raw,
                                            sizeof...(Args) + sizeof...(VArgs), ret, diag);
                },
                promoted);
    }

    GuestCallEnv* env_{};
    std::uint64_t entry_{};
};

template <typename Sig>
auto GuestCallEnv::Lookup(const std::string& name) {
    return GuestFn<Sig>{this, LookupSymbol(name)};
}
template <typename Sig>
auto GuestCallEnv::At(std::uint64_t entry) {
    return GuestFn<Sig>{this, entry};
}

// ---------------------------------------------------------------------------
// ScopedGuestBuffer -- the explicit, opt-in copy semantics
// ---------------------------------------------------------------------------
//
// The deliberate counterpart to GuestPtr: when you DO want host data inside
// the guest window, you say so, and the ownership and the copy-back point are
// both visible in the source.  A scope, not a convention.
class ScopedGuestBuffer {
public:
    ScopedGuestBuffer() = default;
    ScopedGuestBuffer(GuestCallEnv& env, std::uint64_t size, std::uint64_t align = 16);
    ~ScopedGuestBuffer();

    ScopedGuestBuffer(const ScopedGuestBuffer&) = delete;
    ScopedGuestBuffer& operator=(const ScopedGuestBuffer&) = delete;
    ScopedGuestBuffer(ScopedGuestBuffer&& other) noexcept;
    ScopedGuestBuffer& operator=(ScopedGuestBuffer&& other) noexcept;

    [[nodiscard]] bool valid() const { return addr_ != 0; }
    [[nodiscard]] std::uint64_t address() const { return addr_; }
    [[nodiscard]] std::uint64_t size() const { return size_; }

    template <typename T>
    [[nodiscard]] GuestPtr<T> as() const {
        return GuestPtr<T>{addr_};
    }

    // Copy host bytes in / guest bytes out.  Both are explicit calls: there is
    // no implicit write-back at scope exit, because "when does it copy back"
    // must not be something the reader has to remember.
    bool Write(const void* src, std::uint64_t size, std::uint64_t offset = 0);
    bool Read(void* dst, std::uint64_t size, std::uint64_t offset = 0) const;

    // Convenience: copy a NUL-terminated host string into the guest window.
    static ScopedGuestBuffer FromCString(GuestCallEnv& env, const char* s);

private:
    GuestCallEnv* env_{};
    std::uint64_t addr_{};
    std::uint64_t size_{};
};

}  // namespace swift::guest_call
