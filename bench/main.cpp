#include "spsc/naive.hpp"
#include "spsc/padded.hpp"
#include "spsc/cached.hpp"
#include "spsc/acqrel.hpp"
#include "histogram.hpp"
#include "support.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <bit>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <vector>

#ifndef SPSC_BUILD_FLAGS
#define SPSC_BUILD_FLAGS "not recorded; retain the compiler command"
#endif

namespace {
struct options {
    std::uint64_t items = 10000000;
    std::uint64_t samples = 1000000;
    std::uint64_t warmup = 100000;
    std::uint64_t seed = 20261108;
    unsigned repeats = 3;
    std::vector<std::size_t> capacities{64, 1024, 65536};
    int cpu_a = -1;
    int cpu_b = -1;
#if defined(__APPLE__)
    std::string qos = "interactive";
#else
    std::string qos = "default";
#endif
    std::filesystem::path output = "results/local";
};
template<std::size_t Bytes>
struct message {
    std::array<std::uint64_t, Bytes / 8> words{};
    explicit message(std::uint64_t sequence = 0) noexcept {
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = sequence ^ (0x9e3779b97f4a7c15ULL * i);
    }
    bool valid(std::uint64_t sequence) const noexcept {
        bool ok = true;
        for (std::size_t i = 0; i < words.size(); ++i)
            ok &= words[i] == (sequence ^ (0x9e3779b97f4a7c15ULL * i));
        return ok;
    }
};
static_assert(sizeof(message<8>) == 8 && sizeof(message<64>) == 64);
struct result {
    std::string implementation;
    std::size_t capacity;
    std::size_t bytes;
    unsigned repeat;
    double seconds = 0;
    std::uint64_t throughput_errors = 0;
    histogram latency;
};

template<template<class> class Q, std::size_t Bytes>
void throughput(const options& o, result& r) {
    Q<message<Bytes>> queue(r.capacity);
    int error_a = 0, error_b = 0;
    std::uint64_t errors = 0;
    support::clock::time_point start;
    // Completion timestamps after both threads are ready, before either can proceed.
    std::barrier prepared(2);
    std::barrier timed(2, [&]() noexcept { start = support::clock::now(); });
    std::thread consumer([&] {
        error_b = support::configure_thread(o.cpu_b, o.qos);
        prepared.arrive_and_wait();
        if (error_a || error_b) return;
        message<Bytes> value;
        for (std::uint64_t i = 0; i < o.warmup; ++i) {
            while (!queue.try_pop(value)) support::relax();
            if (!value.valid(i)) ++errors;
        }
        timed.arrive_and_wait();
        for (std::uint64_t i = 0; i < o.items; ++i) {
            while (!queue.try_pop(value)) support::relax();
            if (!value.valid(i)) ++errors;
        }
        r.seconds = std::chrono::duration<double>(support::clock::now() - start).count();
    });
    error_a = support::configure_thread(o.cpu_a, o.qos);
    prepared.arrive_and_wait();
    if (!error_a && !error_b) {
        for (std::uint64_t i = 0; i < o.warmup; ++i) {
            message<Bytes> value(i);
            while (!queue.try_push(std::move(value))) support::relax();
        }
        timed.arrive_and_wait();
        for (std::uint64_t i = 0; i < o.items; ++i) {
            message<Bytes> value(i);
            while (!queue.try_push(std::move(value))) support::relax();
        }
    }
    consumer.join();
    support::check_thread_errors(error_a, error_b);
    r.throughput_errors = errors;
    if (errors) throw std::runtime_error("throughput sequence/payload mismatch");
}

template<template<class> class Q, std::size_t Bytes>
void latency(const options& o, result& r) {
    Q<message<Bytes>> request(r.capacity), response(r.capacity);
    int error_a = 0, error_b = 0;
    bool valid = true;
    std::barrier prepared(2);
    std::thread reflector([&] {
        error_b = support::configure_thread(o.cpu_b, o.qos);
        prepared.arrive_and_wait();
        if (error_a || error_b) return;
        message<Bytes> value;
        for (std::uint64_t i = 0; i < o.warmup + o.samples; ++i) {
            while (!request.try_pop(value)) support::relax();
            while (!response.try_push(std::move(value))) support::relax();
        }
    });
    error_a = support::configure_thread(o.cpu_a, o.qos);
    prepared.arrive_and_wait();
    if (!error_a && !error_b) {
        message<Bytes> reply;
        for (std::uint64_t i = 0; i < o.warmup; ++i) {
            message<Bytes> value(i);
            while (!request.try_push(std::move(value))) support::relax();
            while (!response.try_pop(reply)) support::relax();
            valid &= reply.valid(i);
        }
        for (std::uint64_t i = 0; i < o.samples; ++i) {
            message<Bytes> value(i);
            const auto start = support::clock::now();
            while (!request.try_push(std::move(value))) support::relax();
            while (!response.try_pop(reply)) support::relax();
            const auto elapsed = support::ns(support::clock::now() - start);
            r.latency.record(elapsed); // Preserve full RTT ns; divide only when reporting.
            valid &= reply.valid(i); // Validate all bytes outside the measured RTT.
        }
    }
    reflector.join();
    support::check_thread_errors(error_a, error_b);
    if (!valid) throw std::runtime_error("ping-pong sequence/payload mismatch");
}

template<std::size_t Bytes>
void run(const options& o, result& r) {
    if (r.implementation == "naive") {
        throughput<spsc::naive, Bytes>(o, r); latency<spsc::naive, Bytes>(o, r);
    } else if (r.implementation == "acqrel") {
        throughput<spsc::acqrel, Bytes>(o, r); latency<spsc::acqrel, Bytes>(o, r);
    } else if (r.implementation == "padded") {
        throughput<spsc::padded, Bytes>(o, r); latency<spsc::padded, Bytes>(o, r);
    } else {
        throughput<spsc::cached, Bytes>(o, r); latency<spsc::cached, Bytes>(o, r);
    }
}
int cpu_number(std::string_view arg) {
    const auto n = support::number(arg);
    if (n > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("CPU number too large");
    return static_cast<int>(n);
}
options parse(int argc, char** argv) {
    options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "spsc_bench [--items N] [--samples N] [--warmup N] [--repeats N]\n"
                         "  [--capacities 64,1024,65536] [--seed N] [--output DIR]\n"
                         "  [--cpu-a N --cpu-b N] [--qos interactive|default]\n"
                         "Each sweep includes naive/acqrel/padded/cached and 8/64-byte messages.\n";
            std::exit(0);
        }
        if (i + 1 == argc) throw std::invalid_argument("missing value for " + arg);
        const std::string value = argv[++i];
        if (arg == "--items") o.items = support::number(value);
        else if (arg == "--samples") o.samples = support::number(value);
        else if (arg == "--warmup") o.warmup = support::number(value);
        else if (arg == "--seed") o.seed = support::number(value);
        else if (arg == "--repeats") {
            const auto n = support::number(value);
            if (n > 1000) throw std::invalid_argument("repeats must be <= 1000");
            o.repeats = static_cast<unsigned>(n);
        } else if (arg == "--cpu-a") o.cpu_a = cpu_number(value);
        else if (arg == "--cpu-b") o.cpu_b = cpu_number(value);
        else if (arg == "--qos") o.qos = value;
        else if (arg == "--output") o.output = value;
        else if (arg == "--capacities") {
            o.capacities.clear();
            std::istringstream parts(value);
            std::string part;
            while (std::getline(parts, part, ',')) {
                const auto capacity = support::number(part);
                if (!std::has_single_bit(capacity) || capacity > std::numeric_limits<std::size_t>::max() / 2)
                    throw std::invalid_argument("capacities must be powers of two <= size_t_max/2");
                o.capacities.push_back(static_cast<std::size_t>(capacity));
            }
            if (value.empty() || value.back() == ',') throw std::invalid_argument("empty capacity");
        } else throw std::invalid_argument("unknown option " + arg);
    }
    if (!o.items || !o.samples || !o.repeats || o.capacities.empty())
        throw std::invalid_argument("items, samples, repeats and capacities must be nonzero");
    if (o.warmup > std::numeric_limits<std::uint64_t>::max() - o.samples)
        throw std::invalid_argument("warmup + samples overflows");
    if ((o.cpu_a < 0) != (o.cpu_b < 0) || (o.cpu_a >= 0 && o.cpu_a == o.cpu_b))
        throw std::invalid_argument("select two distinct CPUs, or omit both");
    if (o.qos != "interactive" && o.qos != "default") throw std::invalid_argument("unknown QoS");
#if !defined(__APPLE__)
    if (o.qos != "default") throw std::invalid_argument("QoS selection is only implemented on macOS");
#endif
#if !defined(__linux__)
    if (o.cpu_a >= 0) throw std::invalid_argument("CPU pinning is only implemented on Linux");
#endif
    return o;
}
std::string placement(const options& o) {
#if defined(__APPLE__)
    return "laptop run; no pinning; macOS QoS=" + o.qos;
#elif defined(__linux__)
    return o.cpu_a < 0 ? "Linux; unpinned" : "Linux; verified affinity CPU " + std::to_string(o.cpu_a)
        + " / " + std::to_string(o.cpu_b);
#else
    (void)o;
    return "unpinned";
#endif
}
void metadata(std::ostream& out, const options& o, int argc, char** argv) {
    utsname machine{};
    if (uname(&machine) != 0) throw std::runtime_error("uname failed");
    const auto wall_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    out << "# SPSC benchmark\n\n" << placement(o) << "\n\n"
        << "Started (UTC): " << std::put_time(std::gmtime(&wall_time), "%Y-%m-%dT%H:%M:%SZ") << "\n\n"
        << "Machine: " << machine.nodename << "; " << machine.sysname << ' ' << machine.release
        << ' ' << machine.machine << "; hardware_concurrency=" << std::thread::hardware_concurrency()
        << ". See machine.txt when supplied by the runner.\n\n"
        << "Compiler: " << __VERSION__ << "\n\nFlags: `" << SPSC_BUILD_FLAGS << "`\n\n"
        << "Cache-line separation: " << spsc::cache_line << " bytes. C++: " << __cplusplus << ".\n\n"
        << "Command: `";
    for (int i = 0; i < argc; ++i) out << (i ? " " : "") << argv[i];
    out << "`\n\nSeed: " << o.seed << ". Cases shuffled with a specified Fisher-Yates/mt19937_64 mapping.\n"
        << "Each trial: " << o.items << " throughput items, " << o.samples << " RTT samples, "
        << o.warmup << " warmup operations per test; repeats=" << o.repeats << ".\n\n"
        << "Latency is closed-loop RTT/2 (ns), including clock overhead, spinning and scheduling. "
           "It is not measured one-way latency. No overhead subtraction or coordinated-omission correction. "
           "Histogram quantiles are bucket upper bounds (<0.098% relative error above 2047 ns); "
           "max is the largest observed full RTT in integer ns, divided by two. Validation/histogram recording are outside RTT, "
           "but affect offered load. Throughput includes generation and full-payload sequence checking, "
           "and ends after the last pop; thread creation and warmup are excluded.\n\n";
#if defined(__SANITIZE_THREAD__)
    out << "WARNING: ThreadSanitizer build; not a performance result.\n\n";
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    out << "WARNING: ThreadSanitizer build; not a performance result.\n\n";
#endif
#endif
}
} // namespace

int main(int argc, char** argv) try {
    const auto o = parse(argc, argv);
    support::check_thread_errors(support::configure_thread(o.cpu_a, o.qos), 0);
    std::filesystem::create_directories(o.output);
    // Refuse to silently replace an earlier measurement.
    if (std::filesystem::exists(o.output / "summary.md") || std::filesystem::exists(o.output / "measurements.csv"))
        throw std::runtime_error("output already contains results; choose a fresh directory");
    std::ofstream csv(o.output / "measurements.csv"), summary(o.output / "summary.md");
    csv.exceptions(std::ios::badbit | std::ios::failbit);
    summary.exceptions(std::ios::badbit | std::ios::failbit);
    metadata(summary, o, argc, argv);
    histogram clock_cost;
    for (unsigned i = 0; i < 100000; ++i) {
        const auto start = support::clock::now();
        clock_cost.record(support::ns(support::clock::now() - start));
    }
    summary << "Back-to-back steady_clock calls (full elapsed ns, 100000 samples): p50="
            << clock_cost.percentile(50) << ", p99=" << clock_cost.percentile(99)
            << ", max=" << clock_cost.max() << ". Clock period=" << support::clock::period::num
            << '/' << support::clock::period::den << " s (period is not measured resolution).\n\n"
            << "All rows are individual trials in execution order; do not pool their percentiles.\n\n"
            << "| Design | Capacity | Bytes | Trial | M items/s | p50 ns | p90 | p99 | p99.9 | p99.99 | max |\n"
            << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    csv << "implementation,capacity,element_bytes,trial,items,throughput_seconds,items_per_second,samples,"
           "p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,seed,cpu_a,cpu_b,qos,cache_line_bytes\n";
    // Keep only small case descriptors: each histogram is ~440 KiB.
    struct testcase { std::string name; std::size_t capacity, bytes; unsigned repeat; };
    std::vector<testcase> cases;
    for (unsigned repeat = 1; repeat <= o.repeats; ++repeat)
        for (const auto capacity : o.capacities)
            for (const auto bytes : {8U, 64U})
                for (const auto* name : {"naive", "acqrel", "padded", "cached"}) cases.push_back({name, capacity, bytes, repeat});
    std::mt19937_64 rng(o.seed);
    for (std::size_t n = cases.size(); n > 1; --n) {
        // Rejection avoids modulo bias and does not depend on std::shuffle implementation.
        const auto bound = static_cast<std::uint64_t>(n);
        const auto threshold = (std::uint64_t{0} - bound) % bound;
        std::uint64_t random;
        do { random = rng(); } while (random < threshold);
        std::swap(cases[n - 1], cases[random % bound]);
    }
    std::cout << placement(o) << '\n';
    for (const auto& c : cases) {
        result r{c.name, c.capacity, c.bytes, c.repeat, 0, 0, {}};
        if (r.bytes == 8) run<8>(o, r); else run<64>(o, r);
        const auto& h = r.latency;
        if (h.count() != o.samples) throw std::runtime_error("incorrect sample count");
        const double rate = static_cast<double>(o.items) / r.seconds;
        csv << std::setprecision(12) << r.implementation << ',' << r.capacity << ',' << r.bytes << ',' << r.repeat
            << ',' << o.items << ',' << r.seconds << ',' << rate << ',' << h.count()
            << ',' << h.percentile(50) / 2.0 << ',' << h.percentile(90) / 2.0 << ',' << h.percentile(99) / 2.0
            << ',' << h.percentile(999, 1000) / 2.0 << ',' << h.percentile(9999, 10000) / 2.0 << ',' << h.max() / 2.0
            << ',' << o.seed << ',' << o.cpu_a << ',' << o.cpu_b << ',' << o.qos << ',' << spsc::cache_line << '\n';
        summary << "| " << r.implementation << " | " << r.capacity << " | " << r.bytes << " | " << r.repeat
                << " | " << std::fixed << std::setprecision(3) << rate / 1e6 << " | " << h.percentile(50) / 2.0
                << " | " << h.percentile(90) / 2.0 << " | " << h.percentile(99) / 2.0 << " | " << h.percentile(999, 1000) / 2.0
                << " | " << h.percentile(9999, 10000) / 2.0 << " | " << h.max() / 2.0 << " |\n";
        csv.flush(); summary.flush();
        std::cout << r.implementation << " cap=" << r.capacity << " bytes=" << r.bytes << " trial=" << r.repeat
                  << " Mitems/s=" << rate / 1e6 << " p50/p99/p99.9=" << h.percentile(50) / 2.0 << '/'
                  << h.percentile(99) / 2.0 << '/' << h.percentile(999, 1000) / 2.0 << " ns\n" << std::flush;
    }
    summary << "\nRun complete: " << cases.size() << " trials.\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
