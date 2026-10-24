#include "snes/core/detail/Sha256.hpp"
#include "storage/AtomicFile.hpp"
#include "storage/InputMovie.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace snes::core;
using namespace snes::frontend;
namespace fs = std::filesystem;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class F> void Reject(F perform) {
    try {
        perform();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Malformed movie or failed writer was accepted");
}

template <class F> void Cancelled(F perform) {
    try {
        perform();
    } catch (const StateCancelled&) {
        return;
    }
    throw std::runtime_error("Movie cancellation did not propagate distinctly");
}

void RawWrite(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    Check(bool(file), "Write movie fixture");
}

void Put(std::vector<uint8_t>& out, size_t offset, uint64_t value, unsigned bytes) {
    for (unsigned byte = 0; byte < bytes; ++byte)
        out[offset + byte] = uint8_t(value >> (byte * 8));
}

void RepairDigest(std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> input(bytes.begin(), bytes.begin() + 96);
    input.insert(input.end(), bytes.begin() + 128, bytes.end());
    const auto digest = detail::Sha256(input);
    std::copy(digest.begin(), digest.end(), bytes.begin() + 96);
}

std::vector<uint8_t> State(size_t size) {
    std::vector<uint8_t> state(size);
    for (size_t index = 0; index < size; ++index)
        state[index] = uint8_t(index * 17 + 53);
    return state;
}

std::shared_ptr<const InputJournal::Recording> Queries(size_t count) {
    auto samples = std::make_shared<InputJournal::Recording>();
    for (size_t index = 0; index < count; ++index) {
        InputJournal::Sample sample{index / 8, InputJournal::Query(index % 6)};
        if (sample.query == InputJournal::Query::Controller)
            sample.index = int(index % 8);
        if (sample.query == InputJournal::Query::Primary ||
            sample.query == InputJournal::Query::Controller)
            sample.buttons = uint32_t(index & 4095);
        else if (sample.query == InputJournal::Query::Mouse ||
                 sample.query == InputJournal::Query::LightGun) {
            sample.index = int(index % 2);
            sample.x = std::numeric_limits<int32_t>::min() + int32_t(index);
            sample.y = std::numeric_limits<int32_t>::max() - int32_t(index);
            sample.buttons =
                uint32_t(index & (sample.query == InputJournal::Query::Mouse ? 3 : 63));
            if (sample.query == InputJournal::Query::LightGun)
                sample.gun = int((index / 2) % 2);
        } else {
            sample.x = std::numeric_limits<int32_t>::min() + int32_t(index);
            sample.y = std::numeric_limits<int32_t>::max() - int32_t(index);
        }
        samples->push_back(sample);
    }
    return samples;
}

InputMovie::Descriptor Metadata(size_t count) {
    InputMovie::Descriptor descriptor;
    descriptor.lastFrame = count / 8 + 9;
    descriptor.firstClock = 0x0102030405060708ull;
    descriptor.lastClock = descriptor.firstClock + 100000;
    for (size_t index = 0; index < descriptor.cheats.size(); ++index)
        descriptor.cheats[index] = uint8_t(index * 17);
    return descriptor;
}

void NoTemporary(const fs::path& path) {
    const auto prefix = path.filename().native() + fs::path(".tmp-").native();
    for (const auto& entry : fs::directory_iterator(path.parent_path()))
        Check(!entry.path().filename().native().starts_with(prefix),
              "Remove only owned temporary files");
}

void RoundTrip(const fs::path& path) {
    for (size_t bytes : {48, 65535, 65536, 65537, 131071})
        for (size_t count : {0, 1, 2047, 2048, 2049}) {
            const auto state = State(bytes);
            const auto input = Queries(count);
            InputMovie movie(state, input, Metadata(count));
            movie.Write(path);
            const auto decoded = InputMovie::Read(path);
            Check(std::equal(state.begin(), state.end(), decoded.InitialState().begin(),
                             decoded.InitialState().end()) &&
                      *decoded.Inputs() == *input && decoded.Info() == Metadata(count),
                  "Movie round trip preserves unaligned states and every query byte");
            auto encoded = ReadFile(path, 2 * 1024 * 1024);
            Check(encoded.size() == 128 + bytes + count * 24 && encoded[8] == 2 &&
                      encoded[24] == 0 && encoded[40] == 8 && encoded[47] == 1 &&
                      encoded[57] == 17 && encoded[87] == 15 && encoded[88] == 0,
                  "Movie uses the exact fixed envelope and record length");
            const auto original = encoded;
            RepairDigest(encoded);
            Check(encoded == original, "Independent full movie checksum matches");
            if (count > 3) {
                const size_t record = 128 + bytes + 2 * 24;
                Check(encoded[record + 8] == 2 && encoded[record + 9] == 0 &&
                          encoded[record + 11] == 0 && encoded[record + 16] == 2 &&
                          encoded[record + 19] == 0x80,
                      "Record layout preserves type, port, reserved byte and signed coordinates");
            }
        }
    Reject([&] { InputMovie movie(State(47), Queries(0), Metadata(0)); });
    Reject([&] { InputMovie movie(State(48), {}, Metadata(0)); });
    auto backwards = Metadata(0);
    backwards.firstFrame = backwards.lastFrame + 1;
    Reject([&] { InputMovie movie(State(48), Queries(0), backwards); });
    backwards = Metadata(0);
    backwards.lastClock = backwards.firstClock - 1;
    Reject([&] { InputMovie movie(State(48), Queries(0), backwards); });
    InputMovie(State(65537), Queries(4099), Metadata(4099)).Write(path);
    const auto valid = ReadFile(path, 2 * 1024 * 1024);
    for (const auto offset :
         {size_t(0), size_t(8), size_t(12), size_t(16), size_t(24), size_t(32), size_t(40),
          size_t(48), size_t(56), size_t(88), size_t(96), size_t(128), valid.size() - 1}) {
        auto corrupt = valid;
        corrupt[offset] ^= 0x80;
        RawWrite(path, corrupt);
        Reject([&] { InputMovie::Read(path); });
    }
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
        auto corrupt = valid;
        const size_t base = 128 + 65537;
        if (mutation == 0)
            Put(corrupt, 16, InputMovie::maximumSamples + 1, 8);
        else if (mutation == 1)
            Put(corrupt, 12, InputMovie::maximumStateBytes + 1, 4);
        else if (mutation == 2)
            corrupt[base + 11] = 1;
        else if (mutation == 3)
            corrupt[base + 8] = 255;
        else if (mutation == 4)
            Put(corrupt, base + 12, 4096, 4);
        else if (mutation == 5)
            corrupt[base + 9] = 1;
        else if (mutation == 6)
            Put(corrupt, base + 100 * 24, 0, 8);
        else if (mutation == 7)
            corrupt[24] = 1;
        else if (mutation == 8)
            Put(corrupt, 32, 0, 8);
        else if (mutation == 9)
            Put(corrupt, 48, 1, 8);
        else if (mutation == 10)
            corrupt[88] = 1;
        else
            Put(corrupt, base + 4 * 24 + 12, 1, 4);
        RepairDigest(corrupt);
        RawWrite(path, corrupt);
        Reject([&] { InputMovie::Read(path); });
    }
    for (unsigned mutation = 0; mutation < 2; ++mutation) {
        auto corrupt = valid;
        if (mutation)
            corrupt.pop_back();
        else
            corrupt.push_back(0);
        RawWrite(path, corrupt);
        Reject([&] { InputMovie::Read(path); });
    }
    RawWrite(path, valid);
}

void Cancellation(const fs::path& path) {
    InputMovie movie(State(131073), Queries(8193), Metadata(8193));
    const std::vector<uint8_t> prior{1, 2, 3, 4};
    RawWrite(path, prior);
    for (auto phase : {StatePhase::Validate, StatePhase::Capture, StatePhase::WriteFile})
        for (bool final : {false, true}) {
            const StateOperation operation([&](const StateProgress& progress) {
                return progress.phase != phase || !progress.total ||
                       (final ? progress.completed != progress.total : progress.completed == 0);
            });
            Cancelled([&] { movie.Write(path, operation); });
            Check(ReadFile(path, 1024) == prior && movie.Inputs()->size() == 8193 &&
                      movie.InitialState().size() == 131073,
                  "Cancelled movie write preserves destination and the owned retry buffer");
            NoTemporary(path);
        }
    movie.Write(path);
    const auto encoded = ReadFile(path, 1024 * 1024);
    for (auto phase : {StatePhase::ReadFile, StatePhase::Validate})
        for (bool final : {false, true}) {
            const StateOperation operation([&](const StateProgress& progress) {
                return progress.phase != phase || !progress.total ||
                       (final ? progress.completed != progress.total : progress.completed == 0);
            });
            Cancelled([&] { InputMovie::Read(path, operation); });
            Check(ReadFile(path, 1024 * 1024) == encoded,
                  "Cancelled reads retain the complete source");
        }
    Check(*InputMovie::Read(path).Inputs() == *movie.Inputs(),
          "A cancelled movie operation can retry");
}

void Producer(const fs::path& path) {
    const std::vector<uint8_t> prior{9, 8, 7};
    RawWrite(path, prior);
    Reject([&] { WriteFileAtomically(path, 3, {}); });
    Reject([&] {
        WriteFileAtomically(path, 131073, [](uint64_t offset, std::span<uint8_t> out) {
            if (offset)
                throw std::runtime_error("Producer failed");
            std::fill(out.begin(), out.end(), 0x17);
        });
    });
    Check(ReadFile(path, 1024) == prior, "A producer failure leaves the prior file intact");
    NoTemporary(path);
    const StateOperation finalCancel([](const StateProgress& progress) {
        return progress.phase != StatePhase::WriteFile || progress.completed != progress.total;
    });
    Cancelled([&] {
        WriteFileAtomically(
            path, 131073,
            [](uint64_t, std::span<uint8_t> out) { std::fill(out.begin(), out.end(), 7); },
            finalCancel);
    });
    Check(ReadFile(path, 1024) == prior,
          "The final streamed replacement guard retains the prior file");
    NoTemporary(path);
    WriteFileAtomically(path, 131073, [](uint64_t offset, std::span<uint8_t> out) {
        for (size_t index = 0; index < out.size(); ++index)
            out[index] = uint8_t((offset + index) * 17);
    });
    const auto bytes = ReadFile(path, 131073);
    for (size_t index = 0; index < bytes.size(); ++index)
        Check(bytes[index] == uint8_t(index * 17),
              "Streamed writer fills every byte of each chunk");
    WriteFileAtomically(path, 0, [](uint64_t, std::span<uint8_t>) {
        throw std::runtime_error("Empty producer must not run");
    });
    Check(fs::file_size(path) == 0, "Streamed writer supports empty files");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        fs::temp_directory_path() /
        ("cupid-input-movie-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    struct Cleanup {
        fs::path path;

        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{directory};

    try {
        fs::create_directories(directory);
        const auto path = directory / fs::path(u8"\u5f55\u5236.cmv");
        RoundTrip(path);
        Cancellation(path);
        Producer(path);
        const InputMovie portable(State(65537), Queries(4099), Metadata(4099));
        if (argc > 1)
            portable.Write(fs::path(argv[1]));
        if (argc > 2) {
            const auto imported = InputMovie::Read(fs::path(argv[2]));
            Check(*imported.Inputs() == *portable.Inputs() && imported.Info() == portable.Info() &&
                      std::equal(imported.InitialState().begin(), imported.InitialState().end(),
                                 portable.InitialState().begin(), portable.InitialState().end()),
                  "Import the other platform's complete movie");
        }
        std::puts(
            "Input movie checks passed: wire bytes, boundaries, cancellation, retry and atomic "
            "producers");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
