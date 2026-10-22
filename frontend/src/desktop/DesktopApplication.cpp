#include "DesktopApplication.hpp"
#include "DesktopMenu.hpp"
#include "FileDialogs.hpp"
#include "FramePacer.hpp"
#include "SdlAudioOutput.hpp"
#include "SdlInputProvider.hpp"
#include "SdlVideoOutput.hpp"
#include "snes/core/FilePath.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace snes::frontend {
namespace {
std::filesystem::path ScreenshotPath() {
  char *raw = SDL_GetPrefPath("Cupid", "Cupid-SNES");
  if (!raw)
    throw std::runtime_error(SDL_GetError());
  auto directory = core::FilePath(raw) / "screenshots";
  SDL_free(raw);
  std::filesystem::create_directories(directory);
  return directory / ("screenshot-" + std::to_string(SDL_GetTicksNS()) + ".bmp");
}
} // namespace

int RunDesktop(core::Emulator::Config config, std::array<core::ControllerDevice, 2> devices,
               const std::optional<LaunchRequest> &initial) {
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
    SdlVideoOutput::Config videoConfig;
    videoConfig.title = "Cupid-SNES";
    videoConfig.vsync = false;
    videoConfig.deferPresentation = true;
    SdlVideoOutput video(videoConfig);
    SdlAudioOutput audio;
    SdlInputProvider input;
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
    DesktopMenu menu(video.Window());
    FileDialogs dialogs(video.Window());
    FramePacer pacer;
    bool running = true, paused = false, dialogWasPaused = false, picturePending = false;
    bool quitPending = false;
    unsigned steps = 0;
    uint64_t presentationTime = 0;

    const auto refresh = [&] {
      std::string title = "Cupid-SNES";
      if (const auto *request = session.Loaded()) {
        const auto index = request->kind == LaunchRequest::Kind::Handheld ? 1 : 0;
        title += " - " + core::PathText(core::FilePath(request->paths[index]).filename());
      }
      if (paused && session.Machine())
        title += " (Paused)";
      SDL_SetWindowTitle(video.Window(), title.c_str());
      menu.Update(session.Machine() != nullptr, paused);
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
      if (!paused && session.Machine())
        audio.Resume();
      refresh();
    };
    const auto report = [&](const std::exception &error) {
      std::cerr << error.what() << '\n';
      const bool previous = paused;
      setPaused(true);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cupid-SNES", error.what(), video.Window());
      setPaused(previous);
    };
    const auto open = [&](const LaunchRequest &request) {
      session.Open(request);
      resetPresentation();
      video.ClearPicture();
      paused = false;
      steps = 0;
      const bool mouse = devices[0] == core::ControllerDevice::Mouse ||
                         devices[1] == core::ControllerDevice::Mouse;
      if (!SDL_SetWindowRelativeMouseMode(video.Window(), mouse))
        throw std::runtime_error(std::string("Cannot capture mouse: ") + SDL_GetError());
      audio.Resume();
      refresh();
      const auto &header = session.Machine()->LoadedCartridge()->Header();
      std::cout << "Loaded: " << header.title << " (" << core::MappingName(header.mapping) << ")\n";
    };
    if (initial)
      open(*initial);
    else
      video.ShowIdle();

    const auto execute = [&](DesktopCommand command) {
      try {
        if (!running || (dialogs.Active() && command != DesktopCommand::Quit))
          return;
        using Command = DesktopCommand;
        if (command <= Command::OpenPowerFest) {
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
            resetPresentation();
            video.ClearPicture();
            steps = 0;
            if (!paused)
              audio.Resume();
            refresh();
            break;
          case Command::Screenshot: {
            const auto path = ScreenshotPath();
            video.SaveScreenshot(path);
            const auto title = "Screenshot saved: " + core::PathText(path);
            SDL_SetWindowTitle(video.Window(), title.c_str());
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
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
          execute(DesktopCommand::Quit);
        else if (event.type == SDL_EVENT_KEY_DOWN) {
          if (const auto command = DesktopMenu::KeyCommand(event.key))
            execute(*command);
        } else if (event.type == SDL_EVENT_DROP_FILE && !dialogs.Active()) {
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
      while (running) {
        const auto command = menu.TakeCommand();
        if (!command)
          break;
        execute(*command);
      }
      if (!running)
        break;
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
      if (!emulator || dialogs.Active() || (paused && !steps)) {
        SDL_Delay(10);
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
      if (paused) {
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
      if (emulator->CurrentFrame() % 300 == 0)
        session.Flush();
    }
  } catch (const std::exception &error) {
    std::cerr << "Playback failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
} // namespace snes::frontend
