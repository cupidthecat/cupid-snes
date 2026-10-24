#include "debug/RamSearch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

using snes::frontend::RamSearch;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Function> void Reject(Function run) {
    bool rejected = false;
    try {
        run();
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, "Invalid or cancelled search rejects");
}

void Narrowing() {
    RamSearch search;
    std::vector<uint8_t> first(131072, 0), next = first;
    first[8] = first[65536] = 17;
    search.Start(first, {});
    Check(search.Count() == first.size(), "Unknown initial search retains every byte");
    Check(search.Results(100000, 3).front().offset == 100000,
          "Paged results reach every candidate");
    search.Refine(first, {RamSearch::Relation::Equal, RamSearch::Operand::Constant, 17});
    Check(search.Count() == 2 && search.Results(0, 2)[1].offset == 65536,
          "A fixed value narrows candidates across the whole region");
    next = first;
    next[8] = 18;
    search.Refine(next, {RamSearch::Relation::Greater, RamSearch::Operand::Previous});
    auto result = search.Results(0, 1).front();
    Check(search.Count() == 1 && result.offset == 8 && result.current == 18 &&
              result.previous == 17 && result.initial == 17,
          "Previous comparisons retain current, previous and initial values");
    search.Undo();
    Check(search.Count() == 2 && search.Results(0, 1).front().current == 17,
          "Undo restores candidates and their comparison snapshots");
    search.Refine(next, {RamSearch::Relation::Equal, RamSearch::Operand::Initial, 0, 1});
    Check(search.Count() == 1 && search.Results(0, 1).front().offset == 8,
          "A change amount can compare against the initial capture");
    Reject([&] { search.Refine(std::span(first).first(1), {}); });
    Check(search.Count() == 1, "An incompatible region retains the search");
    Reject([&] {
        search.Refine(next, {}, [](size_t, size_t) { throw std::runtime_error("cancel"); });
    });
    Check(search.Count() == 1 && search.Results(0, 1).front().current == 18,
          "Cancelled refinement preserves every result");
    Reject([&] {
        search.Refine(first, {RamSearch::Relation::Equal, RamSearch::Operand::Constant, 0},
                      [](size_t done, size_t total) {
                          if (done == total)
                              throw std::runtime_error("cancel");
                      });
    });
    Check(search.Count() == 1 && search.Results(0, 1).front().current == 18,
          "Cancellation at the final progress update preserves candidates and samples");
    Reject([&] {
        search.Start(first, {}, [](size_t done, size_t) {
            if (done >= 4096)
                throw std::runtime_error("cancel");
        });
    });
    Check(search.Count() == 1, "Cancelled restart preserves the old search");
    search.Clear();
    Check(!search.Active() && !search.Count() && !search.CanUndo(),
          "Clear releases snapshots and history");
}

void Types() {
    const std::vector<uint8_t> bytes{0xff, 0xfe, 0x80, 0x7f, 0x12, 0x34, 0x56, 0x78};
    for (unsigned width = 1; width <= 4; ++width)
        for (bool big : {false, true})
            for (bool sign : {false, true})
                for (bool aligned : {false, true}) {
                    RamSearch search;
                    search.Start(bytes, {width, sign, big, aligned});
                    const auto results = search.Results(0, SIZE_MAX);
                    const auto stride = aligned ? width : 1;
                    Check(results.size() == (bytes.size() - width) / stride + 1,
                          "Width and alignment retain all complete values");
                    for (const auto& result : results) {
                        uint64_t expected = 0;
                        for (unsigned byte = 0; byte < width; ++byte)
                            expected = big ? (expected << 8) | bytes[result.offset + byte]
                                           : expected | (uint64_t(bytes[result.offset + byte])
                                                         << (byte * 8));
                        const int64_t value = sign && (expected & (uint64_t(1) << (width * 8 - 1)))
                                                  ? int64_t(expected) - (int64_t(1) << (width * 8))
                                                  : int64_t(expected);
                        Check(result.current == value,
                              "Signed values and byte order match an independent byte assembly");
                    }
                }
    for (unsigned width = 1; width <= 4; ++width) {
        const auto mask = (int64_t(1) << (width * 8)) - 1;
        Check(RamSearch::ParseValue("-1", {width, true}) == -1,
              "Signed decimal input retains its sign");
        Check(RamSearch::ParseValue(std::to_string(mask), {width}) == mask,
              "Unsigned maximum values are supported");
        Reject([&] { RamSearch::ParseValue(std::to_string(mask + 1), {width}); });
    }
    Check(RamSearch::ParseValue("$FF", {1, true}) == -1 &&
              RamSearch::ParseValue(" 0X80000000 ", {4, true}) == -2147483648ll,
          "Signed hexadecimal input accepts raw bit patterns");
    for (auto text : {"", "-1", "0xGG", "12x", "256", "1 2", "0x"})
        Reject([&] { RamSearch::ParseValue(text, {}); });
    RamSearch small;
    small.Start(std::span(bytes).first(2), {3});
    Check(small.Count() == 0 && small.Results(0, 1).empty(),
          "Incomplete values never read past a region");
    Reject([&] { small.Start(bytes, {0}); });
    Reject([&] { small.Start(bytes, {5}); });
}

void Comparisons() {
    using R = RamSearch::Relation;
    const std::vector<uint8_t> bytes{0, 1, 2, 3, 4};
    for (auto [relation, expected] : std::array<std::pair<R, size_t>, 6>{{{R::Equal, 1},
                                                                          {R::NotEqual, 4},
                                                                          {R::Less, 2},
                                                                          {R::LessEqual, 3},
                                                                          {R::Greater, 2},
                                                                          {R::GreaterEqual, 3}}}) {
        RamSearch search;
        search.Start(bytes, {});
        search.Refine(bytes, {relation, RamSearch::Operand::Constant, 2});
        Check(search.Count() == expected,
              "All six comparison operators select the expected values");
    }
    RamSearch search;
    search.Start(bytes, {});
    for (unsigned n = 0; n < 20; ++n)
        search.Refine(bytes, {R::Equal, RamSearch::Operand::Previous});
    unsigned undo = 0;
    while (search.CanUndo()) {
        search.Undo();
        ++undo;
    }
    Check(undo == 16, "Search history has a fixed maximum depth");
    Reject([&] { search.Undo(); });
    Reject([&] { search.Refine(bytes, {R(99)}); });
    Reject([&] { search.Refine(bytes, {R::Equal, RamSearch::Operand(99)}); });
}
} // namespace

int main() {
    try {
        Narrowing();
        Types();
        Comparisons();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
