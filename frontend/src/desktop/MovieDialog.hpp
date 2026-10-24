#pragma once

#include "session/Playback.hpp"
#include <filesystem>
#include <memory>
#include <optional>

namespace snes::frontend {
class MovieDialog {
  public:
    struct Action {
        enum class Kind { Record, RecordReset, Play, Stop, Save, Discard } kind;
        std::filesystem::path path;
    };

    MovieDialog();
    ~MovieDialog();
    void Open(std::filesystem::path directory, std::string name);
    bool Active() const noexcept;
    void Update(bool recording, bool replaying, bool pending, const char* failure, size_t queries);
    void Draw();
    std::optional<Action> TakeAction();
    void Completed();
    void Rejected(std::string error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
