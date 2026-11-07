#include "code_image.h"
#include "dispatch.h"
#include "host_backend.h"
#include "runtime.h"
#include "runtime/arch/x86/cpu.h"
#include "svm/code_build_id.h"

#include <bit>
#include <cstring>

namespace swift::runtime::execution {
namespace {
void Number(Sha256& hash, std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes;
    for (unsigned i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    hash.Update(bytes);
}
}  // namespace

CodeImageKey MakeCodeImageKey(const Digest& guest,
                              std::uint64_t load_bias,
                              const HostBackendOps& backend,
                              const RuntimeOptions& options) {
    constexpr std::string_view build{SVM_CODE_BUILD_ID};
    CodeImageKey key;
    key.build_id = HashBytes({reinterpret_cast<const std::uint8_t*>(build.data()), build.size()});
    key.guest_id = guest;
    key.host_isa = backend.target.target;
    key.load_bias = load_bias;
    key.requested_scope = options.scope;
    Sha256 config;
    Number(config, CodeImageFormatVersion);
    Number(config, sizeof(GuestState));
    Number(config, sizeof(NativeRunContext));
    Number(config, offsetof(NativeRunContext, state));
    Number(config, sizeof(DispatchSlot));
    Number(config, std::endian::native == std::endian::little);
    Number(config, static_cast<unsigned>(backend.target.mode));
    Number(config, backend.target.target);
    Number(config, backend.target.features);
    Number(config, backend.isa_features);
    const auto& c = backend.capabilities;
    for (const bool value : {c.rcpc_loads,
                             c.rcpc_imm9,
                             c.native_nzcv,
                             c.lse_atomics,
                             c.crc32c,
                             c.afp,
                             c.aes,
                             c.pmull,
                             c.sha256})
        Number(config, value);
    Number(config, backend.pinned_uniform_slots);
    Number(config, backend.pinned_vectors);
    Number(config, backend.specializes_fp_flush);
    Number(config, options.optimize);
    // Counted dispatch changes emitted code, unlike logger/argv/environment.
    Number(config, options.decode_timing);
    Number(config, options.function_limits.max_instructions);
    Number(config, options.function_limits.max_blocks);
    Number(config, x86::BaselineGuestCpu.cet_ibt);
    Number(config, x86::BaselineGuestCpu.cet_shadow_stack);
    key.configuration = config.Finish();
    return key;
}

Digest CodeImageKeyDigest(const CodeImageKey& key) {
    Sha256 hash;
    hash.Update(key.build_id);
    hash.Update(key.guest_id);
    hash.Update(key.configuration);
    Number(hash, key.load_bias);
    Number(hash, key.host_isa);
    Number(hash, static_cast<unsigned>(key.requested_scope));
    return hash.Finish();
}
}  // namespace swift::runtime::execution
