#ifndef VIXL_SVM_VIXL_PROF_H_
#define VIXL_SVM_VIXL_PROF_H_

#include <chrono>
#include <cstddef>

namespace vixl {
namespace svm_vixl_prof {

enum class Part {
  kMacro,
  kBuffer,
  kPoolFixup,
};

bool Enabled();
bool FastEnabled();
bool Recording();
bool BeginJit(bool fast_enabled);
void EndJit(bool previous_fast_enabled);
bool Enter(Part part);
void Leave(Part part, unsigned long long ns);
void CountBufferGrow();
void FlushThread();

class JitScope {
 public:
  explicit JitScope(bool fast_enabled)
      : previous_fast_enabled_(BeginJit(fast_enabled)) {}
  ~JitScope() { EndJit(previous_fast_enabled_); }

  JitScope(const JitScope&) = delete;
  JitScope& operator=(const JitScope&) = delete;

 private:
  bool previous_fast_enabled_;
};

class Scope {
 public:
  Scope() = default;

  explicit Scope(Part part) { Start(part); }

  ~Scope() { Stop(); }

  void Start(Part part) {
    part_ = part;
    entered_ = Recording();
    if (entered_) {
      timing_ = Enter(part);
    }
    if (timing_) {
      start_ = std::chrono::steady_clock::now();
    }
  }

  void Stop() {
    if (!entered_) return;
    unsigned long long ns = 0;
    if (timing_) {
      ns = static_cast<unsigned long long>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start_)
              .count());
    }
    Leave(part_, ns);
    entered_ = false;
    timing_ = false;
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Part part_;
  bool entered_{};
  bool timing_{};
  std::chrono::steady_clock::time_point start_;
};

}  // namespace svm_vixl_prof
}  // namespace vixl

#endif  // VIXL_SVM_VIXL_PROF_H_
