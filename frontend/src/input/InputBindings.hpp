#pragma once

#include "snes/core/Platform.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <string>
#include <string_view>

namespace snes::frontend {
struct KeyboardMap {
  SDL_Scancode up = SDL_SCANCODE_UP, down = SDL_SCANCODE_DOWN;
  SDL_Scancode left = SDL_SCANCODE_LEFT, right = SDL_SCANCODE_RIGHT;
  SDL_Scancode a = SDL_SCANCODE_X, b = SDL_SCANCODE_Z;
  SDL_Scancode x = SDL_SCANCODE_S, y = SDL_SCANCODE_A;
  SDL_Scancode l = SDL_SCANCODE_Q, r = SDL_SCANCODE_W;
  SDL_Scancode start = SDL_SCANCODE_RETURN, select = SDL_SCANCODE_RSHIFT;
};
struct ControllerButton {
  const char *key;
  const char *label;
  bool core::InputState::*state;
  SDL_Scancode KeyboardMap::*keyboard;
  bool turbo = true;
};
inline constexpr std::array controllerButtons{
    ControllerButton{"up", "Up", &core::InputState::up, &KeyboardMap::up, false},
    ControllerButton{"down", "Down", &core::InputState::down, &KeyboardMap::down, false},
    ControllerButton{"left", "Left", &core::InputState::left, &KeyboardMap::left, false},
    ControllerButton{"right", "Right", &core::InputState::right, &KeyboardMap::right, false},
    ControllerButton{"a", "A", &core::InputState::a, &KeyboardMap::a},
    ControllerButton{"b", "B", &core::InputState::b, &KeyboardMap::b},
    ControllerButton{"x", "X", &core::InputState::x, &KeyboardMap::x},
    ControllerButton{"y", "Y", &core::InputState::y, &KeyboardMap::y},
    ControllerButton{"l", "L", &core::InputState::l, &KeyboardMap::l},
    ControllerButton{"r", "R", &core::InputState::r, &KeyboardMap::r},
    ControllerButton{"start", "Start", &core::InputState::start, &KeyboardMap::start},
    ControllerButton{"select", "Select", &core::InputState::select, &KeyboardMap::select}};

struct InputBinding {
  enum class Kind { None, Keyboard, GamepadButton, AxisPositive, AxisNegative, MouseButton };
  Kind kind = Kind::None;
  int control = 0;
  bool operator==(const InputBinding &) const = default;
  void Validate() const;
  std::string Label() const;
};
using InputAlternatives = std::array<InputBinding, 4>;

struct AxisBinding {
  enum class Kind { None, MouseX, MouseY, GamepadAxis, Digital };
  Kind kind = Kind::None;
  int axis = 0, scale = 100;
  bool inverted = false;
  InputBinding digital;
  bool operator==(const AxisBinding &) const = default;
  void Validate() const;
};
using AxisAlternatives = std::array<AxisBinding, 4>;

inline constexpr std::array mouseButtonKeys{"left", "right"};
inline constexpr std::array gunButtonKeys{"trigger", "cursor", "turbo",
                                          "pause",   "start",  "offscreen"};
inline constexpr std::array gunButtonLabels{"Trigger", "Cursor", "Turbo switch",
                                            "Pause",   "Start",  "Offscreen"};
struct PeripheralBindings {
  std::string device = "auto";
  int axisThreshold = 16000;
  bool pointerAim = false;
  std::array<InputAlternatives, gunButtonKeys.size()> buttons{}, turbo{};
  std::array<AxisAlternatives, 2> axes{};
  bool operator==(const PeripheralBindings &) const = default;
  static PeripheralBindings Mouse();
  static PeripheralBindings Gun(size_t gun);
  void Validate(bool mouse) const;
};

struct PlayerBindings {
  // Automatic assignment retains occupied slots when another host pad disconnects.
  std::string device = "auto";
  int axisThreshold = 16000;
  std::array<InputAlternatives, controllerButtons.size()> buttons{}, turbo{};
  bool operator==(const PlayerBindings &) const = default;
  static PlayerBindings Defaults(size_t player);
};
struct InputBindings {
  std::array<PlayerBindings, 8> players;
  PeripheralBindings mouse = PeripheralBindings::Mouse();
  std::array<PeripheralBindings, 2> guns{PeripheralBindings::Gun(0), PeripheralBindings::Gun(1)};
  int turboFrequency = 4;
  InputBindings();
  bool operator==(const InputBindings &) const = default;
  void Validate() const;
  void SetKeyboard(const KeyboardMap &keyboard);
  KeyboardMap Keyboard() const;
};
} // namespace snes::frontend
