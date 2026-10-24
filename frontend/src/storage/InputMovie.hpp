#pragma once

#include "input/InputJournal.hpp"
#include <array>
#include <filesystem>
#include <span>

namespace snes::frontend {

class InputMovie {
  public:
    struct Descriptor {
        uint64_t firstFrame = 0, lastFrame = 0, firstClock = 0, lastClock = 0;
        std::array<uint8_t, 32> cheats{};
        bool operator==(const Descriptor&) const = default;
    };

    static constexpr size_t maximumStateBytes = 128 * 1024 * 1024;
    static constexpr size_t maximumSamples = size_t(512) * 1024 * 1024 / 24;

    InputMovie(std::vector<uint8_t> state, std::shared_ptr<const InputJournal::Recording> input,
               Descriptor descriptor);
    InputMovie(std::shared_ptr<const std::vector<uint8_t>> state,
               std::shared_ptr<const InputJournal::Recording> input, Descriptor descriptor);
    void Validate(const core::StateOperation& operation = {}) const;

    std::span<const uint8_t> InitialState() const noexcept {
        return *state_;
    }

    std::shared_ptr<const std::vector<uint8_t>> ShareInitialState() const noexcept {
        return state_;
    }

    std::shared_ptr<const InputJournal::Recording> Inputs() const noexcept {
        return input_;
    }

    const Descriptor& Info() const noexcept {
        return descriptor_;
    }

    static InputMovie Read(const std::filesystem::path& path,
                           const core::StateOperation& operation = {});
    void Write(const std::filesystem::path& path, const core::StateOperation& operation = {}) const;

  private:
    std::shared_ptr<const std::vector<uint8_t>> state_;
    std::shared_ptr<const InputJournal::Recording> input_;
    Descriptor descriptor_;
};

} // namespace snes::frontend
