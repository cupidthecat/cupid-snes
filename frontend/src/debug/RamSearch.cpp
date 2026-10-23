#include "RamSearch.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace snes::frontend {
namespace {
bool Compare(int64_t value, int64_t target, RamSearch::Relation relation) {
    switch (relation) {
    case RamSearch::Relation::Equal:
        return value == target;
    case RamSearch::Relation::NotEqual:
        return value != target;
    case RamSearch::Relation::Less:
        return value < target;
    case RamSearch::Relation::LessEqual:
        return value <= target;
    case RamSearch::Relation::Greater:
        return value > target;
    case RamSearch::Relation::GreaterEqual:
        return value >= target;
    }
    throw std::invalid_argument("Invalid memory search comparison");
}

void Report(const RamSearch::Progress& progress, size_t done, size_t total) {
    if (progress)
        progress(done, total);
}
} // namespace

void RamSearch::Validate(Config config) {
    if (config.bytes < 1 || config.bytes > 4)
        throw std::invalid_argument("Memory searches support one through four bytes per value");
}

void RamSearch::Start(std::span<const uint8_t> bytes, Config config, const Progress& progress) {
    Validate(config);
    if (bytes.size() > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Memory region exceeds the supported search address range");
    Report(progress, 0, bytes.size());
    RamSearch next;
    next.config_ = config;
    next.initial_.assign(bytes.begin(), bytes.end());
    next.previous_ = next.current_ = next.initial_;
    next.candidates_.resize(bytes.size() / 64 + (bytes.size() % 64 != 0));
    const auto end = bytes.size() >= config.bytes ? bytes.size() - config.bytes + 1 : 0;
    const size_t stride = config.aligned ? config.bytes : 1;
    for (size_t offset = 0; offset < end; offset += stride) {
        if (!(offset & 4095))
            Report(progress, offset, bytes.size());
        next.candidates_[offset / 64] |= uint64_t(1) << (offset % 64);
        ++next.count_;
    }
    Report(progress, bytes.size(), bytes.size());
    next.active_ = true;
    *this = std::move(next);
}

int64_t RamSearch::Value(std::span<const uint8_t> bytes, size_t offset) const noexcept {
    uint32_t value = 0;
    for (unsigned byte = 0; byte < config_.bytes; ++byte) {
        const auto shift = config_.bigEndian ? config_.bytes - byte - 1 : byte;
        value |= uint32_t(bytes[offset + byte]) << (shift * 8);
    }
    const auto bits = config_.bytes * 8;
    if (config_.signedValues && (value & (uint32_t(1) << (bits - 1))))
        return int64_t(value) - (int64_t(1) << bits);
    return value;
}

void RamSearch::Refine(std::span<const uint8_t> bytes, Filter filter, const Progress& progress) {
    if (!active_ || bytes.size() != current_.size())
        throw std::invalid_argument("Start a new search for this memory region");
    if (filter.operand < Operand::Constant || filter.operand > Operand::Initial ||
        filter.relation < Relation::Equal || filter.relation > Relation::GreaterEqual ||
        filter.adjustment < -int64_t(UINT32_MAX) || filter.adjustment > int64_t(UINT32_MAX))
        throw std::invalid_argument("Invalid memory search filter");
    const auto bits = config_.bytes * 8;
    const auto minimum = config_.signedValues ? -(int64_t(1) << (bits - 1)) : 0;
    const auto maximum = (int64_t(1) << (bits - unsigned(config_.signedValues))) - 1;
    if (filter.operand == Operand::Constant && (filter.value < minimum || filter.value > maximum))
        throw std::invalid_argument("Search value is outside the selected byte width");
    Report(progress, 0, bytes.size());
    std::vector<uint8_t> next(bytes.begin(), bytes.end());
    auto candidates = candidates_;
    size_t count = 0;
    for (size_t word = 0; word < candidates.size(); ++word) {
        if (!(word & 63))
            Report(progress, word * 64, bytes.size());
        auto remaining = candidates[word];
        while (remaining) {
            const auto bit = unsigned(std::countr_zero(remaining));
            const auto offset = word * 64 + bit;
            auto target =
                filter.operand == Operand::Constant
                    ? filter.value
                    : Value(filter.operand == Operand::Previous ? current_ : initial_, offset);
            target += filter.adjustment;
            if (!Compare(Value(next, offset), target, filter.relation))
                candidates[word] &= ~(uint64_t(1) << bit);
            else
                ++count;
            remaining &= remaining - 1;
        }
    }
    Report(progress, bytes.size(), bytes.size());
    history_.push_back({current_, previous_, candidates_, count_});
    const uint64_t imageBytes =
        uint64_t(current_.size()) * 2 + uint64_t(candidates_.size()) * sizeof(uint64_t);
    const size_t limit =
        std::clamp<size_t>(imageBytes ? (64 * 1024 * 1024) / imageBytes : 16, 1, 16);
    if (history_.size() > limit)
        history_.erase(history_.begin());
    previous_.swap(current_);
    current_.swap(next);
    candidates_.swap(candidates);
    count_ = count;
}

void RamSearch::Undo() {
    if (history_.empty())
        throw std::runtime_error("There is no memory search to undo");
    auto& old = history_.back();
    current_.swap(old.current);
    previous_.swap(old.previous);
    candidates_.swap(old.candidates);
    count_ = old.count;
    history_.pop_back();
}

void RamSearch::Clear() noexcept {
    *this = RamSearch{};
}

std::vector<RamSearch::Result> RamSearch::Results(size_t first, size_t count) const {
    std::vector<Result> results;
    if (first >= count_ || !count)
        return results;
    results.reserve(std::min(count, count_ - first));
    for (size_t word = 0; word < candidates_.size(); ++word) {
        auto remaining = candidates_[word];
        const auto matches = size_t(std::popcount(remaining));
        if (first >= matches) {
            first -= matches;
            continue;
        }
        while (remaining) {
            const auto bit = unsigned(std::countr_zero(remaining));
            remaining &= remaining - 1;
            if (first) {
                --first;
                continue;
            }
            const auto offset = word * 64 + bit;
            results.push_back({uint32_t(offset), Value(current_, offset), Value(previous_, offset),
                               Value(initial_, offset)});
            if (results.size() == count)
                return results;
        }
    }
    return results;
}

int64_t RamSearch::ParseValue(std::string_view text, Config config) {
    Validate(config);
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        throw std::invalid_argument("Enter a memory search value");
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    int base = 10;
    if (text.starts_with('$')) {
        text.remove_prefix(1);
        base = 16;
    } else if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument(
            "Use a decimal value or hexadecimal value prefixed with 0x or $");
    const unsigned bits = config.bytes * 8;
    const auto mask = (int64_t(1) << bits) - 1;
    if (base == 16 && config.signedValues && value >= 0 && value <= mask &&
        (value & (int64_t(1) << (bits - 1))))
        value -= int64_t(1) << bits;
    const auto minimum = config.signedValues ? -(int64_t(1) << (bits - 1)) : 0;
    const auto maximum = config.signedValues ? mask >> 1 : mask;
    if (value < minimum || value > maximum)
        throw std::invalid_argument("Search value is outside the selected byte width");
    return value;
}
} // namespace snes::frontend
