#pragma once
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <pthread.h>
#if defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace support {
using clock = std::chrono::steady_clock;
static_assert(clock::is_steady);
inline std::uint64_t number(std::string_view text) {
    std::uint64_t n = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw std::invalid_argument("invalid unsigned number: " + std::string(text));
    return n;
}
inline std::uint64_t ns(clock::duration d) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
}
inline void relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}
// Return an error instead of throwing before a worker barrier (which would deadlock).
inline int configure_thread(int cpu, const std::string& qos) noexcept {
#if defined(__linux__)
    (void)qos;
    if (cpu < 0) return 0;
    if (cpu >= CPU_SETSIZE) return EINVAL;
    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(cpu, &requested);
    int error = pthread_setaffinity_np(pthread_self(), sizeof(requested), &requested);
    if (error != 0) return error;
    cpu_set_t actual;
    CPU_ZERO(&actual);
    error = pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual);
    if (error != 0) return error;
    return CPU_COUNT(&actual) == 1 && CPU_ISSET(cpu, &actual) ? 0 : EINVAL;
#elif defined(__APPLE__)
    if (cpu >= 0) return ENOTSUP;
    if (qos == "default") return 0;
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    (void)qos;
    return cpu < 0 ? 0 : ENOTSUP;
#endif
}
inline void check_thread_errors(int a, int b) {
    if (a != 0 || b != 0)
        throw std::system_error(a != 0 ? a : b, std::generic_category(), "thread placement/QoS");
}
} // namespace support
