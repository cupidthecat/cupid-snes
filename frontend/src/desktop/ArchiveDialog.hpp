#pragma once

#include "session/GameAssets.hpp"
#include <memory>
#include <optional>

namespace snes::frontend {
class ArchiveDialog {
  public:
    struct Result {
        std::optional<std::string> member;
    };

    ArchiveDialog();
    ~ArchiveDialog();
    void Open(const ArchivePlan& plan);
    bool Active() const noexcept;
    void Draw();
    std::optional<Result> TakeResult();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
