#pragma once

#include <condition_variable>
#include <mutex>

#include "types.h"

namespace swift::runtime {

class ScopedRangeLock;

class RangeMutex {
public:
    explicit RangeMutex() = default;
    ~RangeMutex() = default;

private:
    friend class ScopedRangeLock;

    void Lock(ScopedRangeLock& l);
    void Unlock(ScopedRangeLock& l);
    bool HasIntersectionLocked(ScopedRangeLock& l);

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;

    using LockList = IntrusiveListBaseTraits<ScopedRangeLock>::ListType;
    LockList m_list;
};

class ScopedRangeLock : public IntrusiveListBaseNode<ScopedRangeLock> {
public:
    explicit ScopedRangeLock(RangeMutex& mutex, size_t address, size_t size)
        : m_mutex(mutex), m_address(address), m_size(size) {
        if (m_size > 0) {
            m_mutex.Lock(*this);
        }
    }
    ~ScopedRangeLock() {
        if (m_size > 0) {
            m_mutex.Unlock(*this);
        }
    }

    [[nodiscard]] size_t GetAddress() const {
        return m_address;
    }

    [[nodiscard]] size_t GetSize() const {
        return m_size;
    }

private:
    RangeMutex& m_mutex;
    const size_t m_address{};
    const size_t m_size{};
};

inline void RangeMutex::Lock(ScopedRangeLock& l) {
    std::unique_lock lk{m_mutex};
    m_cv.wait(lk, [&] { return !HasIntersectionLocked(l); });
    m_list.push_back(l);
}

inline void RangeMutex::Unlock(ScopedRangeLock& l) {
    {
        std::scoped_lock lk{m_mutex};
        m_list.erase(m_list.iterator_to(l));
    }
    m_cv.notify_all();
}

inline bool RangeMutex::HasIntersectionLocked(ScopedRangeLock& l) {
    const auto cur_begin = l.GetAddress();
    const auto cur_last = l.GetAddress() + l.GetSize() - 1;

    for (const auto& other : m_list) {
        const auto other_begin = other.GetAddress();
        const auto other_last = other.GetAddress() + other.GetSize() - 1;

        if (cur_begin <= other_last && other_begin <= cur_last) {
            return true;
        }
    }

    return false;
}

} // namespace swift::runtime
