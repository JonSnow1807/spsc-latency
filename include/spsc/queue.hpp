#pragma once

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace spsc {

// Explicit ABI choice: Apple M-series uses 128-byte cache lines; 64 elsewhere.
// Override for a different target, identically in every translation unit.
#ifdef SPSC_CACHE_LINE
inline constexpr std::size_t cache_line = SPSC_CACHE_LINE;
#elif defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t cache_line = 128;
#else
inline constexpr std::size_t cache_line = 64;
#endif
static_assert(std::has_single_bit(cache_line) && cache_line >= 2 * sizeof(std::size_t));
static_assert(std::atomic<std::size_t>::is_always_lock_free);

namespace detail {
enum class design { naive, acqrel, padded, cached };
struct compact_index { std::atomic<std::size_t> value{0}; };
struct alignas(cache_line) adjacent_indices {
    compact_index write;
    compact_index read;
};
struct alignas(cache_line) isolated_index {
    std::atomic<std::size_t> value{0};
    std::size_t cached_peer{0}; // Accessed only by this index's owning thread.
};
struct isolated_indices {
    isolated_index write;
    isolated_index read;
};
static_assert(alignof(adjacent_indices) == cache_line);
static_assert(sizeof(adjacent_indices) == cache_line);
static_assert(offsetof(adjacent_indices, read) + sizeof(compact_index) <= cache_line);
static_assert(alignof(isolated_index) == cache_line);
static_assert(sizeof(isolated_index) == cache_line);
static_assert(offsetof(isolated_indices, read) == cache_line);

// Index is a test seam for exercising actual unsigned counter overflow quickly.
template<class T, design Design, std::unsigned_integral Index = std::size_t>
class queue {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_assignable_v<T>);
    static_assert(std::is_nothrow_destructible_v<T>);
    static constexpr bool sequential = Design == design::naive;
    static constexpr auto own_order = sequential ? std::memory_order_seq_cst : std::memory_order_relaxed;
    // Acquire pairs with peer release: see the load sites below.
    static constexpr auto peer_order = sequential ? std::memory_order_seq_cst : std::memory_order_acquire;
    // Release publishes construction/destruction: see the store sites below.
    static constexpr auto publish_order = sequential ? std::memory_order_seq_cst : std::memory_order_release;
    using indices = std::conditional_t<sequential || Design == design::acqrel, adjacent_indices, isolated_indices>;
    struct alignas(T) slot { std::byte bytes[sizeof(T)]; };
    static_assert(alignof(slot) >= alignof(T));
    static_assert(alignof(indices) >= cache_line);

    // Keep read-only metadata and the allocation separate from both index lines.
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<slot[]> slots_;
    alignas(cache_line) indices indices_;

    static std::size_t checked_capacity(std::size_t n) {
        if (!std::has_single_bit(n) || n > std::numeric_limits<Index>::max() / 2)
            throw std::invalid_argument("capacity must be a power of two <= index_max/2");
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(slot))
            throw std::length_error("capacity * slot size overflows");
        return n;
    }
    static Index advance(Index n) noexcept { return static_cast<Index>(n + Index{1}); }
    static std::size_t distance(Index a, Index b) noexcept { return static_cast<Index>(a - b); }
    T* raw(Index n) noexcept { return reinterpret_cast<T*>(slots_[n & mask_].bytes); }
    T* live(Index n) noexcept { return std::launder(raw(n)); }

public:
    explicit queue(std::size_t capacity)
        : capacity_(checked_capacity(capacity)), mask_(capacity_ - 1),
          slots_(std::make_unique<slot[]>(capacity_)) {}
    queue(const queue&) = delete;
    queue& operator=(const queue&) = delete;
    queue(queue&&) = delete;
    queue& operator=(queue&&) = delete;
    // Both threads must have stopped before destruction.
    ~queue() {
        auto read = static_cast<Index>(indices_.read.value.load(std::memory_order_relaxed));
        const auto write = static_cast<Index>(indices_.write.value.load(std::memory_order_relaxed));
        while (read != write) {
            std::destroy_at(live(read));
            read = advance(read);
        }
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    template<class... Args> requires std::constructible_from<T, Args...>
    bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const auto write = static_cast<Index>(indices_.write.value.load(own_order));
        Index read;
        if constexpr (Design == design::cached) {
            read = static_cast<Index>(indices_.write.cached_peer);
            if (distance(write, read) == capacity_) {
                // Acquire observes consumer destruction before this slot is reused.
                read = static_cast<Index>(indices_.read.value.load(peer_order));
                indices_.write.cached_peer = read;
                if (distance(write, read) == capacity_) return false;
            }
        } else {
            // Acquire observes consumer destruction before this slot is reused (seq_cst in naive).
            read = static_cast<Index>(indices_.read.value.load(peer_order));
            if (distance(write, read) == capacity_) return false;
        }
        std::construct_at(raw(write), std::forward<Args>(args)...);
        // Release makes the constructed element visible to the consumer (seq_cst in naive).
        indices_.write.value.store(advance(write), publish_order);
        return true;
    }
    bool try_push(T&& value) noexcept { return try_emplace(std::move(value)); }
    bool try_push(const T& value) requires std::copy_constructible<T> { return try_emplace(value); }

    bool try_pop(T& out) noexcept {
        const auto read = static_cast<Index>(indices_.read.value.load(own_order));
        Index write;
        if constexpr (Design == design::cached) {
            write = static_cast<Index>(indices_.read.cached_peer);
            if (read == write) {
                // Acquire observes producer construction before reading this element.
                write = static_cast<Index>(indices_.write.value.load(peer_order));
                indices_.read.cached_peer = write;
                if (read == write) return false;
            }
        } else {
            // Acquire observes producer construction before reading (seq_cst in naive).
            write = static_cast<Index>(indices_.write.value.load(peer_order));
            if (read == write) return false;
        }
        out = std::move(*live(read));
        std::destroy_at(live(read));
        // Release completes element access/destruction before producer reuse (seq_cst in naive).
        indices_.read.value.store(advance(read), publish_order);
        return true;
    }
};
} // namespace detail
} // namespace spsc
