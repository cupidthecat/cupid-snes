#pragma once
#include "debug/HardwareSnapshot.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <optional>

namespace snes::frontend {
class HardwareDialog {
  public:
    struct Export {
        std::string label;
        core::VideoFrame picture;
        std::string text;
    };

    explicit HardwareDialog(SDL_Renderer* renderer);
    ~HardwareDialog();
    void Open(HardwareSnapshot snapshot, bool audio);
    void Updated(HardwareSnapshot snapshot);
    bool Active() const;
    void Draw();
    bool TakeRefresh();
    bool TakeStep();
    std::optional<Export> TakeExport();
    void Message(std::string message);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace snes::frontend
