#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "desktop/ApplicationDriver.hpp"
#include "desktop/DesktopApplication.hpp"
#include "imgui_internal.h"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/StateStorage.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

using namespace snes::tests::desktop;
using namespace snes::frontend;
namespace fs = std::filesystem;
namespace {
struct View {
  bool active = false, paused = false, focused = false, editing = false;
  float tabX = 0, tabY = 0;
  std::string selectedTab;
};
View Observe(SDL_WindowID id, const char *control = "", const char *tab = "",
             const fs::path &picture = {}) {
  View result;
  OnMain([&] {
    auto *host = SDL_GetWindowFromID(id);
    result.paused =
        host && std::string_view(SDL_GetWindowTitle(host)).find("Paused") != std::string_view::npos;
    if (!ImGui::GetCurrentContext())
      return;
    auto &context = *ImGui::GetCurrentContext();
    auto *window = ImGui::FindWindowByName("Console CPU debugger");
    result.active = window && window->Active;
    if (!result.active)
      return;
    const auto rootId = window->GetID(control);
    result.focused = context.NavId == rootId;
    result.editing = context.ActiveId == rootId;
    for (auto *child : context.Windows)
      if (child->ParentWindow == window &&
          std::string_view(child->Name).find("Debugger content") != std::string_view::npos)
        if (auto *tabs = context.TabBars.GetByKey(child->GetID("Debugger views"))) {
          const auto id = ImHashStr(control, 0, tabs->SelectedTabId);
          result.focused |= context.NavId == id;
          result.editing |= context.ActiveId == id;
          for (auto &item : tabs->Tabs) {
            const std::string_view name = ImGui::TabBarGetTabName(tabs, &item);
            if (item.ID == tabs->SelectedTabId)
              result.selectedTab = name;
            if (name == tab) {
              result.tabX = tabs->BarRect.Min.x + item.Offset + item.Width / 2;
              result.tabY = tabs->BarRect.GetCenter().y;
            }
          }
        }
    if (!picture.empty()) {
      auto *surface = SDL_RenderReadPixels(SDL_GetRenderer(host), nullptr);
      Check(surface, "Debugger renders a picture");
      Check(SDL_SaveBMP(surface, snes::core::PathText(picture).c_str()), "Save debugger picture");
      SDL_DestroySurface(surface);
    }
  });
  return result;
}
void Focus(SDL_WindowID window, const char *control) {
  for (unsigned count = 0; count < 48; ++count) {
    const auto view = Observe(window, control);
    Check(view.active, "Debugger stays open while navigating");
    if (view.focused || view.editing)
      return;
    Key(window, SDL_SCANCODE_TAB);
  }
  OnMain([&] {
    const auto &g = *ImGui::GetCurrentContext();
    std::fprintf(stderr, "Cannot focus debugger control: %s, nav=%08X active=%08X window=%s\n",
                 control, g.NavId, g.ActiveId, g.NavWindow ? g.NavWindow->Name : "");
  });
  throw std::runtime_error("Debugger controls accept keyboard navigation");
}
void Activate(SDL_WindowID window, const char *control) {
  Focus(window, control);
  Key(window, SDL_SCANCODE_RETURN);
}
void SetText(SDL_WindowID window, const char *control, const char *value) {
  Focus(window, control);
  if (!Observe(window, control).editing)
    Key(window, SDL_SCANCODE_RETURN);
  Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL);
  Text(window, value);
  Key(window, SDL_SCANCODE_RETURN);
}
void Tab(SDL_WindowID window, const char *name) {
  const auto view = Observe(window, "", name);
  Check(view.tabX > 0, "Debugger tab is available");
  OnMain([&] { SDL_WarpMouseInWindow(SDL_GetWindowFromID(window), view.tabX, view.tabY); });
  Click(window, view.tabX, view.tabY);
  Wait([&] { return Observe(window).selectedTab == name; }, "Debugger selects the requested tab");
}
std::vector<fs::path> Traces(const fs::path &directory) {
  std::vector<fs::path> paths;
  if (fs::is_directory(directory))
    for (const auto &entry : fs::directory_iterator(directory))
      if (entry.path().extension() == ".tsv")
        paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end());
  return paths;
}
std::string Read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Workflow(const fs::path &directory, const fs::path &pictures) {
  auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
  constexpr uint8_t program[]{0xa9, 0x5a, 0x8f, 0x34, 0x12, 0x7e, 0xea, 0x80, 0xf7};
  std::copy(std::begin(program), std::end(program), image.begin());
  const auto rom = directory / "debug-game.sfc", profile = directory / "settings.cfg";
  {
    std::ofstream file(rom, std::ios::binary);
    file.write(reinterpret_cast<const char *>(image.data()), image.size());
  }
  PreferencesFile file(profile);
  auto preferences = file.Load();
  preferences.focus = FocusBehavior::AllowInput;
  file.Save(preferences);
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  StateStorage states(request);
  const auto traces = states.Directory() / "traces";
  if (!SDL_getenv("SNES_TEST_WINDOW"))
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
  std::atomic<SDL_WindowID> window{0};
  std::atomic<bool> returned{false};
  Check(SDL_AddEventWatch(Created, &window), "Observe debugger application");
  std::exception_ptr failure;
  std::thread driver([&] {
    try {
      Wait([&] { return window.load() || returned.load(); }, "Application opens its window");
      Check(!returned, "Application stays available");
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      Key(window, SDL_SCANCODE_P);
      Wait([&] { return Observe(window).paused; }, "Pause before opening debugger");
      Key(window, SDL_SCANCODE_D, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
      Wait([&] { return Observe(window).active; }, "Debugger shortcut opens the console controls");
      SetText(window, "Run to (hex)", "invalid");
      Activate(window, "Run to address");
      Check(Observe(window).active, "Invalid address leaves debugger available");
      Tab(window, "Trace");
      Activate(window, "Capture trace");
      SetText(window, "Run to (hex)", "008000");
      Activate(window, "Run to address");
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      Key(window, SDL_SCANCODE_F10);
      Key(window, SDL_SCANCODE_F10);
      Tab(window, "Breakpoints");
      SetText(window, "First (hex)", "1000000");
      Activate(window, "Add breakpoint");
      SetText(window, "First (hex)", "7E1234");
      Activate(window, "Execute");
      Activate(window, "Write");
      Activate(window, "Add breakpoint");
      Key(window, SDL_SCANCODE_F9);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      if (!pictures.empty())
        Observe(window, "", "", pictures / "cpu-breakpoints.bmp");
      Tab(window, "Trace");
      fs::create_directories(states.Directory());
      {
        std::ofstream blocker(traces);
        Check(bool(blocker), "Create an isolated export failure fixture");
        blocker << "keep";
      }
      Activate(window, "Export trace");
      Check(Read(traces) == "keep", "Export failure preserves the obstructing file");
      if (!pictures.empty())
        Observe(window, "", "", pictures / "cpu-export-error.bmp");
      fs::remove(traces);
      Activate(window, "Export trace");
      Wait([&] { return Traces(traces).size() == 1; }, "Trace export retries successfully");
      const auto text = Read(Traces(traces).front());
      Check(text.find("boundary\t") != std::string::npos &&
                text.find("7E1234\t5A") != std::string::npos &&
                text.find("008000\tA9") != std::string::npos,
            "Real controls step instructions, hit watchpoints and export actual accesses");
      if (!pictures.empty())
        Observe(window, "", "", pictures / "cpu-trace.bmp");
      Key(window, SDL_SCANCODE_F5);
      Key(window, SDL_SCANCODE_N);
      SDL_Event drop{};
      drop.type = SDL_EVENT_DROP_FILE;
      drop.drop.windowID = window;
      drop.drop.data = request.paths[0].c_str();
      Push(drop);
      Activate(window, "Export trace");
      Wait([&] { return Traces(traces).size() == 2; }, "A stopped trace can be exported again");
      Check(Read(Traces(traces).back()) == text,
            "Blocked gameplay commands leave debugger execution unchanged");
      Key(window, SDL_SCANCODE_ESCAPE);
      Wait([&] { return !Observe(window).active; }, "Close debugger");
      Check(Observe(window).paused, "Debugger restores the previous pause state");
      Key(window, SDL_SCANCODE_D, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
      Wait([&] { return Observe(window).active; }, "Debugger reopens before quit");
      Quit();
    } catch (...) {
      failure = std::current_exception();
      if (!returned)
        Quit();
    }
  });
  const auto result = RunDesktop(snes::tests::DeterministicConsoleConfig(), {}, request, profile);
  returned = true;
  driver.join();
  SDL_RemoveEventWatch(Created, &window);
  SDL_Quit();
  if (failure)
    std::rethrow_exception(failure);
  Check(result == 0, "Application exits cleanly with debugger open");
}
} // namespace
int main(int argc, char **argv) {
  const auto directory =
      fs::temp_directory_path() /
      ("cupid-cpu-debugger-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(directory);
    fs::path pictures;
    if (argc > 1) {
      pictures = snes::core::FilePath(argv[1]);
      fs::create_directories(pictures);
    }
    Workflow(directory, pictures);
    fs::remove_all(directory);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("CPU debugger application checks passed");
}
