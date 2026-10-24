#include "InputMovie.hpp"
#include "AtomicFile.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/detail/Sha256.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <stdexcept>

namespace snes::frontend {
namespace {
constexpr size_t headerBytes = 128, digestOffset = 96, sampleBytes = 24;
constexpr std::array<uint8_t, 8> magic{'C', 'U', 'P', 'I', 'D', 'M', 'V', 0};

void Put(std::span<uint8_t> out, size_t offset, uint64_t value, unsigned bytes) {
    for (unsigned byte = 0; byte < bytes; ++byte)
        out[offset + byte] = uint8_t(value >> (byte * 8));
}

uint64_t Get(std::span<const uint8_t> in, size_t offset, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < bytes; ++byte)
        value |= uint64_t(in[offset + byte]) << (byte * 8);
    return value;
}

std::array<uint8_t, sampleBytes> Encode(const InputJournal::Sample& sample) {
    std::array<uint8_t, sampleBytes> bytes{};
    Put(bytes, 0, sample.frame, 8);
    bytes[8] = uint8_t(sample.query);
    bytes[9] = uint8_t(sample.index);
    bytes[10] = uint8_t(sample.gun);
    Put(bytes, 12, sample.buttons, 4);
    Put(bytes, 16, uint32_t(sample.x), 4);
    Put(bytes, 20, uint32_t(sample.y), 4);
    return bytes;
}

InputJournal::Sample Decode(std::span<const uint8_t> bytes) {
    if (bytes[11])
        throw std::runtime_error("Invalid movie query flags");
    return {Get(bytes, 0, 8),
            InputJournal::Query(bytes[8]),
            int(bytes[9]),
            int(bytes[10]),
            uint32_t(Get(bytes, 12, 4)),
            std::bit_cast<int32_t>(uint32_t(Get(bytes, 16, 4))),
            std::bit_cast<int32_t>(uint32_t(Get(bytes, 20, 4)))};
}

void Sizes(size_t state, size_t samples) {
    if (state < 48 || state > InputMovie::maximumStateBytes || samples > InputMovie::maximumSamples)
        throw std::runtime_error("Input movie exceeds the supported size");
}

void ValidateRecording(const InputJournal::Recording& input,
                       const InputMovie::Descriptor& descriptor,
                       const core::StateOperation& operation) {
    if (descriptor.firstFrame > descriptor.lastFrame ||
        descriptor.firstClock > descriptor.lastClock)
        throw std::invalid_argument("Input movie has a backwards timeline");
    InputJournal::Validate(input, operation);
    for (size_t index = 0; index < input.size(); ++index) {
        if (index % 2048 == 0)
            operation.Report(core::StatePhase::Validate, index, input.size(), "Movie timeline");
        if (input[index].frame < descriptor.firstFrame || input[index].frame > descriptor.lastFrame)
            throw std::invalid_argument("Input movie query is outside its recorded timeline");
    }
    operation.Report(core::StatePhase::Validate, input.size(), input.size(), "Movie timeline");
}
} // namespace

InputMovie::InputMovie(std::vector<uint8_t> state,
                       std::shared_ptr<const InputJournal::Recording> input, Descriptor descriptor)
    : InputMovie(std::make_shared<const std::vector<uint8_t>>(std::move(state)), std::move(input),
                 descriptor) {
}

InputMovie::InputMovie(std::shared_ptr<const std::vector<uint8_t>> state,
                       std::shared_ptr<const InputJournal::Recording> input, Descriptor descriptor)
    : state_(std::move(state)), input_(std::move(input)), descriptor_(descriptor) {
    if (!state_ || !input_)
        throw std::invalid_argument("Input movie has no query sequence");
    Sizes(state_->size(), input_->size());
    if (descriptor.firstFrame > descriptor.lastFrame ||
        descriptor.firstClock > descriptor.lastClock)
        throw std::invalid_argument("Input movie has a backwards timeline");
}

void InputMovie::Validate(const core::StateOperation& operation) const {
    Sizes(state_->size(), input_->size());
    ValidateRecording(*input_, descriptor_, operation);
}

void InputMovie::Write(const std::filesystem::path& path,
                       const core::StateOperation& operation) const {
    const auto state = state_;
    const auto input = input_;
    Sizes(state->size(), input->size());
    Validate(operation);
    std::array<uint8_t, headerBytes> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    Put(header, 8, 2, 4);
    Put(header, 12, state->size(), 4);
    Put(header, 16, input->size(), 8);
    Put(header, 24, descriptor_.firstFrame, 8);
    Put(header, 32, descriptor_.lastFrame, 8);
    Put(header, 40, descriptor_.firstClock, 8);
    Put(header, 48, descriptor_.lastClock, 8);
    std::copy(descriptor_.cheats.begin(), descriptor_.cheats.end(), header.begin() + 56);
    const uint64_t size = headerBytes + state->size() + uint64_t(input->size()) * sampleBytes;
    const auto fill = [&](uint64_t offset, std::span<uint8_t> out) {
        while (!out.empty()) {
            size_t count;
            if (offset < headerBytes) {
                count = std::min(out.size(), headerBytes - size_t(offset));
                std::copy_n(header.data() + size_t(offset), count, out.data());
            } else if (offset - headerBytes < state->size()) {
                const auto index = size_t(offset - headerBytes);
                count = std::min(out.size(), state->size() - index);
                std::copy_n(state->data() + index, count, out.data());
            } else {
                const auto index = size_t(offset - headerBytes - state->size());
                const auto bytes = Encode((*input)[index / sampleBytes]);
                const auto within = index % sampleBytes;
                count = std::min(out.size(), sampleBytes - within);
                std::copy_n(bytes.data() + within, count, out.data());
            }
            offset += count;
            out = out.subspan(count);
        }
    };
    core::detail::Sha256Hasher hash;
    hash.Update(std::span(header).first(digestOffset));
    std::array<uint8_t, 65536> buffer;
    const auto label = operation ? core::PathText(path.filename()) : std::string{};
    for (uint64_t offset = headerBytes; offset < size;) {
        operation.Report(core::StatePhase::Capture, offset, size, label);
        const auto count = size_t(std::min<uint64_t>(buffer.size(), size - offset));
        auto bytes = std::span(buffer).first(count);
        fill(offset, bytes);
        hash.Update(bytes);
        offset += count;
    }
    operation.Report(core::StatePhase::Capture, size, size, label);
    const auto digest = hash.Finish();
    std::copy(digest.begin(), digest.end(), header.begin() + digestOffset);
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    WriteFileAtomically(path, size, fill, operation);
}

InputMovie InputMovie::Read(const std::filesystem::path& path,
                            const core::StateOperation& operation) {
    const auto label = operation ? core::PathText(path.filename()) : std::string{};
    operation.Report(core::StatePhase::ReadFile, 0, 0, label);
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, headerBytes> header;
    if (!file || !file.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        !std::equal(magic.begin(), magic.end(), header.begin()) || Get(header, 8, 4) != 2 ||
        Get(header, 88, 8) != 0)
        throw std::runtime_error("Unsupported or missing input movie: " + core::PathText(path));
    const auto stateSize = Get(header, 12, 4), count = Get(header, 16, 8);
    if (stateSize > maximumStateBytes || count > maximumSamples)
        throw std::runtime_error("Input movie exceeds the supported size");
    Sizes(size_t(stateSize), size_t(count));
    Descriptor descriptor{
        Get(header, 24, 8), Get(header, 32, 8), Get(header, 40, 8), Get(header, 48, 8), {}};
    std::copy_n(header.begin() + 56, descriptor.cheats.size(), descriptor.cheats.begin());
    if (descriptor.firstFrame > descriptor.lastFrame ||
        descriptor.firstClock > descriptor.lastClock)
        throw std::runtime_error("Input movie has a backwards timeline");
    const auto size = headerBytes + stateSize + count * sampleBytes;
    if (std::filesystem::file_size(path) != size)
        throw std::runtime_error("Invalid input movie length");
    std::vector<uint8_t> state(static_cast<size_t>(stateSize));
    auto input = std::make_shared<InputJournal::Recording>();
    input->reserve(size_t(count));
    core::detail::Sha256Hasher hash;
    hash.Update(std::span(header).first(digestOffset));
    uint64_t consumed = headerBytes;
    const auto read = [&](std::span<uint8_t> bytes) {
        operation.Report(core::StatePhase::ReadFile, consumed, size, label);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
            throw std::runtime_error("Cannot read input movie: " + core::PathText(path));
        hash.Update(bytes);
        consumed += bytes.size();
    };
    for (size_t offset = 0; offset < state.size(); offset += 65536)
        read(std::span(state).subspan(offset, std::min<size_t>(65536, state.size() - offset)));
    std::array<uint8_t, sampleBytes * 2048> buffer;
    for (size_t offset = 0; offset < size_t(count);) {
        const auto records = std::min<size_t>(2048, size_t(count) - offset);
        read(std::span(buffer).first(records * sampleBytes));
        for (size_t index = 0; index < records; ++index)
            input->push_back(Decode(std::span(buffer).subspan(index * sampleBytes, sampleBytes)));
        offset += records;
    }
    if (file.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Unexpected data after input movie");
    operation.Report(core::StatePhase::ReadFile, size, size, label);
    operation.Report(core::StatePhase::Validate, size, size, label);
    const auto digest = hash.Finish();
    if (!std::equal(digest.begin(), digest.end(), header.begin() + digestOffset))
        throw std::runtime_error("Input movie checksum does not match");
    ValidateRecording(*input, descriptor, operation);
    return InputMovie(std::move(state), std::move(input), descriptor);
}

} // namespace snes::frontend
