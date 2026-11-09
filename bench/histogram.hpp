#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

// HDR-style fixed relative precision, no allocation on record. Exact below 1024 ns;
// 1024 sub-buckets per binary octave thereafter. Quantiles are bucket upper bounds.
class histogram {
    static constexpr unsigned precision = 10;
    static constexpr std::size_t width = 1U << precision;
    std::array<std::uint64_t, 55 * width> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t max_ = 0;
    static std::size_t bucket(std::uint64_t x) noexcept {
        const unsigned bits = std::bit_width(x);
        const unsigned shift = bits > precision + 1 ? bits - precision - 1 : 0;
        return shift == 0 ? static_cast<std::size_t>(x)
                          : (shift + 1) * width + static_cast<std::size_t>((x >> shift) - width);
    }
    static std::uint64_t upper(std::size_t index) noexcept {
        if (index < 2 * width) return index;
        const auto shift = static_cast<unsigned>(index / width - 1);
        const std::uint64_t lower = width + index % width;
        return ((lower + 1) << shift) - 1; // Unsigned wrap at UINT64_MAX is intentional.
    }
public:
    void record(std::uint64_t ns) noexcept {
        ++counts_[bucket(ns)];
        ++count_;
        if (ns > max_) max_ = ns;
    }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    // Exact fraction: percentile(99) = 99/100, percentile(999, 1000) = p99.9.
    [[nodiscard]] std::uint64_t percentile(std::uint32_t numerator, std::uint32_t denominator = 100) const {
        if (denominator == 0 || numerator == 0 || numerator > denominator)
            throw std::invalid_argument("quantile fraction must be in (0,1]");
        if (count_ == 0) return 0;
        // ceil(count * numerator / denominator), without a uint64 product overflow.
        const auto rank = (count_ / denominator) * numerator
            + ((count_ % denominator) * numerator + denominator - 1) / denominator;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= rank) return upper(i) < max_ ? upper(i) : max_;
        }
        return max_;
    }
};
