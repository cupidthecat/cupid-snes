#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "desktop/ApplicationDriver.hpp"
#include "desktop/DesktopApplication.hpp"
#include "imgui_internal.h"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/StateStorage.hpp"

#include <cstdio>
#include <fstream>

using namespace snes::tests::desktop;
using namespace snes::frontend;
namespace fs = std::filesystem;

namespace {
struct View {
    bool memory = false, cheats = false, paused = false, focused = false, editing = false;
    float x = 0, y = 0, bodyY = 0, row = 0, firstRowY = 0, checkX = 0, checkY = 0, applyX = 0,
          applyY = 0;
    ImGuiID desired = 0, nav = 0;
    ImGuiID rootDesired = 0, active = 0;
    std::string navWindow;
};

View Observe(SDL_WindowID id, const char* control = "", const fs::path& picture = {}) {
    View view;
    OnMain([&] {
        auto* window = SDL_GetWindowFromID(id);
        view.paused = window && std::string_view(SDL_GetWindowTitle(window)).find("Paused") !=
                                    std::string_view::npos;
        if (!ImGui::GetCurrentContext())
            return;
        auto& context = *ImGui::GetCurrentContext();
        if (auto* memory = ImGui::FindWindowByName("Memory and RAM Search")) {
            view.memory = memory->Active;
            view.x = memory->WorkRect.Min.x;
            view.y = memory->WorkRect.Min.y;
            view.row = ImGui::GetFrameHeightWithSpacing();
            if (auto* tabs = context.TabBars.GetByKey(memory->GetID("Memory tools"))) {
                view.bodyY = tabs->BarRect.Max.y + ImGui::GetStyle().ItemSpacing.y;
                view.firstRowY = view.bodyY + view.row * 5 + ImGui::GetTextLineHeight() +
                                 ImGui::GetStyle().CellPadding.y * 3 + 8;
                view.desired = ImHashStr(control, 0, tabs->SelectedTabId);
            }
            view.nav = context.NavId;
            view.rootDesired = memory->GetID(control);
            view.active = context.ActiveId;
            view.navWindow = context.NavWindow ? context.NavWindow->Name : "";
            view.focused = view.nav == view.desired;
            view.editing = context.ActiveId == view.desired;
        }
        if (auto* cheats = ImGui::FindWindowByName("Cheats")) {
            view.cheats = cheats->Active;
            view.applyX = cheats->Pos.x + 28;
            view.applyY = cheats->DC.CursorPosPrevLine.y + 12;
            for (const auto* child : context.Windows)
                if (child->ParentWindow == cheats &&
                    std::string_view(child->Name).find("Cheat list") != std::string_view::npos) {
                    view.checkX = child->WorkRect.Min.x + 8;
                    view.checkY = child->WorkRect.Min.y + 8;
                }
        }
        if (!picture.empty()) {
            auto* renderer = window ? SDL_GetRenderer(window) : nullptr;
            auto* surface = renderer ? SDL_RenderReadPixels(renderer, nullptr) : nullptr;
            Check(surface, "Memory tools have a rendered picture");
            const auto path = snes::core::PathText(picture);
            Check(SDL_SaveBMP(surface, path.c_str()), "Save memory tool picture");
            SDL_DestroySurface(surface);
        }
    });
    return view;
}

void Focus(SDL_WindowID window, const char* control) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto view = Observe(window, control);
        Check(view.memory, "Memory window stays open during keyboard navigation");
        if (view.focused || view.editing)
            return;
        Key(window, SDL_SCANCODE_TAB);
    }
    const auto view = Observe(window, control);
    std::fprintf(
        stderr, "Focus failed for %s, expected=%08x root=%08x nav=%08x active=%08x window=%s\n",
        control, view.desired, view.rootDesired, view.nav, view.active, view.navWindow.c_str());
    throw std::runtime_error("Memory control can be reached with keyboard navigation");
}

void Activate(SDL_WindowID window, const char* control) {
    Focus(window, control);
    Key(window, SDL_SCANCODE_RETURN);
}

void SetText(SDL_WindowID window, const char* control, const char* text) {
    Focus(window, control);
    if (!Observe(window, control).editing)
        Key(window, SDL_SCANCODE_RETURN);
    Wait([&] { return Observe(window, control).editing; }, "Text control accepts keyboard focus");
    Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    Text(window, text);
}

std::vector<uint8_t> Image() {
    auto bytes = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    constexpr uint8_t guest[]{0x78, 0xa9, 0x5a, 0x8f, 0x34, 0x12, 0x7e, 0xdb};
    std::copy(std::begin(guest), std::end(guest), bytes.begin());
    return bytes;
}

void Workflow(const fs::path& directory, const fs::path& pictures) {
    const auto rom = directory / "memory-game.sfc", profile = directory / "settings.cfg";
    const auto image = Image();
    {
        std::ofstream file(rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), image.size());
    }
    PreferencesFile file(profile);
    auto preferences = file.Load();
    preferences.focus = FocusBehavior::AllowInput;
    file.Save(preferences);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage states(request);
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(Created, &window), "Observe the application window");
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "Memory application creates a window");
            Check(!returned, "Application remains available");
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            Key(window, SDL_SCANCODE_P);
            Wait([&] { return Observe(window).paused; }, "Pause the generated guest");
            Key(window, SDL_SCANCODE_F2);
            const auto quick0 = states.Path(StateStorage::Quick(0));
            Wait([&] { return fs::is_regular_file(quick0); },
                 "Capture the machine before inspection");
            const auto before = StateFile::Read(quick0);
            Key(window, SDL_SCANCODE_M, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).memory; },
                 "Memory shortcut opens the physical byte viewer");
            SetText(window, "Address (hex)", "FFFFFFFF");
            Activate(window, "Go");
            Check(Observe(window).memory, "Invalid addresses leave the viewer available for retry");
            SetText(window, "Address (hex)", "7E1234");
            Activate(window, "Go");
            if (!pictures.empty())
                Observe(window, "", pictures / "memory-viewer.bmp");
            const auto dumpDirectory = states.Directory() / "memory-dumps";
            {
                std::ofstream blocked(dumpDirectory);
                blocked << "retained blocker";
            }
            Activate(window, "Export region");
            Check(fs::is_regular_file(dumpDirectory),
                  "Failed export preserves a path that is a file");
            if (!pictures.empty())
                Observe(window, "", pictures / "memory-export-error.bmp");
            fs::remove(dumpDirectory);
            Activate(window, "Export region");
            fs::path dump;
            Wait(
                [&] {
                    if (fs::is_directory(dumpDirectory))
                        for (const auto& entry : fs::directory_iterator(dumpDirectory))
                            if (entry.path().extension() == ".bin")
                                dump = entry.path();
                    return !dump.empty();
                },
                "Memory export reaches its documented folder");
            std::ifstream exported(dump, std::ios::binary);
            std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(exported),
                                       std::istreambuf_iterator<char>()};
            Check(bytes.size() == snes::core::MemoryBus::WramSize && bytes[0x1234] == 0x5a,
                  "Export contains the actual physical WRAM witness");
            exported.close();
            Key(window, SDL_SCANCODE_F5);
            Key(window, SDL_SCANCODE_N);
            SDL_Event drop{};
            drop.type = SDL_EVENT_DROP_FILE;
            drop.drop.windowID = window;
            drop.drop.data = request.paths[0].c_str();
            Push(drop);
            Key(window, SDL_SCANCODE_ESCAPE);
            Wait([&] { return !Observe(window).memory; }, "Close the memory viewer");
            Check(Observe(window).paused, "Closing memory tools restores the previous pause state");
            Key(window, SDL_SCANCODE_F7);
            Key(window, SDL_SCANCODE_F2);
            const auto quick1 = states.Path(StateStorage::Quick(1));
            Wait([&] { return fs::is_regular_file(quick1); }, "Capture after inspection");
            Check(StateFile::Read(quick1).machine == before.machine,
                  "Inspection and blocked commands preserve the exact paused machine");
            Key(window, SDL_SCANCODE_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).memory; }, "RAM search opens");
            SetText(window, "Value (decimal, 0x or $ hex)", "bad");
            Activate(window, "Search");
            SetText(window, "Value (decimal, 0x or $ hex)", "90");
            Activate(window, "Search");
            if (!pictures.empty())
                Observe(window, "", pictures / "ram-search.bmp");
            auto view = Observe(window);
            Click(window, view.x + 30, view.firstRowY);
            if (!pictures.empty())
                Observe(window, "", pictures / "ram-search-selected.bmp");
            Activate(window, "Create cheat...");
            Wait([&] { return Observe(window).cheats; },
                 "Selected search result opens a reviewable cheat group");
            view = Observe(window);
            Click(window, view.checkX, view.checkY);
            Click(window, view.applyX, view.applyY);
            const auto cheatPath = states.Directory() / "cheats.ccht";
            Wait([&] { return fs::is_regular_file(cheatPath); },
                 "Generated cheat can be saved and applied");
            const auto book = CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console);
            Check(book.entries.size() == 1 && book.entries[0].codes == "7E1234=5A" &&
                      book.entries[0].enabled,
                  "RAM search generates the matching mapped CPU address and sampled byte");
            Key(window, SDL_SCANCODE_ESCAPE);
            Key(window, SDL_SCANCODE_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).memory; },
                 "Search reopens with its retained candidates");
            view = Observe(window);
            Click(window, view.x + 30, view.firstRowY);
            Activate(window, "Create cheat...");
            Wait([&] { return Observe(window).cheats; },
                 "Retained candidates remain usable after closing the search");
            Key(window, SDL_SCANCODE_ESCAPE);
            Check(CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console).entries.size() ==
                      1,
                  "Closing the generated draft preserves the existing cheat file");
            Key(window, SDL_SCANCODE_M, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).memory; }, "Memory viewer reopens before quitting");
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
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "Memory application exits normally");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        fs::temp_directory_path() /
        ("cupid-memory-app-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(directory))
        return 2;

    struct Cleanup {
        fs::path path;

        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
            SDL_Quit();
        }
    } cleanup{directory};

    try {
        const auto pictures = argc > 1 ? snes::core::FilePath(argv[1]) : fs::path{};
        if (!pictures.empty())
            fs::create_directories(pictures);
        Workflow(directory, pictures);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
