#include "InputBindings.hpp"

#include <stdexcept>

namespace snes::frontend {
void InputBinding::Validate() const {
  const bool valid =
      (kind == Kind::None && control == 0) ||
      (kind == Kind::Keyboard && control > SDL_SCANCODE_UNKNOWN && control < SDL_SCANCODE_COUNT) ||
      (kind == Kind::GamepadButton && control >= 0 && control < SDL_GAMEPAD_BUTTON_COUNT) ||
      ((kind == Kind::AxisPositive || kind == Kind::AxisNegative) && control >= 0 &&
       control < SDL_GAMEPAD_AXIS_COUNT) ||
      (kind == Kind::MouseButton && control >= 1 && control <= 5);
  if (!valid)
    throw std::invalid_argument("Input binding contains an invalid control");
}
std::string InputBinding::Label() const {
  switch (kind) {
  case Kind::Keyboard: {
    const auto *name = SDL_GetScancodeName(SDL_Scancode(control));
    return name && *name ? name : "Key " + std::to_string(control);
  }
  case Kind::GamepadButton: {
    constexpr std::array names{"South",       "East",          "West",           "North",
                               "Back",        "Guide",         "Start",          "Left stick",
                               "Right stick", "Left shoulder", "Right shoulder", "D-pad up",
                               "D-pad down",  "D-pad left",    "D-pad right"};
    if (control >= 0 && control < int(names.size()))
      return "Pad " + std::string(names[control]);
    if (const auto *name = SDL_GetGamepadStringForButton(SDL_GamepadButton(control)))
      return "Pad " + std::string(name);
    break;
  }
  case Kind::AxisPositive:
  case Kind::AxisNegative: {
    constexpr std::array names{"Left stick X",  "Left stick Y", "Right stick X",
                               "Right stick Y", "Left trigger", "Right trigger"};
    if (control >= 0 && control < int(names.size()))
      return "Pad " + std::string(names[control]) + (kind == Kind::AxisPositive ? " +" : " -");
    break;
  }
  case Kind::MouseButton: {
    constexpr std::array names{"Left", "Middle", "Right", "X1", "X2"};
    if (control >= 1 && control <= int(names.size()))
      return "Mouse " + std::string(names[control - 1]);
    break;
  }
  default:
    break;
  }
  return "Unassigned";
}
PlayerBindings PlayerBindings::Defaults(size_t player) {
  PlayerBindings result;
  constexpr std::array padButtons{
      SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,
      SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
      SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_SOUTH,
      SDL_GAMEPAD_BUTTON_NORTH,         SDL_GAMEPAD_BUTTON_WEST,
      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
      SDL_GAMEPAD_BUTTON_START,         SDL_GAMEPAD_BUTTON_BACK};
  for (size_t button = 0; button < controllerButtons.size(); ++button) {
    if (player == 0)
      result.buttons[button][0] = {InputBinding::Kind::Keyboard,
                                   KeyboardMap{}.*controllerButtons[button].keyboard};
    result.buttons[button][1] = {InputBinding::Kind::GamepadButton, padButtons[button]};
    if (button < 4)
      result.buttons[button][2] = {button % 2 == 0 ? InputBinding::Kind::AxisNegative
                                                   : InputBinding::Kind::AxisPositive,
                                   button < 2 ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_LEFTX};
  }
  return result;
}
InputBindings::InputBindings() {
  for (size_t player = 0; player < players.size(); ++player)
    players[player] = PlayerBindings::Defaults(player);
}
void InputBindings::Validate() const {
  for (const auto &player : players) {
    if (player.device.empty() || player.device.size() > 4096 ||
        player.device.find('\0') != std::string::npos || player.axisThreshold < 1 ||
        player.axisThreshold > 32767)
      throw std::invalid_argument(
          "Input preferences contain an invalid gamepad selection or threshold");
    for (const auto &button : player.buttons)
      for (const auto &binding : button)
        binding.Validate();
  }
}
void InputBindings::SetKeyboard(const KeyboardMap &keyboard) {
  for (size_t button = 0; button < controllerButtons.size(); ++button) {
    const auto key = keyboard.*controllerButtons[button].keyboard;
    players[0].buttons[button][0] = key == SDL_SCANCODE_UNKNOWN
                                        ? InputBinding{}
                                        : InputBinding{InputBinding::Kind::Keyboard, key};
  }
}
KeyboardMap InputBindings::Keyboard() const {
  KeyboardMap result;
  for (size_t button = 0; button < controllerButtons.size(); ++button) {
    const auto &binding = players[0].buttons[button][0];
    result.*controllerButtons[button].keyboard = binding.kind == InputBinding::Kind::Keyboard
                                                     ? SDL_Scancode(binding.control)
                                                     : SDL_SCANCODE_UNKNOWN;
  }
  return result;
}
} // namespace snes::frontend
