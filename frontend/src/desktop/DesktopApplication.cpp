#include "DesktopApplication.hpp"
#include "DesktopMenu.hpp"
#include "DesktopUi.hpp"
#include "FileDialogs.hpp"
#include "FramePacer.hpp"
#include "SdlAudioOutput.hpp"
#include "SdlInputProvider.hpp"
#include "SdlVideoOutput.hpp"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace snes::frontend {
namespace {
bool WindowFocused(SDL_Window *window) {
  const auto flags = SDL_GetWindowFlags(window);
  return (flags & SDL_WINDOW_INPUT_FOCUS) != 0 &&
         (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
}
std::filesystem::path ScreenshotPath(const Preferences &preferences, const PreferencesFile &file) {
  if (!preferences.screenshotDirectory.empty()) {
    auto directory = file.ResolvePath(preferences.screenshotDirectory);
    std::filesystem::create_directories(directory);
    return directory / ("screenshot-" + std::to_string(SDL_GetTicksNS()) + ".bmp");
  }
  char *raw = SDL_GetPrefPath("Cupid", "Cupid-SNES");
  if (!raw)
    throw std::runtime_error(SDL_GetError());
  auto directory = core::FilePath(raw) / "screenshots";
  SDL_free(raw);
  std::filesystem::create_directories(directory);
  return directory / ("screenshot-" + std::to_string(SDL_GetTicksNS()) + ".bmp");
}
} // namespace

int RunDesktop(core::Emulator::Config config, const DeviceOverrides &overrides,
               const std::optional<LaunchRequest> &initial,
               const std::filesystem::path &preferencesPath) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
    return EXIT_FAILURE;
  }
  struct SdlLifetime {
    ~SdlLifetime() {
      SDL_Quit();
    }
  } lifetime;
  try {
    PreferencesFile preferencesFile(preferencesPath.empty() ? PreferencesFile::DefaultPath()
                                                            : preferencesPath);
    auto preferences = preferencesFile.Load();
    const auto console = overrides.Resolve(preferences.console);
    auto devices = console.ports;
    config.expansionDevice = console.expansion;
    if (!config.cartridgeFirmware && !preferences.firmwareDirectory.empty()) {
      auto firmware = std::make_shared<core::CartridgeFirmware>();
      std::string error;
      if (!firmware->LoadDirectory(preferencesFile.ResolvePath(preferences.firmwareDirectory),
                                   &error))
        throw std::runtime_error(error);
      config.cartridgeFirmware = std::move(firmware);
    }
    SdlVideoOutput::Config videoConfig;
    videoConfig.scale = preferences.scale;
    videoConfig.aspectCorrection = preferences.aspectCorrection;
    videoConfig.integerScaling = preferences.integerScaling;
    videoConfig.linearFilter = preferences.linearFilter;
    videoConfig.title = "Cupid-SNES";
    videoConfig.vsync = false;
    videoConfig.deferPresentation = true;
    SdlVideoOutput video(videoConfig);
    SdlAudioOutput::Config audioConfig;
    audioConfig.deviceName = preferences.audioDevice;
    SdlAudioOutput audio(audioConfig);
    audio.SetVolume(preferences.volume, preferences.mute);
    SdlInputProvider input(preferences.input);
    Session session(std::move(config), {&video, &audio, &input}, devices);
    struct SaveOnExit {
      Session &session;
      ~SaveOnExit() {
        try {
          session.Flush();
        } catch (const std::exception &error) {
          std::cerr << "Cannot save on exit: " << error.what() << '\n';
        }
      }
    } saveOnExit{session};
    DesktopUi ui(video.Window(), video.Renderer());
    ui.Frame();
    video.SetOverlay([&] { ui.Render(); }, ui.TopInset(), ui.BottomInset());
    auto &menu = ui.Menu();
    menu.SetRecent(preferences.recent);
    menu.SetHotkeys(preferences.hotkeys);
    FileDialogs dialogs(video.Window());
    FramePacer pacer;
    bool running = true, paused = false, dialogWasPaused = false, picturePending = false;
    bool quitPending = false;
    bool focusPaused = false;
    bool audioMuted = preferences.mute;
    bool preferencesActive = false, preferencesWasPaused = false;
    bool errorActive = false, errorWasPaused = false;
    unsigned steps = 0;
    uint64_t presentationTime = 0;
    uint64_t nextUiFrame = 0;

    const auto refresh = [&] {
      std::string title = "Cupid-SNES";
      if (const auto *request = session.Loaded()) {
        const auto index = request->kind == LaunchRequest::Kind::Handheld ? 1 : 0;
        title += " - " + core::PathText(core::FilePath(request->paths[index]).filename());
      }
      if ((paused || focusPaused) && session.Machine())
        title += " (Paused)";
      SDL_SetWindowTitle(video.Window(), title.c_str());
      menu.Update(session.Machine() != nullptr, paused);
      ui.SetStatus(session.Machine() ? title : "Ready - Open a game from File");
    };
    const auto resetPresentation = [&] {
      audio.Clear();
      video.PresentPending();
      picturePending = false;
      pacer.Reset();
    };
    const auto setPaused = [&](bool value) {
      if (paused == value)
        return;
      paused = value;
      resetPresentation();
      if (paused)
        SDL_SetWindowRelativeMouseMode(video.Window(), false);
      if (!paused && !focusPaused && session.Machine())
        audio.Resume();
      refresh();
    };
    const auto report = [&](const std::exception &error) {
      std::cerr << error.what() << '\n';
      if (!ui.ErrorOpen())
        errorWasPaused = paused;
      errorActive = true;
      setPaused(true);
      ui.ShowError(error.what());
    };
    const auto open = [&](const LaunchRequest &request) {
      session.Open(request);
      input.ResetSampling();
      resetPresentation();
      video.ClearPicture();
      paused = false;
      steps = 0;
      const bool mouse = devices[0] == core::ControllerDevice::Mouse ||
                         devices[1] == core::ControllerDevice::Mouse;
      if (!SDL_SetWindowRelativeMouseMode(video.Window(), mouse))
        throw std::runtime_error(std::string("Cannot capture mouse: ") + SDL_GetError());
      if (!focusPaused)
        audio.Resume();
      refresh();
      const auto &header = session.Machine()->LoadedCartridge()->Header();
      std::cout << "Loaded: " << header.title << " (" << core::MappingName(header.mapping) << ")\n";
      preferences.Remember(request);
      menu.SetRecent(preferences.recent);
      try {
        preferencesFile.Save(preferences);
      } catch (const std::exception &error) {
        report(error);
      }
    };
    if (initial)
      open(*initial);
    else
      video.ShowIdle();
    refresh();

    const auto presentation = [](const Preferences &p) {
      return SdlVideoOutput::Presentation{p.scale, p.aspectCorrection, p.integerScaling,
                                          p.linearFilter};
    };
    const auto applyPreferences = [&](const Preferences &candidate) {
      candidate.Validate();
      auto next = candidate;
      std::shared_ptr<const core::CartridgeFirmware> firmware;
      const bool changedFirmware = next.firmwareDirectory != preferences.firmwareDirectory;
      if (changedFirmware && !next.firmwareDirectory.empty()) {
        auto loaded = std::make_shared<core::CartridgeFirmware>();
        std::string error;
        if (!loaded->LoadDirectory(preferencesFile.ResolvePath(next.firmwareDirectory), &error))
          throw std::runtime_error(error);
        firmware = std::move(loaded);
      }
      const auto shots = preferencesFile.ResolvePath(next.screenshotDirectory);
      if (!shots.empty() && std::filesystem::exists(shots) && !std::filesystem::is_directory(shots))
        throw std::runtime_error("Screenshot path must be a directory");
      const bool deviceChanged = next.audioDevice != preferences.audioDevice;
      const bool previousMuted = audioMuted;
      const bool nextMuted = next.mute || (next.muteUnfocused && !WindowFocused(video.Window()));
      try {
        if (deviceChanged)
          audio.SelectDevice(next.audioDevice);
        video.SetPresentation(presentation(next));
        audio.SetVolume(next.volume, nextMuted);
        input.SetBindings(next.input);
        preferencesFile.Save(next);
      } catch (...) {
        const auto original = std::current_exception();
        try {
          if (deviceChanged)
            audio.SelectDevice(preferences.audioDevice);
          audio.SetVolume(preferences.volume, previousMuted);
        } catch (const std::exception &error) {
          std::cerr << "Cannot restore audio settings: " << error.what() << '\n';
        }
        try {
          video.SetPresentation(presentation(preferences));
        } catch (const std::exception &error) {
          std::cerr << "Cannot restore video settings: " << error.what() << '\n';
        }
        input.SetBindings(preferences.input);
        std::rethrow_exception(original);
      }
      if (changedFirmware)
        session.SetFirmware(std::move(firmware));
      const bool changedPorts = devices != next.console.ports;
      session.SetDevices(next.console);
      devices = next.console.ports;
      if (changedPorts)
        video.SetGunAim({}, 0);
      menu.SetHotkeys(next.hotkeys);
      preferences = std::move(next);
      audioMuted = nextMuted;
      input.ResetSampling();
      refresh();
    };

    const auto execute = [&](DesktopCommand command) {
      try {
        if (!running || ((dialogs.Active() || ui.PreferencesEditor().Active() || ui.ErrorOpen()) &&
                         command != DesktopCommand::Quit))
          return;
        using Command = DesktopCommand;
        if (command >= Command::Recent0 && command <= Command::Recent9) {
          const auto index = unsigned(command) - unsigned(Command::Recent0);
          if (index < preferences.recent.size()) {
            const auto request = preferences.recent[index];
            open(request);
          }
        } else if (command == Command::ClearRecent) {
          preferences.recent.clear();
          menu.SetRecent(preferences.recent);
          preferencesFile.Save(preferences);
        } else if (command == Command::Preferences) {
          preferencesWasPaused = paused;
          auto current = preferences;
          current.console = session.Devices();
          ui.PreferencesEditor().Open(current, preferencesFile.Path());
          preferencesActive = true;
          steps = 0;
          setPaused(true);
        } else if (command <= Command::OpenPowerFest) {
          dialogWasPaused = paused;
          setPaused(true);
          constexpr LaunchRequest::Kind kinds[] = {
              LaunchRequest::Kind::Cartridge,   LaunchRequest::Kind::Handheld,
              LaunchRequest::Kind::SufamiTurbo, LaunchRequest::Kind::Broadcast,
              LaunchRequest::Kind::Campus92,    LaunchRequest::Kind::PowerFest94};
          try {
            dialogs.Begin(kinds[unsigned(command)]);
          } catch (...) {
            setPaused(dialogWasPaused);
            throw;
          }
        } else if (command == Command::Quit) {
          if (dialogs.Active()) {
            quitPending = true;
            dialogs.Cancel();
            return;
          }
          preferencesFile.Save(preferences);
          session.Close();
          resetPresentation();
          running = false;
        } else if (command == Command::Fullscreen) {
          if (!SDL_SetWindowFullscreen(
                  video.Window(), !(SDL_GetWindowFlags(video.Window()) & SDL_WINDOW_FULLSCREEN)))
            throw std::runtime_error(SDL_GetError());
          if (session.Machine())
            video.Redraw();
          else
            video.ShowIdle();
        } else if (session.Machine()) {
          switch (command) {
          case Command::Unload:
            session.Close();
            resetPresentation();
            paused = false;
            steps = 0;
            SDL_SetWindowRelativeMouseMode(video.Window(), false);
            video.ShowIdle();
            refresh();
            break;
          case Command::Pause:
            steps = 0;
            setPaused(!paused);
            break;
          case Command::FrameAdvance:
            setPaused(true);
            ++steps;
            break;
          case Command::Reset:
          case Command::PowerCycle:
            session.Reset(command == Command::PowerCycle);
            input.ResetSampling();
            resetPresentation();
            video.ClearPicture();
            steps = 0;
            if (!paused && !focusPaused)
              audio.Resume();
            refresh();
            break;
          case Command::Screenshot: {
            const auto path = ScreenshotPath(preferences, preferencesFile);
            video.SaveScreenshot(path);
            const auto title = "Screenshot saved: " + core::PathText(path);
            ui.SetStatus(title);
            break;
          }
          default:
            break;
          }
        }
      } catch (const std::exception &error) {
        report(error);
      }
    };

    while (running) {
      SDL_Event event;
      while (running && SDL_PollEvent(&event)) {
        const bool capturedKeyboard = ui.WantsKeyboard();
        ui.ProcessEvent(event);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_HIDDEN ||
            event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_DROP_BEGIN)
          menu.ResetHotkeys();
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
          execute(DesktopCommand::Quit);
        else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
          if (!capturedKeyboard && !ui.WantsKeyboard())
            if (const auto command = menu.KeyCommand(event.key))
              execute(*command);
          if (capturedKeyboard || ui.WantsKeyboard())
            menu.ResetHotkeys();
        } else if (event.type == SDL_EVENT_DROP_FILE && !dialogs.Active() &&
                   !ui.PreferencesEditor().Active() && !ui.ErrorOpen()) {
          try {
            LaunchRequest request;
            request.paths[0] = event.drop.data;
            open(request);
          } catch (const std::exception &error) {
            report(error);
          }
        } else if (event.type == SDL_EVENT_WINDOW_EXPOSED ||
                   event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
          if (session.Machine())
            video.Redraw();
          else
            video.ShowIdle();
        }
      }
      const auto uiNow = SDL_GetTicksNS();
      const bool uiFrame = uiNow >= nextUiFrame;
      if (uiFrame) {
        ui.Frame();
        nextUiFrame = uiNow + 16'000'000;
      }
      while (running) {
        const auto command = menu.TakeCommand();
        if (!command)
          break;
        execute(*command);
      }
      if (!running)
        break;
      if (const auto changes = ui.PreferencesEditor().TakeChanges()) {
        try {
          applyPreferences(*changes);
          ui.PreferencesEditor().Applied();
        } catch (const std::exception &error) {
          ui.PreferencesEditor().Rejected(error.what());
        }
      }
      if (preferencesActive && !ui.PreferencesEditor().Active()) {
        preferencesActive = false;
        input.ResetSampling();
        setPaused(preferencesWasPaused);
      }
      if (errorActive && !ui.ErrorOpen()) {
        errorActive = false;
        setPaused(errorWasPaused);
      }
      try {
        if (auto selected = dialogs.Poll()) {
          if (quitPending) {
            quitPending = false;
            execute(DesktopCommand::Quit);
            continue;
          }
          if (selected->request)
            open(*selected->request);
          else
            setPaused(dialogWasPaused);
          if (!selected->error.empty())
            throw std::runtime_error(selected->error);
        }
      } catch (const std::exception &error) {
        setPaused(dialogWasPaused);
        report(error);
      }
      auto *emulator = session.Machine();
      const bool focused = WindowFocused(video.Window());
      const bool suspend = !focused && preferences.focus == FocusBehavior::Pause;
      input.SetEnabled(focused || preferences.focus == FocusBehavior::AllowInput);
      input.SetUiCapture(ui.WantsKeyboard(), ui.WantsPointer());
      const bool muted = preferences.mute || (preferences.muteUnfocused && !focused);
      if (audioMuted != muted) {
        audio.SetVolume(preferences.volume, muted);
        audioMuted = muted;
      }
      const bool capture = emulator && focused && !paused && !dialogs.Active() &&
                           !ui.WantsPointer() &&
                           (devices[0] == core::ControllerDevice::Mouse ||
                            devices[1] == core::ControllerDevice::Mouse);
      if (SDL_GetWindowRelativeMouseMode(video.Window()) != capture)
        SDL_SetWindowRelativeMouseMode(video.Window(), capture);
      if (focusPaused != suspend) {
        focusPaused = suspend;
        resetPresentation();
        if (!focusPaused && !paused && emulator)
          audio.Resume();
        refresh();
      }
      if (!emulator || dialogs.Active() || ui.PreferencesEditor().Active() || ui.ErrorOpen() ||
          ((paused || focusPaused) && !steps)) {
        if (uiFrame)
          video.Redraw();
        SDL_Delay(2);
        continue;
      }
      if (picturePending) {
        const auto now = SDL_GetTicksNS();
        if (now < presentationTime) {
          SDL_DelayPrecise(std::min<uint64_t>(presentationTime - now, 2'000'000));
          continue;
        }
        video.PresentPending();
        pacer.Presented(SDL_GetTicksNS());
        picturePending = false;
        continue;
      }
      if (audio.IsPlaying() && audio.QueuedMilliseconds() >= 80.0) {
        pacer.Reset();
        SDL_Delay(1);
        continue;
      }
      if (core::IsLightGun(devices[1])) {
        input.SetGunViewport(video.InputViewport(),
                             (SDL_GetWindowFlags(video.Window()) & SDL_WINDOW_INPUT_FOCUS) != 0);
        const unsigned count = devices[1] == core::ControllerDevice::Justifiers ? 2 : 1;
        std::array<core::LightGunState, 2> aim;
        for (unsigned gun = 0; gun < count; ++gun)
          aim[gun] = input.PollLightGun(1, gun, emulator->CurrentFrame());
        video.SetGunAim(aim, count);
      }
      const auto before = emulator->CurrentMasterCycles();
      const auto result = emulator->StepFrame();
      if (paused || focusPaused) {
        --steps;
        video.PresentPending();
        audio.Clear();
      } else if (audio.IsPlaying()) {
        presentationTime =
            pacer.Schedule(SDL_GetTicksNS(), result.masterCycles - before,
                           emulator->GetTiming().MasterClockHz(), audio.QueuedMilliseconds());
        picturePending = true;
      } else {
        pacer.Reset();
      }
      if (emulator->CurrentFrame() % 300 == 0) {
        try {
          session.Flush();
        } catch (const std::exception &error) {
          report(error);
        }
      }
    }
  } catch (const std::exception &error) {
    std::cerr << "Playback failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
} // namespace snes::frontend
