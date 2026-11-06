// Copyright 2017, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_CODE_BUFFER_H
#define VIXL_CODE_BUFFER_H
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "globals-vixl.h"
#include "utils-vixl.h"
namespace vixl {
struct CodeBufferOverflow {};
// SVM exact fork: the caller's words are the sole storage and cursor.
class CodeBuffer {
 public:
  explicit CodeBuffer(std::vector<uint32_t>& words) : words_(words) {}
  void Reset() { words_.clear(); }
  ptrdiff_t GetOffsetFrom(ptrdiff_t offset) const { return GetCursorOffset() - offset; }
  ptrdiff_t GetCursorOffset() const { return static_cast<ptrdiff_t>(words_.size() * 4); }
  void Rewind(ptrdiff_t offset) { VIXL_ASSERT(offset >= 0 && !(offset & 3)); words_.resize(offset / 4); }
  template<class T> T GetOffsetAddress(ptrdiff_t offset) const {
    return reinterpret_cast<T>(reinterpret_cast<byte*>(words_.data()) + offset);
  }
  template<class T> T GetStartAddress() const { return GetOffsetAddress<T>(0); }
  template<class T> T GetEndAddress() const { return GetOffsetAddress<T>(GetCursorOffset()); }
  size_t GetSizeInBytes() const { return words_.size() * 4; }
  size_t GetCapacity() const { return words_.capacity() * 4; }
  size_t GetRemainingBytes() const { return (words_.capacity() - words_.size()) * 4; }
  bool IsManaged() const { return false; }
  bool IsDirty() const { return false; }
  void SetClean() {}
  bool Is16bitAligned() const { return true; }
  bool Is32bitAligned() const { return true; }
  bool HasSpaceFor(size_t amount) const { return GetRemainingBytes() >= amount; }
  void EnsureSpaceFor(size_t amount, bool* grown) {
    *grown = !HasSpaceFor(amount);
    if (*grown) Grow(std::max(GetCapacity() * 2, GetSizeInBytes() + amount));
  }
  void EnsureSpaceFor(size_t amount) { bool grown; EnsureSpaceFor(amount, &grown); }
  void Grow(size_t capacity) { words_.reserve((capacity + 3) / 4); }
  template<class T> VIXL_ALWAYS_INLINE void Emit(T value) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    if (words_.size() > UINT32_MAX / 4 - sizeof(T) / 4) throw CodeBufferOverflow{};
    if constexpr (sizeof(T) == 4) {
      uint32_t word; std::memcpy(&word, &value, 4); words_.push_back(word);
    } else {
      uint64_t data; std::memcpy(&data, &value, 8);
      words_.push_back(static_cast<uint32_t>(data)); words_.push_back(static_cast<uint32_t>(data >> 32));
    }
  }
  VIXL_ALWAYS_INLINE void Emit32(uint32_t value) { Emit(value); }
  VIXL_ALWAYS_INLINE void Emit64(uint64_t value) { Emit(value); }
  void Emit8(uint8_t) = delete;
  void Emit16(uint16_t) = delete;
  void EmitData(const void* data, size_t bytes) {
    if (bytes & 3) throw std::invalid_argument("native DATA must be word aligned");
    const auto old = words_.size(); words_.resize(old + bytes / 4);
    std::memcpy(words_.data() + old, data, bytes);
  }
  void UpdateData(size_t offset, const void* data, size_t bytes) { std::memcpy(GetOffsetAddress<void*>(offset), data, bytes); }
  void Align() {}
  void EmitZeroedBytes(int bytes) {
    if (bytes < 0 || (bytes & 3)) throw std::invalid_argument("native DATA must be word aligned");
    words_.resize(words_.size() + bytes / 4);
  }
 private:
  std::vector<uint32_t>& words_;
};
} // namespace vixl
#endif
