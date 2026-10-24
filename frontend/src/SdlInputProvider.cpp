// SDL3 keyboard to controller state translation.

#include "SdlInputProvider.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace snes::frontend {
namespace {
std::string GamepadKey(SDL_Gamepad* pad) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(SDL_GetGamepadID(pad)), guid, sizeof(guid));
    std::string key = guid;
    if (const auto* serial = SDL_GetGamepadSerial(pad); serial && *serial)
        return key + "/serial/" + serial;
    if (const auto* path = SDL_GetGamepadPath(pad); path && *path)
        return key + "/path/" + path;
    return key + "/index/";
}
} // namespace

std::vector<SdlInputProvider::Gamepad> SdlInputProvider::ConnectedGamepads() {
    static std::vector<Gamepad> known;
    std::vector<Gamepad> result;
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        auto* pad = SDL_OpenGamepad(ids[i]);
        if (!pad)
            continue;
        const auto base = GamepadKey(pad);
        const auto previous = std::find_if(known.begin(), known.end(), [&](const auto& device) {
            return device.id == ids[i] && device.key.starts_with(base);
        });
        auto key = previous != known.end() ? previous->key : base;
        if (previous == known.end()) {
            unsigned ordinal = 0;
            const auto occupied = [&](const auto& device) {
                return device.key == key && std::find(ids, ids + count, device.id) != ids + count;
            };
            if (base.ends_with("/index/"))
                key += "0";
            while (std::any_of(known.begin(), known.end(), occupied) ||
                   std::any_of(result.begin(), result.end(), occupied)) {
                ++ordinal;
                key = base + (base.ends_with("/index/") ? "" : "/index/") + std::to_string(ordinal);
            }
        }
        const auto* name = SDL_GetGamepadName(pad);
        result.push_back({ids[i], name ? name : "Gamepad", std::move(key)});
        SDL_CloseGamepad(pad);
    }
    SDL_free(ids);
    known = result;
    return result;
}

SdlInputProvider::SdlInputProvider() : SdlInputProvider(InputBindings{}) {
}

SdlInputProvider::SdlInputProvider(const KeyMap& keyMap) : SdlInputProvider(InputBindings{}) {
    bindings_.SetKeyboard(keyMap);
    bindings_.Validate();
}

SdlInputProvider::SdlInputProvider(const InputBindings& bindings)
    : bindings_(bindings), scriptedRanges_(ParseScript()) {
    bindings_.Validate();
    // Keep host state current; SetEnabled applies the frontend's focus policy.
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE);
}

void SdlInputProvider::SetBindings(const InputBindings& bindings) {
    bindings.Validate();
    auto next = bindings;
    if (next.turboFrequency != bindings_.turboFrequency)
        turboPhase_ = 0;
    std::swap(bindings_, next);
    ResetSampling();
}

SdlInputProvider::~SdlInputProvider() {
    for (auto* gamepad : gamepads_)
        if (gamepad)
            SDL_CloseGamepad(gamepad);
}

void SdlInputProvider::RefreshGamepads(uint64_t frameIndex) {
    if (lastDeviceFrame_ == frameIndex)
        return;
    lastDeviceFrame_ = frameIndex;
    bool changed = false;
    for (auto& gamepad : gamepads_) {
        if (gamepad && !SDL_GamepadConnected(gamepad)) {
            SDL_CloseGamepad(gamepad);
            gamepad = nullptr;
            changed = true;
        }
    }
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        const auto known = std::find_if(gamepads_.begin(), gamepads_.end(), [&](SDL_Gamepad* pad) {
            return pad && SDL_GetGamepadID(pad) == ids[i];
        });
        if (known != gamepads_.end())
            continue;
        auto empty = std::find(gamepads_.begin(), gamepads_.end(), nullptr);
        auto* opened = SDL_OpenGamepad(ids[i]);
        if (empty == gamepads_.end())
            gamepads_.push_back(opened);
        else
            *empty = opened;
        changed |= opened != nullptr;
    }
    SDL_free(ids);
    if (!changed)
        return;
    const auto connected = ConnectedGamepads();
    gamepadKeys_.resize(gamepads_.size());
    for (size_t slot = 0; slot < gamepads_.size(); ++slot) {
        gamepadKeys_[slot].clear();
        if (gamepads_[slot])
            for (const auto& device : connected)
                if (device.id == SDL_GetGamepadID(gamepads_[slot]))
                    gamepadKeys_[slot] = device.key;
    }
}

SDL_Gamepad* SdlInputProvider::ResolveGamepad(const std::string& device, int automatic) {
    if (device == "auto")
        return gamepads_[automatic];
    if (device != "none")
        for (size_t slot = 0; slot < gamepads_.size(); ++slot)
            if (gamepadKeys_[slot] == device)
                return gamepads_[slot];
    return nullptr;
}

bool SdlInputProvider::Pressed(const InputBinding& binding, SDL_Gamepad* pad, int threshold) const {
    using Kind = InputBinding::Kind;
    const auto control = binding.control;
    switch (binding.kind) {
    case Kind::Keyboard: {
        int count = 0;
        const auto* keys = SDL_GetKeyboardState(&count);
        return !captureKeyboard_ && keys && control < count && keys[control];
    }
    case Kind::MouseButton:
        return !capturePointer_ &&
               (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(control)) != 0;
    case Kind::GamepadButton:
        return pad && SDL_GetGamepadButton(pad, SDL_GamepadButton(control));
    case Kind::AxisPositive:
        return pad && SDL_GetGamepadAxis(pad, SDL_GamepadAxis(control)) > threshold;
    case Kind::AxisNegative:
        return pad && SDL_GetGamepadAxis(pad, SDL_GamepadAxis(control)) < -threshold;
    default:
        return false;
    }
}

bool SdlInputProvider::Pressed(const InputAlternatives& bindings, SDL_Gamepad* pad,
                               int threshold) const {
    return std::any_of(bindings.begin(), bindings.end(),
                       [&](const auto& binding) { return Pressed(binding, pad, threshold); });
}

bool SdlInputProvider::MappedButton(const InputAlternatives& normal, const InputAlternatives& turbo,
                                    SDL_Gamepad* pad, int threshold) const {
    if (Pressed(turbo, pad, threshold))
        return turboPhase_ >= unsigned(bindings_.turboFrequency);
    return Pressed(normal, pad, threshold);
}

void SdlInputProvider::SampleMotion(uint64_t frame) {
    if (lastMotionFrame_ == frame)
        return;
    lastMotionFrame_ = frame;
    SDL_GetRelativeMouseState(&motion_[0], &motion_[1]);
    for (auto& value : motion_)
        if (capturePointer_ || !std::isfinite(value))
            value = 0;
}

float SdlInputProvider::ReadAxis(const AxisAlternatives& bindings, SDL_Gamepad* pad, int threshold,
                                 uint64_t frame) {
    float result = 0;
    for (const auto& binding : bindings) {
        float value = 0;
        using Kind = AxisBinding::Kind;
        switch (binding.kind) {
        case Kind::MouseX:
        case Kind::MouseY:
            SampleMotion(frame);
            value = motion_[binding.kind == Kind::MouseX ? 0 : 1];
            break;
        case Kind::GamepadAxis:
            value = pad ? (SDL_GetGamepadAxis(pad, SDL_GamepadAxis(binding.axis)) >> 8) : 0;
            break;
        case Kind::Digital:
            value = Pressed(binding.digital, pad, threshold) ? 1 : 0;
            break;
        default:
            break;
        }
        result += value * (float(binding.scale) / 100) * (binding.inverted ? -1 : 1);
    }
    return std::clamp(result, -1000000.0f, 1000000.0f);
}

snes::core::InputState SdlInputProvider::PollController(int player, uint64_t frameIndex) {
    if (!enabled_ || player < 0 || player >= static_cast<int>(bindings_.players.size()))
        return {};
    RefreshGamepads(frameIndex);
    const auto& mapping = bindings_.players[player];
    auto* pad = ResolveGamepad(mapping.device, player);
    snes::core::InputState state;
    for (size_t button = 0; button < controllerButtons.size(); ++button)
        state.*controllerButtons[button].state =
            MappedButton(mapping.buttons[button], mapping.turbo[button], pad,
                         mapping.axisThreshold) ||
            (player == 0 && !captureKeyboard_ && ScriptPressed(Button(button), frameIndex));
    return state;
}

snes::core::MouseState SdlInputProvider::PollMouse(int port, uint64_t frameIndex) {
    if (!enabled_ || port < 0 || port > 1) {
        SDL_GetRelativeMouseState(nullptr, nullptr);
        return {};
    }
    if (lastMouseFrame_ == frameIndex)
        return mouseState_;
    lastMouseFrame_ = frameIndex;
    RefreshGamepads(frameIndex);
    const auto& mapping = bindings_.mouse;
    auto* pad = ResolveGamepad(mapping.device, 0);
    mouseFractionX_ += ReadAxis(mapping.axes[0], pad, mapping.axisThreshold, frameIndex);
    mouseFractionY_ += ReadAxis(mapping.axes[1], pad, mapping.axisThreshold, frameIndex);
    // Bound conversions even if a backend reports an extreme motion event.
    const auto dx =
        static_cast<int32_t>(std::clamp(std::trunc(mouseFractionX_), -1000000.0f, 1000000.0f));
    const auto dy =
        static_cast<int32_t>(std::clamp(std::trunc(mouseFractionY_), -1000000.0f, 1000000.0f));
    mouseFractionX_ -= dx;
    mouseFractionY_ -= dy;
    mouseState_ = {dx, dy,
                   MappedButton(mapping.buttons[0], mapping.turbo[0], pad, mapping.axisThreshold),
                   MappedButton(mapping.buttons[1], mapping.turbo[1], pad, mapping.axisThreshold)};
    return mouseState_;
}

snes::core::InputState SdlInputProvider::Poll(uint64_t frameIndex) {
    return PollController(0, frameIndex);
}

snes::core::LightGunState SdlInputProvider::PollLightGun(int port, int gun, uint64_t frameIndex) {
    if (!enabled_)
        return {};
    if (port < 0 || port > 1 || gun < 0 || gun > 1)
        return {};
    RefreshGamepads(frameIndex);
    const auto& mapping = bindings_.guns[gun];
    auto* pad = ResolveGamepad(mapping.device, gun);
    snes::core::LightGunState state;
    if (mapping.pointerAim) {
        float x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        state =
            capturePointer_ || !gunFocused_ ? snes::core::LightGunState{} : gunViewport_.Aim(x, y);
    } else {
        if (lastGunFrame_[gun] != frameIndex) {
            lastGunFrame_[gun] = frameIndex;
            for (size_t axis = 0; axis < 2; ++axis)
                gunPosition_[gun][axis] = std::clamp(
                    gunPosition_[gun][axis] +
                        ReadAxis(mapping.axes[axis], pad, mapping.axisThreshold, frameIndex),
                    0.0f, axis == 0 ? 255.0f : float(std::max(1u, gunViewport_.visibleLines) - 1));
        }
        state.x = int(gunPosition_[gun][0]);
        state.y = int(gunPosition_[gun][1]);
        state.offscreen = gunViewport_.width <= 0 || gunViewport_.height <= 0;
    }
    constexpr std::array members{
        &snes::core::LightGunState::trigger, &snes::core::LightGunState::cursor,
        &snes::core::LightGunState::turbo, &snes::core::LightGunState::pause,
        &snes::core::LightGunState::start};
    for (size_t button = 0; button < members.size(); ++button)
        state.*members[button] = MappedButton(mapping.buttons[button], mapping.turbo[button], pad,
                                              mapping.axisThreshold);
    state.offscreen |= Pressed(mapping.buttons[5], pad, mapping.axisThreshold);
    return state;
}

SdlInputProvider::ScriptRanges SdlInputProvider::ParseScript() {
    ScriptRanges ranges;

    const char* raw = std::getenv("SNES_INPUT_SCRIPT");
    if (!raw || !*raw) {
        return ranges;
    }

    auto parseButton = [](const std::string& name) -> Button {
        if (name == "up")
            return Button::Up;
        if (name == "down")
            return Button::Down;
        if (name == "left")
            return Button::Left;
        if (name == "right")
            return Button::Right;
        if (name == "a")
            return Button::A;
        if (name == "b")
            return Button::B;
        if (name == "x")
            return Button::X;
        if (name == "y")
            return Button::Y;
        if (name == "l")
            return Button::L;
        if (name == "r")
            return Button::R;
        if (name == "start")
            return Button::Start;
        if (name == "select")
            return Button::Select;
        return Button::Count;
    };

    std::string script(raw);
    size_t cursor = 0;
    while (cursor < script.size()) {
        size_t next = script.find(',', cursor);
        std::string token =
            script.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        cursor = (next == std::string::npos) ? script.size() : next + 1;
        if (token.empty())
            continue;

        size_t at = token.find('@');
        size_t dash = token.find('-', at == std::string::npos ? 0 : at + 1);
        if (at == std::string::npos || dash == std::string::npos)
            continue;

        Button button = parseButton(token.substr(0, at));
        if (button == Button::Count)
            continue;

        try {
            uint64_t first = std::stoull(token.substr(at + 1, dash - at - 1));
            uint64_t last = std::stoull(token.substr(dash + 1));
            if (last < first)
                continue;
            ranges[static_cast<size_t>(button)].push_back({first, last});
        } catch (...) {
            continue;
        }
    }

    return ranges;
}

bool SdlInputProvider::ScriptPressed(Button button, uint64_t frameIndex) const noexcept {
    const auto& ranges = scriptedRanges_[static_cast<size_t>(button)];
    for (const auto& range : ranges) {
        if (frameIndex >= range.first && frameIndex <= range.last) {
            return true;
        }
    }
    return false;
}

} // namespace snes::frontend
