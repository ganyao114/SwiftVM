#include "test_vm.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <string>

#include "abi_stubs_index.h"

namespace svmtest {

using swift::guest_call::JitGuestEnv;

namespace {

std::unique_ptr<JitGuestEnv> g_env;
VmKind g_kind{};
bool g_have{};
std::uint64_t g_stub_table[kStubCount]{};

void BringUpStub() {
    auto env = std::make_unique<JitGuestEnv>();
    std::string error;
    if (!env->Init(SVM_GUEST_CALL_STUB_ELF, error)) {
        FAIL("cannot load the ABI stub guest: " << error);
    }
    // The dump buffer lives at a fixed guest address the stubs write to.
    if (!env->space().MapFixed(SVM_DUMP_ADDR, 0x4000)) {
        FAIL("cannot map the argument dump page");
    }
    const std::uint64_t entry = env->image().entry;
    void* table = env->HostPointer(entry, sizeof(g_stub_table));
    if (table == nullptr) {
        FAIL("stub pointer table is not mapped");
    }
    std::memcpy(g_stub_table, table, sizeof(g_stub_table));
    g_env = std::move(env);
}

void BringUpGlibc() {
    auto env = std::make_unique<JitGuestEnv>();
    std::string error;
    if (!env->Init(SVM_GUEST_CALL_GLIBC_ELF, error)) {
        FAIL("cannot load the glibc guest: " << error);
    }
    if (!env->RunStartupUntil("main", error)) {
        FAIL("glibc startup did not reach main: " << error);
    }
    g_env = std::move(env);
}

}  // namespace

JitGuestEnv& Vm(VmKind kind) {
    if (g_have && g_kind == kind) {
        return *g_env;
    }
    g_env.reset();  // only one environment may be live
    g_have = false;
    if (kind == VmKind::Stub) {
        BringUpStub();
    } else {
        BringUpGlibc();
    }
    g_kind = kind;
    g_have = true;
    return *g_env;
}

std::uint64_t StubEntry(int index) {
    Vm(VmKind::Stub);
    REQUIRE(index >= 0);
    REQUIRE(index < kStubCount);
    return g_stub_table[index];
}

const std::uint64_t* DumpBuffer() {
    auto& env = Vm(VmKind::Stub);
    return static_cast<const std::uint64_t*>(env.HostPointer(SVM_DUMP_ADDR, SVM_DUMP_SIZE));
}

void ClearDumpBuffer() {
    auto& env = Vm(VmKind::Stub);
    std::memset(env.HostPointer(SVM_DUMP_ADDR, SVM_DUMP_SIZE), 0, SVM_DUMP_SIZE);
}

}  // namespace svmtest
