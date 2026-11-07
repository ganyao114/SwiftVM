#pragma once

namespace swift::tests {
enum class NativeISA { None, Arm64, Arm64Android, Riscv64, X64, LoongArch64, Host };

// Selected once by the shared test runner. Compile cases never execute
// generated instructions; native cases require the requested real host ISA.
bool NativeMode() noexcept;
void SetNativeMode(bool) noexcept;
bool CanExecute(NativeISA) noexcept;
NativeISA HostISA() noexcept;
}  // namespace swift::tests
