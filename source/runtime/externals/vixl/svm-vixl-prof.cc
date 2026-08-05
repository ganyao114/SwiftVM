#include "svm-vixl-prof.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace swift { namespace runtime {
bool SvmVixlProfEnabled();
}}  // namespace swift::runtime

namespace vixl {
namespace svm_vixl_prof {
namespace {

struct Counters {
  std::atomic<unsigned long long> macro_ns{0};
  std::atomic<unsigned long long> macro_calls{0};
  std::atomic<unsigned long long> buffer_ns{0};
  std::atomic<unsigned long long> buffer_calls{0};
  std::atomic<unsigned long long> pool_fixup_ns{0};
  std::atomic<unsigned long long> pool_fixup_calls{0};
  std::atomic<unsigned long long> buffer_grows{0};
};

struct LocalCounters {
  unsigned long long macro_ns{};
  unsigned long long macro_calls{};
  unsigned long long buffer_ns{};
  unsigned long long buffer_calls{};
  unsigned long long pool_fixup_ns{};
  unsigned long long pool_fixup_calls{};
  unsigned long long buffer_grows{};
  unsigned macro_depth{};
  unsigned buffer_depth{};
  unsigned pool_fixup_depth{};
};

Counters& GetCounters() {
  static Counters counters;
  return counters;
}

thread_local LocalCounters local;
thread_local unsigned recording_depth;
thread_local bool jit_fast_enabled = true;

void Dump() {
  FlushThread();
  auto& c = GetCounters();
  auto get = [](const std::atomic<unsigned long long>& value) {
    return value.load(std::memory_order_relaxed);
  };
  std::fprintf(stderr,
               "[svm-vixl] macro_ns=%llu macro_calls=%llu "
               "buffer_ns=%llu buffer_calls=%llu buffer_grows=%llu "
               "pool_fixup_ns=%llu pool_fixup_calls=%llu\n",
               get(c.macro_ns), get(c.macro_calls), get(c.buffer_ns),
               get(c.buffer_calls), get(c.buffer_grows), get(c.pool_fixup_ns),
               get(c.pool_fixup_calls));
}

}  // namespace

bool Enabled() {
  const bool enabled = swift::runtime::SvmVixlProfEnabled();
  if (enabled) {
    static const bool registered = [] {
      std::atexit(Dump);
      return true;
    }();
    (void)registered;
  }
  return enabled;
}

bool FastEnabled() {
  return jit_fast_enabled;
}

bool Recording() {
  return recording_depth != 0 && Enabled();
}

bool BeginJit(bool fast_enabled) {
  const bool previous = jit_fast_enabled;
  jit_fast_enabled = fast_enabled;
  if (Enabled()) ++recording_depth;
  return previous;
}

void EndJit(bool previous_fast_enabled) {
  jit_fast_enabled = previous_fast_enabled;
  if (recording_depth != 0 && --recording_depth == 0) FlushThread();
}

bool Enter(Part part) {
  unsigned* depth = nullptr;
  switch (part) {
    case Part::kMacro:
      depth = &local.macro_depth;
      break;
    case Part::kBuffer:
      depth = &local.buffer_depth;
      break;
    case Part::kPoolFixup:
      depth = &local.pool_fixup_depth;
      break;
  }
  return (*depth)++ == 0;
}

void Leave(Part part, unsigned long long ns) {
  unsigned* depth = nullptr;
  unsigned long long* total = nullptr;
  unsigned long long* calls = nullptr;
  switch (part) {
    case Part::kMacro:
      depth = &local.macro_depth;
      total = &local.macro_ns;
      calls = &local.macro_calls;
      break;
    case Part::kBuffer:
      depth = &local.buffer_depth;
      total = &local.buffer_ns;
      calls = &local.buffer_calls;
      break;
    case Part::kPoolFixup:
      depth = &local.pool_fixup_depth;
      total = &local.pool_fixup_ns;
      calls = &local.pool_fixup_calls;
      break;
  }
  if (--*depth == 0) {
    *total += ns;
    ++*calls;
  }
}

void CountBufferGrow() {
  if (Recording()) ++local.buffer_grows;
}

void FlushThread() {
  if (!Enabled()) return;
  auto& c = GetCounters();
  c.macro_ns.fetch_add(local.macro_ns, std::memory_order_relaxed);
  c.macro_calls.fetch_add(local.macro_calls, std::memory_order_relaxed);
  c.buffer_ns.fetch_add(local.buffer_ns, std::memory_order_relaxed);
  c.buffer_calls.fetch_add(local.buffer_calls, std::memory_order_relaxed);
  c.pool_fixup_ns.fetch_add(local.pool_fixup_ns, std::memory_order_relaxed);
  c.pool_fixup_calls.fetch_add(local.pool_fixup_calls, std::memory_order_relaxed);
  c.buffer_grows.fetch_add(local.buffer_grows, std::memory_order_relaxed);
  local = {};
}

}  // namespace svm_vixl_prof
}  // namespace vixl
