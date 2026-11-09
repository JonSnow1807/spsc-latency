#include "spsc/naive.hpp"
#include "spsc/padded.hpp"
#include "spsc/cached.hpp"
#include "spsc/acqrel.hpp"
#include "histogram.hpp"
#include "support.hpp"
#include <array>
#include <barrier>
#include <cstdint>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// Apple libc++ does not yet ship std::jthread/stop_token with this compiler.
class watchdog {
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
public:
    explicit watchdog(std::uint64_t seconds) : thread_([this, seconds] {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(seconds), [this] { return done_; })) {
            std::cerr << "FAIL: test timeout\n";
            std::_Exit(2);
        }
    }) {}
    ~watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        cv_.notify_one();
        thread_.join();
    }
};
struct alignas(256) tracked {
    static inline int alive = 0;
    int value;
    explicit tracked(int n) : value(n) {
        require(reinterpret_cast<std::uintptr_t>(this) % alignof(tracked) == 0, "runtime over-alignment");
        if (n == -1) throw std::runtime_error("test constructor failure");
        ++alive;
    }
    tracked(tracked&& other) noexcept : value(other.value) { ++alive; other.value = -2; }
    tracked& operator=(tracked&& other) noexcept { value = other.value; other.value = -2; return *this; }
    ~tracked() { --alive; }
};
template<template<class> class Q>
void unit_queue() {
    static_assert(alignof(Q<int>) >= spsc::cache_line);
    for (const auto capacity : {1U, 2U, 4U, 64U}) {
        Q<int> q(capacity);
        require(q.capacity() == capacity, "exact usable capacity");
        int output = -7;
        require(!q.try_pop(output) && output == -7, "empty leaves output unchanged");
        for (int cycle = 0; cycle < 100; ++cycle) {
            for (unsigned i = 0; i < capacity; ++i) require(q.try_push(static_cast<int>(i)), "fill");
            require(!q.try_push(99), "full");
            for (unsigned i = 0; i < capacity; ++i)
                require(q.try_pop(output) && output == static_cast<int>(i), "FIFO");
            require(!q.try_pop(output), "empty after drain");
        }
    }
    for (const auto bad : {0U, 3U, 7U}) {
        bool threw = false;
        try { Q<int> q(bad); } catch (const std::invalid_argument&) { threw = true; }
        require(threw, "reject non-power-of-two");
    }
    bool too_large = false;
    try { Q<tracked> q(std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 2)); }
    catch (const std::length_error&) { too_large = true; }
    require(too_large, "reject allocation size overflow");
    {
        Q<std::unique_ptr<int>> q(1);
        auto a = std::make_unique<int>(42);
        require(q.try_push(std::move(a)) && !a, "move-only push");
        auto b = std::make_unique<int>(43);
        require(!q.try_push(std::move(b)) && b && *b == 43, "full must not move");
        require(q.try_pop(a) && *a == 42, "move-only pop");
    }
    {
        Q<tracked> q(2);
        tracked output(0);
        bool threw = false;
        try { (void)q.try_emplace(-1); } catch (const std::runtime_error&) { threw = true; }
        require(threw && !q.try_pop(output), "failed construction must not publish");
        require(q.try_emplace(5), "over-aligned emplacement");
        require(q.try_pop(output) && output.value == 5 && tracked::alive == 1, "destroy on pop");
        require(q.try_emplace(6) && q.try_emplace(7), "leave items for destructor");
    }
    require(tracked::alive == 0, "destroy remaining items exactly once");
}
struct payload { std::array<std::uint64_t, 8> words{}; };
template<class Q>
void stress_queue(const char* name, std::uint64_t items, std::size_t capacity) {
    Q q(capacity);
    std::barrier ready(2);
    bool valid = true; // Written by consumer, read only after join.
    const auto start = support::clock::now();
    std::thread consumer([&] {
        ready.arrive_and_wait();
        payload value;
        for (std::uint64_t i = 0; i < items; ++i) {
            while (!q.try_pop(value)) support::relax();
            for (std::size_t w = 0; w < value.words.size(); ++w)
                if (value.words[w] != (i ^ (0x9e3779b97f4a7c15ULL * w))) valid = false;
        }
    });
    ready.arrive_and_wait();
    for (std::uint64_t i = 0; i < items; ++i) {
        payload value;
        for (std::size_t w = 0; w < value.words.size(); ++w)
            value.words[w] = i ^ (0x9e3779b97f4a7c15ULL * w);
        while (!q.try_push(std::move(value))) support::relax();
    }
    consumer.join();
    require(valid, "sequence or payload corruption");
    payload value;
    require(!q.try_pop(value), "stress queue must be empty");
    std::cout << "PASS " << name << " items=" << items << " capacity=" << capacity
              << " seconds=" << std::chrono::duration<double>(support::clock::now() - start).count() << '\n';
}
template<spsc::detail::design D>
void overflow() {
    using Q = spsc::detail::queue<payload, D, std::uint8_t>;
    stress_queue<Q>("8-bit counter wrap", 100000, 64);
}
void unit_histogram() {
    histogram h;
    require(h.count() == 0 && h.percentile(50) == 0, "empty histogram");
    for (std::uint64_t i = 1; i <= 10000; ++i) h.record(i);
    require(h.percentile(50) >= 5000 && h.percentile(50) <= 5004, "histogram median");
    require(h.percentile(99) >= 9900 && h.percentile(99) <= 9908, "histogram p99");
    require(h.max() == 10000 && h.count() == 10000, "histogram exact count/max");
    for (const auto x : {0ULL, 1ULL, 1023ULL, 1024ULL, 2047ULL, 2048ULL, 4095ULL,
                         4096ULL, 999999999ULL, 0xffffffffffffffffULL}) {
        histogram single;
        single.record(x);
        require(single.percentile(100) == x, "histogram boundary/clamped max");
    }
    for (unsigned bit = 1; bit < 64; ++bit) {
        const auto x = std::uint64_t{1} << bit;
        histogram edge;
        edge.record(x - 1); edge.record(x); edge.record(x + 1);
        const auto median = edge.percentile(50);
        require(median >= x && median - x <= x / 1024 + 1, "histogram relative error");
    }
    histogram random_hist;
    std::mt19937_64 rng(42);
    std::vector<std::uint64_t> exact;
    for (unsigned i = 0; i < 100000; ++i) {
        const auto x = rng() >> (rng() % 64);
        exact.push_back(x); random_hist.record(x);
    }
    std::sort(exact.begin(), exact.end());
    for (const auto numerator : {5000U, 9000U, 9900U, 9990U, 9999U, 10000U}) {
        const auto rank = (exact.size() * numerator + 9999) / 10000;
        const auto expected = exact[rank - 1], actual = random_hist.percentile(numerator, 10000);
        require(actual >= expected && actual - expected <= expected / 1024, "histogram vs sorted reference");
    }
    for (const auto samples : {1000U, 10000U, 1000000U}) {
        histogram boundary;
        const auto slow = samples / 1000;
        for (unsigned i = 0; i < samples - slow; ++i) boundary.record(1);
        for (unsigned i = 0; i < slow; ++i) boundary.record(1000000);
        require(boundary.percentile(999, 1000) == 1, "exact p99.9 rank at an outlier boundary");
        require(boundary.percentile(100) == 1000000, "outlier maximum");
    }
}
} // namespace

int main(int argc, char** argv) try {
    std::uint64_t items = 100000000;
    std::size_t capacity = 1024;
    bool unit_only = false;
    std::uint64_t timeout = 600;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--unit") unit_only = true;
        else if (arg == "--items" && i + 1 < argc) items = support::number(argv[++i]);
        else if (arg == "--capacity" && i + 1 < argc) capacity = support::number(argv[++i]);
        else if (arg == "--timeout-seconds" && i + 1 < argc) timeout = support::number(argv[++i]);
        else throw std::invalid_argument("usage: spsc_tests [--unit] [--items N] [--capacity power-of-two] [--timeout-seconds N]");
    }
    if (!timeout || timeout > 86400) throw std::invalid_argument("timeout must be 1..86400 seconds");
    watchdog deadline(timeout);
    unit_queue<spsc::naive>(); unit_queue<spsc::acqrel>();
    unit_queue<spsc::padded>(); unit_queue<spsc::cached>(); unit_histogram();
    overflow<spsc::detail::design::naive>();
    overflow<spsc::detail::design::acqrel>();
    overflow<spsc::detail::design::padded>();
    overflow<spsc::detail::design::cached>();
    std::cout << "PASS unit tests (including concurrent counter overflow)\n";
    if (!unit_only) {
        stress_queue<spsc::naive<payload>>("naive", items, capacity);
        stress_queue<spsc::acqrel<payload>>("acqrel", items, capacity);
        stress_queue<spsc::padded<payload>>("padded", items, capacity);
        stress_queue<spsc::cached<payload>>("cached", items, capacity);
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
