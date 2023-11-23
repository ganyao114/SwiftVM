#pragma once

#include <type_traits>
#include <utility>

namespace swift::runtime {

void* AllocateMemoryPages(std::size_t size) noexcept;
void FreeMemoryPages(void* base, std::size_t size) noexcept;

template <typename T> class VirtualVector final {
public:
    constexpr VirtualVector() = default;
    explicit VirtualVector(std::size_t count) : alloc_size{count * sizeof(T)} {
        base_ptr = reinterpret_cast<T*>(AllocateMemoryPages(alloc_size));
    }

    ~VirtualVector() noexcept { FreeMemoryPages(base_ptr, alloc_size); }

    VirtualVector(const VirtualVector&) = delete;
    VirtualVector& operator=(const VirtualVector&) = delete;

    VirtualVector(VirtualVector&& other) noexcept
            : alloc_size{std::exchange(other.alloc_size, 0)}
            , base_ptr{std::exchange(other.base_ptr), nullptr} {}

    VirtualVector& operator=(VirtualVector&& other) noexcept {
        alloc_size = std::exchange(other.alloc_size, 0);
        base_ptr = std::exchange(other.base_ptr, nullptr);
        return *this;
    }

    void resize(std::size_t count) {
        const auto new_size = count * sizeof(T);
        if (new_size == alloc_size) {
            return;
        }

        FreeMemoryPages(base_ptr, alloc_size);

        alloc_size = new_size;
        base_ptr = reinterpret_cast<T*>(AllocateMemoryPages(alloc_size));
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const {
        return base_ptr[index - start_index];
    }

    [[nodiscard]] constexpr T& operator[](std::size_t index) {
        return base_ptr[index - start_index];
    }

    [[nodiscard]] constexpr T* data() { return base_ptr; }

    [[nodiscard]] constexpr const T* data() const { return base_ptr; }

    [[nodiscard]] constexpr std::size_t size() const { return alloc_size / sizeof(T); }

    constexpr void SetStartIndex(std::size_t index) { start_index = index; }

    [[nodiscard]] constexpr bool Overlap(std::size_t index) const {
        ssize_t i = index - start_index;
        return i >= 0 && i <= size();
    }

private:
    std::size_t alloc_size{};
    T* base_ptr{};
    std::size_t start_index{};
};

}
