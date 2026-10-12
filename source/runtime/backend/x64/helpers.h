#pragma once

#include "runtime/backend/context.h"
#include "runtime/ir/opcodes.h"
#include "runtime/common/host_pair_result.h"

namespace swift::runtime::backend::x64 {

u64 Condition(u64 flags, ir::Cond condition);
u64 TestFlags(u64 flags, ir::Flags requested);
void SaveFlags(State* state, u64 result, u64 native_flags, ir::Flags requested,
               ir::OpCode producer, u32 bits);
void ClearFlags(State* state, ir::Flags requested);
bool CheckMemory(State* state, u64 address, u64 size);
bool CheckAndBeginMemory(State* state, u64 address, u64 size);
void BeginMemory(State* state);
void EndMemory(State* state);
u64 SignedDivide(u64 left, u64 right, u32 bits);
u64 UnsignedDivide(u64 left, u64 right);
u64 CountBits(u64 value);
u64 CompareCondition(u64 packed, ir::Cond condition);
void PublishCompareFlags(State* state, u64 packed, u64 compact);
u64 RoundIntegral(u64 raw, u32 bits, u32 mode);
u64 FusedMultiplyAdd(u64 a, u64 b, u64 c, u32 bits, u32 flags);
void AesRound(void* output, const void* input, const void* key, u32 mode);
void AesKeygen(void* output, const void* input, u32 rcon);
void PolynomialMultiply(void* output, u64 left, u64 right);
const u32* Crc32cTable();
void Sha256Message(void* output, const void* destination, const void* source, u32 second);
void Sha256Rounds(void* output, const void* destination, const void* source, const void* key);
HostPairResult AtomicCompareExchange128(void* address, u64 expected_low, u64 expected_high,
                                       u64 desired_low, u64 desired_high);

}  // namespace swift::runtime::backend::x64
