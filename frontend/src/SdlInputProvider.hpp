// SDL3 input provider interface.

#pragma once
// Maps host controls to console devices. Pump SDL events before polling.

#include "VideoGeometry.hpp"
#include "input/InputBindings.hpp"
#include "snes/core/Platform.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <vector>

namespace snes::frontend {

class SdlInputProvider final : public snes::core::IInputProvider {
public:
  using KeyMap = KeyboardMap;
  struct Gamepad {
    SDL_JoystickID id = 0;
    std::string name, key;
  };
  static std::vector<Gamepad> ConnectedGamepads();

  explicit SdlInputProvider();
  explicit SdlInputProvider(const KeyMap &keyMap);
  explicit SdlInputProvider(const InputBindings &bindings);
  ~SdlInputProvider() override;

  // IInputProvider
  snes::core::InputState Poll(uint64_t frameIndex) override;
  snes::core::InputState PollController(int player, uint64_t frameIndex) override;
  snes::core::MouseState PollMouse(int port, uint64_t frameIndex) override;
  snes::core::LightGunState PollLightGun(int port, int gun, uint64_t frameIndex) override;
  void SetGunViewport(VideoViewport viewport, bool focused) {
    gunViewport_ = viewport;
    gunFocused_ = focused;
  }

  void SetBindings(const InputBindings &bindings);
  void AdvanceFrame() noexcept {
    turboPhase_ = (turboPhase_ + 1) % (2 * unsigned(bindings_.turboFrequency));
  }
  void SetEnabled(bool enabled) noexcept {
    if (enabled_ == enabled)
      return;
    enabled_ = enabled;
    ResetSampling();
  }
  void ResetSampling() noexcept {
    SDL_GetRelativeMouseState(nullptr, nullptr);
    mouseState_ = {};
    mouseFractionX_ = mouseFractionY_ = 0;
    lastMouseFrame_ = lastMotionFrame_ = lastDeviceFrame_ = uint64_t(-1);
    lastGunFrame_.fill(uint64_t(-1));
  }
  void SetUiCapture(bool keyboard, bool pointer) noexcept {
    if (captureKeyboard_ == keyboard && capturePointer_ == pointer)
      return;
    captureKeyboard_ = keyboard;
    capturePointer_ = pointer;
    ResetSampling();
  }

private:
  struct FrameRange {
    uint64_t first = 0;
    uint64_t last = 0;
  };

  enum class Button : size_t {
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    X,
    Y,
    L,
    R,
    Start,
    Select,
    Count,
  };

  using ScriptRanges = std::array<std::vector<FrameRange>, static_cast<size_t>(Button::Count)>;

  static ScriptRanges ParseScript();
  bool ScriptPressed(Button button, uint64_t frameIndex) const noexcept;

  InputBindings bindings_;
  bool enabled_ = true;
  bool captureKeyboard_ = false, capturePointer_ = false;
  ScriptRanges scriptedRanges_;
  std::vector<SDL_Gamepad *> gamepads_{8};
  std::vector<std::string> gamepadKeys_{8};
  void RefreshGamepads(uint64_t frameIndex);
  SDL_Gamepad *ResolveGamepad(const std::string &device, int automatic);
  bool Pressed(const InputBinding &binding, SDL_Gamepad *pad, int threshold) const;
  bool Pressed(const InputAlternatives &bindings, SDL_Gamepad *pad, int threshold) const;
  bool MappedButton(const InputAlternatives &normal, const InputAlternatives &turbo,
                    SDL_Gamepad *pad, int threshold) const;
  float ReadAxis(const AxisAlternatives &bindings, SDL_Gamepad *pad, int threshold, uint64_t frame);
  void SampleMotion(uint64_t frame);
  uint64_t lastDeviceFrame_ = uint64_t(-1);
  unsigned turboPhase_ = 0;
  std::array<float, 2> motion_{};
  uint64_t lastMotionFrame_ = uint64_t(-1);
  float mouseFractionX_ = 0, mouseFractionY_ = 0;
  snes::core::MouseState mouseState_;
  uint64_t lastMouseFrame_ = uint64_t(-1);
  VideoViewport gunViewport_;
  bool gunFocused_ = false;
  std::array<std::array<float, 2>, 2> gunPosition_{{{128, 112}, {128, 112}}};
  std::array<uint64_t, 2> lastGunFrame_{uint64_t(-1), uint64_t(-1)};
};

} // namespace snes::frontend
