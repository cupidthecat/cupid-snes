#include "Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace snes::frontend {
namespace {
constexpr size_t recentLimit = 10, maximumFileSize = 1024 * 1024;

int Number(std::string_view text, int minimum, int maximum) {
  int value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum ||
      value > maximum)
    throw std::runtime_error("Invalid preference value: " + std::string(text));
  return value;
}
std::map<std::string, std::string> Fields(const Preferences &p) {
  std::map<std::string, std::string> result{
      {"video.scale", std::to_string(p.scale)},
      {"video.aspect", std::to_string(p.aspectCorrection)},
      {"video.integer", std::to_string(p.integerScaling)},
      {"video.linear", std::to_string(p.linearFilter)},
      {"audio.volume", std::to_string(p.volume)},
      {"audio.mute", std::to_string(p.mute)},
      {"audio.mute-unfocused", std::to_string(p.muteUnfocused)},
      {"audio.device", p.audioDevice},
      {"input.focus", std::to_string(int(p.focus))},
      {"input.turbo-frequency", std::to_string(p.input.turboFrequency)},
      {"input.port1", std::to_string(int(p.console.ports[0]))},
      {"input.port2", std::to_string(int(p.console.ports[1]))},
      {"input.expansion", std::to_string(int(p.console.expansion))},
      {"path.firmware", p.firmwareDirectory},
      {"path.screenshots", p.screenshotDirectory},
      {"path.saves", p.saveDirectory},
      {"recent.count", std::to_string(p.recent.size())}};
  const auto keyboard = p.input.Keyboard();
  for (const auto &button : controllerButtons)
    result["key." + std::string(button.key)] = std::to_string(keyboard.*button.keyboard);
  for (size_t player = 0; player < p.input.players.size(); ++player) {
    const auto prefix = "player." + std::to_string(player + 1) + ".";
    const auto &mapping = p.input.players[player];
    result[prefix + "device"] = mapping.device;
    result[prefix + "threshold"] = std::to_string(mapping.axisThreshold);
    for (size_t button = 0; button < controllerButtons.size(); ++button) {
      for (size_t slot = 0; slot < mapping.buttons[button].size(); ++slot) {
        const auto binding = prefix + controllerButtons[button].key + "." + std::to_string(slot);
        result[binding + ".type"] = std::to_string(int(mapping.buttons[button][slot].kind));
        result[binding + ".control"] = std::to_string(mapping.buttons[button][slot].control);
        const auto &turbo = mapping.turbo[button][slot];
        const auto turboKey =
            prefix + "turbo." + controllerButtons[button].key + "." + std::to_string(slot);
        result[turboKey + ".type"] = std::to_string(int(turbo.kind));
        result[turboKey + ".control"] = std::to_string(turbo.control);
      }
    }
  }
  const std::array peripherals{&p.input.mouse, &p.input.guns[0], &p.input.guns[1]};
  constexpr std::array peripheralNames{"mouse", "gun1", "gun2"};
  for (size_t index = 0; index < peripherals.size(); ++index) {
    const auto prefix = "peripheral." + std::string(peripheralNames[index]) + ".";
    const auto &mapping = *peripherals[index];
    result[prefix + "device"] = mapping.device;
    result[prefix + "threshold"] = std::to_string(mapping.axisThreshold);
    result[prefix + "pointer"] = std::to_string(mapping.pointerAim);
    const auto count = index == 0 ? mouseButtonKeys.size() : gunButtonKeys.size();
    for (size_t button = 0; button < count; ++button) {
      const std::string key = index == 0 ? mouseButtonKeys[button] : gunButtonKeys[button];
      for (size_t slot = 0; slot < InputAlternatives{}.size(); ++slot) {
        for (int turbo = 0; turbo < 2; ++turbo) {
          const auto binding = prefix + (turbo ? "turbo." : "") + key + "." + std::to_string(slot);
          const auto &value = (turbo ? mapping.turbo : mapping.buttons)[button][slot];
          result[binding + ".type"] = std::to_string(int(value.kind));
          result[binding + ".control"] = std::to_string(value.control);
        }
      }
    }
    for (size_t axis = 0; axis < 2; ++axis) {
      for (size_t slot = 0; slot < AxisAlternatives{}.size(); ++slot) {
        const auto binding = prefix + (axis == 0 ? "x." : "y.") + std::to_string(slot) + ".";
        const auto &value = mapping.axes[axis][slot];
        result[binding + "type"] = std::to_string(int(value.kind));
        result[binding + "axis"] = std::to_string(value.axis);
        result[binding + "scale"] = std::to_string(value.scale);
        result[binding + "invert"] = std::to_string(value.inverted);
        result[binding + "digital.type"] = std::to_string(int(value.digital.kind));
        result[binding + "digital.control"] = std::to_string(value.digital.control);
      }
    }
  }
  for (size_t command = 0; command < desktopCommands.size(); ++command) {
    for (size_t slot = 0; slot < Hotkeys::bindingCount; ++slot) {
      const auto prefix =
          "hotkey." + std::string(desktopCommands[command].key) + "." + std::to_string(slot) + ".";
      const auto &chord = p.hotkeys.bindings[command][slot];
      result[prefix + "key"] = std::to_string(chord.scancode);
      result[prefix + "modifiers"] = std::to_string(chord.modifiers);
      const auto &pad = p.hotkeys.gamepads[command][slot];
      result[prefix + "pad.device"] = pad.device;
      result[prefix + "pad.threshold"] = std::to_string(pad.axisThreshold);
      for (size_t control = 0; control < pad.controls.size(); ++control) {
        const auto binding = prefix + "pad." + std::to_string(control) + ".";
        result[binding + "type"] = std::to_string(int(pad.controls[control].kind));
        result[binding + "control"] = std::to_string(pad.controls[control].control);
      }
    }
  }
  for (unsigned index = 0; index < p.recent.size(); ++index) {
    const auto prefix = "recent." + std::to_string(index) + ".";
    result[prefix + "kind"] = std::to_string(int(p.recent[index].kind));
    result[prefix + "dip"] = std::to_string(p.recent[index].competitionDip);
    for (unsigned asset = 0; asset < 4; ++asset)
      result[prefix + "path" + std::to_string(asset)] = p.recent[index].paths[asset];
  }
  return result;
}
} // namespace

void Preferences::Validate() const {
  console.Validate();
  hotkeys.Validate();
  input.Validate();
  if (scale < 1 || scale > 6 || volume < 0 || volume > 200 || int(focus) < 0 || int(focus) > 2 ||
      recent.size() > recentLimit)
    throw std::invalid_argument("Preferences contain an unsupported setting");
  for (const auto &request : recent) {
    if (int(request.kind) < 0 || request.kind > LaunchRequest::Kind::PowerFest94 ||
        request.paths[0].empty())
      throw std::invalid_argument("Recent game contains an invalid launch request");
    const unsigned required = request.kind == LaunchRequest::Kind::Handheld ? 3
                              : request.kind == LaunchRequest::Kind::Campus92 ||
                                      request.kind == LaunchRequest::Kind::PowerFest94
                                  ? 4
                                  : 1;
    for (unsigned index = 0; index < required; ++index)
      if (request.paths[index].empty() || request.paths[index] == "-")
        throw std::invalid_argument("Recent game is missing a required asset");
  }
}
void Preferences::Remember(LaunchRequest request) {
  for (auto &name : request.paths) {
    if (name.empty() || name == "-")
      continue;
    name = core::PathText(std::filesystem::absolute(core::FilePath(name)).lexically_normal());
  }
  std::erase(recent, request);
  recent.insert(recent.begin(), std::move(request));
  if (recent.size() > recentLimit)
    recent.resize(recentLimit);
}
PreferencesFile::PreferencesFile(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path PreferencesFile::ResolvePath(std::string_view text) const {
  if (text.empty())
    return {};
  auto path = core::FilePath(text);
  if (!path.is_absolute())
    path = std::filesystem::absolute(path_).parent_path() / path;
  return path.lexically_normal();
}

std::filesystem::path PreferencesFile::DefaultPath() {
  char *raw = SDL_GetPrefPath("Cupid", "Cupid-SNES");
  if (!raw)
    throw std::runtime_error(SDL_GetError());
  const auto path = core::FilePath(raw) / "preferences.cfg";
  SDL_free(raw);
  return path;
}

Preferences PreferencesFile::Load() {
  writable_ = false;
  unknown_.clear();
  loadedRecent_.clear();
  if (!std::filesystem::exists(path_)) {
    writable_ = true;
    return {};
  }
  const auto bytes = ReadFile(path_, maximumFileSize);
  if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end())
    throw std::runtime_error("Preferences contain a null character: " + core::PathText(path_));
  std::string contents(bytes.begin(), bytes.end());
  if (contents.starts_with("\xef\xbb\xbf"))
    contents.erase(0, 3);
  std::istringstream stream(std::move(contents));
  std::string header;
  if (!std::getline(stream, header) ||
      (header != "Cupid-SNES preferences 1" && header != "Cupid-SNES preferences 1\r"))
    throw std::runtime_error("Unsupported preferences file: " + core::PathText(path_));
  std::map<std::string, std::string> fields;
  std::string name, value;
  while (stream >> name) {
    if (!(stream >> std::quoted(value)) || !fields.emplace(name, value).second)
      throw std::runtime_error("Malformed or duplicate preference: " + name);
  }
  if (!stream.eof())
    throw std::runtime_error("Cannot parse preferences: " + core::PathText(path_));
  Preferences p;
  const auto text = [&](const std::string &key, std::string fallback) {
    const auto found = fields.find(key);
    if (found == fields.end())
      return fallback;
    auto result = std::move(found->second);
    fields.erase(found);
    return result;
  };
  const auto number = [&](const std::string &key, int fallback, int minimum, int maximum) {
    return Number(text(key, std::to_string(fallback)), minimum, maximum);
  };
  p.scale = number("video.scale", p.scale, 1, 6);
  p.aspectCorrection = number("video.aspect", p.aspectCorrection, 0, 1);
  p.integerScaling = number("video.integer", p.integerScaling, 0, 1);
  p.linearFilter = number("video.linear", p.linearFilter, 0, 1);
  p.volume = number("audio.volume", p.volume, 0, 200);
  p.mute = number("audio.mute", p.mute, 0, 1);
  p.muteUnfocused = number("audio.mute-unfocused", p.muteUnfocused, 0, 1);
  p.audioDevice = text("audio.device", "");
  p.firmwareDirectory = text("path.firmware", "");
  p.screenshotDirectory = text("path.screenshots", "");
  p.saveDirectory = text("path.saves", "");
  p.focus = FocusBehavior(number("input.focus", int(p.focus), 0, 2));
  p.input.turboFrequency = number("input.turbo-frequency", p.input.turboFrequency, 1, 8);
  for (size_t port = 0; port < p.console.ports.size(); ++port)
    p.console.ports[port] = core::ControllerDevice(number("input.port" + std::to_string(port + 1),
                                                          int(p.console.ports[port]), 0,
                                                          int(core::ControllerDevice::MacsRifle)));
  p.console.expansion =
      core::Emulator::ExpansionDevice(number("input.expansion", int(p.console.expansion), 0,
                                             int(core::Emulator::ExpansionDevice::Satellaview)));
  for (size_t command = 0; command < desktopCommands.size(); ++command) {
    for (size_t slot = 0; slot < Hotkeys::bindingCount; ++slot) {
      const auto prefix =
          "hotkey." + std::string(desktopCommands[command].key) + "." + std::to_string(slot) + ".";
      auto &chord = p.hotkeys.bindings[command][slot];
      chord.scancode =
          SDL_Scancode(number(prefix + "key", chord.scancode, 0, SDL_SCANCODE_COUNT - 1));
      chord.modifiers = unsigned(number(prefix + "modifiers", chord.modifiers, 0, 15));
      auto &pad = p.hotkeys.gamepads[command][slot];
      pad.device = text(prefix + "pad.device", pad.device);
      pad.axisThreshold = number(prefix + "pad.threshold", pad.axisThreshold, 1, 32767);
      for (size_t control = 0; control < pad.controls.size(); ++control) {
        const auto binding = prefix + "pad." + std::to_string(control) + ".";
        auto &value = pad.controls[control];
        value.kind = InputBinding::Kind(number(binding + "type", int(value.kind), 0, 5));
        value.control = number(binding + "control", value.control, 0, SDL_SCANCODE_COUNT - 1);
      }
    }
  }
  KeyboardMap keyboard;
  for (const auto &button : controllerButtons)
    keyboard.*button.keyboard = SDL_Scancode(number(
        "key." + std::string(button.key), keyboard.*button.keyboard, 0, SDL_SCANCODE_COUNT - 1));
  p.input.SetKeyboard(keyboard);
  for (size_t player = 0; player < p.input.players.size(); ++player) {
    const auto prefix = "player." + std::to_string(player + 1) + ".";
    auto &mapping = p.input.players[player];
    mapping.device = text(prefix + "device", mapping.device);
    mapping.axisThreshold = number(prefix + "threshold", mapping.axisThreshold, 1, 32767);
    for (size_t button = 0; button < controllerButtons.size(); ++button) {
      for (size_t slot = 0; slot < mapping.buttons[button].size(); ++slot) {
        const auto binding = prefix + controllerButtons[button].key + "." + std::to_string(slot);
        auto &target = mapping.buttons[button][slot];
        target.kind = InputBinding::Kind(number(binding + ".type", int(target.kind), 0, 5));
        target.control = number(binding + ".control", target.control, 0, SDL_SCANCODE_COUNT - 1);
        const auto turboKey =
            prefix + "turbo." + controllerButtons[button].key + "." + std::to_string(slot);
        auto &turbo = mapping.turbo[button][slot];
        turbo.kind = InputBinding::Kind(number(turboKey + ".type", int(turbo.kind), 0, 5));
        turbo.control = number(turboKey + ".control", turbo.control, 0, SDL_SCANCODE_COUNT - 1);
      }
    }
  }
  const std::array peripherals{&p.input.mouse, &p.input.guns[0], &p.input.guns[1]};
  constexpr std::array peripheralNames{"mouse", "gun1", "gun2"};
  for (size_t index = 0; index < peripherals.size(); ++index) {
    const auto prefix = "peripheral." + std::string(peripheralNames[index]) + ".";
    auto &mapping = *peripherals[index];
    mapping.device = text(prefix + "device", mapping.device);
    mapping.axisThreshold = number(prefix + "threshold", mapping.axisThreshold, 1, 32767);
    mapping.pointerAim = number(prefix + "pointer", mapping.pointerAim, 0, 1);
    const auto count = index == 0 ? mouseButtonKeys.size() : gunButtonKeys.size();
    for (size_t button = 0; button < count; ++button) {
      const std::string key = index == 0 ? mouseButtonKeys[button] : gunButtonKeys[button];
      for (size_t slot = 0; slot < InputAlternatives{}.size(); ++slot) {
        for (int turbo = 0; turbo < 2; ++turbo) {
          const auto binding = prefix + (turbo ? "turbo." : "") + key + "." + std::to_string(slot);
          auto &value = (turbo ? mapping.turbo : mapping.buttons)[button][slot];
          value.kind = InputBinding::Kind(number(binding + ".type", int(value.kind), 0, 5));
          value.control = number(binding + ".control", value.control, 0, SDL_SCANCODE_COUNT - 1);
        }
      }
    }
    for (size_t axis = 0; axis < 2; ++axis) {
      for (size_t slot = 0; slot < AxisAlternatives{}.size(); ++slot) {
        const auto binding = prefix + (axis == 0 ? "x." : "y.") + std::to_string(slot) + ".";
        auto &value = mapping.axes[axis][slot];
        value.kind = AxisBinding::Kind(number(binding + "type", int(value.kind), 0, 4));
        value.axis = number(binding + "axis", value.axis, 0, SDL_GAMEPAD_AXIS_COUNT - 1);
        value.scale = number(binding + "scale", value.scale, 1, 400);
        value.inverted = number(binding + "invert", value.inverted, 0, 1);
        value.digital.kind =
            InputBinding::Kind(number(binding + "digital.type", int(value.digital.kind), 0, 5));
        value.digital.control =
            number(binding + "digital.control", value.digital.control, 0, SDL_SCANCODE_COUNT - 1);
      }
    }
  }
  const auto count = number("recent.count", 0, 0, int(recentLimit));
  for (int index = 0; index < count; ++index) {
    const auto prefix = "recent." + std::to_string(index) + ".";
    LaunchRequest request;
    request.kind =
        LaunchRequest::Kind(number(prefix + "kind", -1, 0, int(LaunchRequest::Kind::PowerFest94)));
    request.competitionDip = uint8_t(number(prefix + "dip", 0, 0, 255));
    for (unsigned asset = 0; asset < 4; ++asset)
      request.paths[asset] = text(prefix + "path" + std::to_string(asset), "");
    p.recent.push_back(std::move(request));
  }
  p.Validate();
  loadedRecent_ = p.recent;
  unknown_ = std::move(fields);
  writable_ = true;
  return p;
}

void PreferencesFile::Save(const Preferences &p) {
  if (!writable_)
    throw std::runtime_error("Read valid preferences before replacing: " + core::PathText(path_));
  p.Validate();
  auto extra = unknown_;
  if (p.recent != loadedRecent_)
    std::erase_if(extra, [](const auto &entry) { return entry.first.starts_with("recent."); });
  auto fields = extra;
  for (auto &[name, value] : Fields(p))
    fields[name] = std::move(value);
  std::ostringstream stream;
  stream << "Cupid-SNES preferences 1\n";
  for (const auto &[name, value] : fields)
    if (value.find('\0') != std::string::npos)
      throw std::invalid_argument("Preference contains a null character: " + name);
  for (const auto &[name, value] : fields)
    stream << name << ' ' << std::quoted(value) << '\n';
  const auto text = stream.str();
  if (text.size() > maximumFileSize)
    throw std::runtime_error("Preferences exceed the supported file size");
  if (!path_.parent_path().empty())
    std::filesystem::create_directories(path_.parent_path());
  auto recent = p.recent;
  WriteFileAtomically(path_, {reinterpret_cast<const uint8_t *>(text.data()), text.size()});
  unknown_ = std::move(extra);
  loadedRecent_ = std::move(recent);
}
} // namespace snes::frontend
