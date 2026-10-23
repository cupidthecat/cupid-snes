#include "DesktopUi.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "snes/core/FilePath.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
void LoadFonts() {
  auto &io = ImGui::GetIO();
  ImFont *base = nullptr;
  const char *windows = std::getenv("WINDIR");
  const auto fonts = windows ? core::FilePath(windows) / "Fonts" : std::filesystem::path{};
  const std::array basePaths{
      fonts.empty() ? std::filesystem::path{} : fonts / "segoeui.ttf",
      std::filesystem::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
      std::filesystem::path("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf")};
  for (const auto &path : basePaths) {
    std::error_code error;
    if (!path.empty() && std::filesystem::is_regular_file(path, error)) {
      base = io.Fonts->AddFontFromFileTTF(core::PathText(path).c_str(), 16);
      if (base)
        break;
    }
  }
  if (!base)
    base = io.Fonts->AddFontDefault();
  const std::array fallbackPaths{
      fonts.empty() ? std::filesystem::path{} : fonts / "YuGothR.ttc",
      std::filesystem::path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
      fonts.empty() ? std::filesystem::path{} : fonts / "seguiemj.ttf",
      std::filesystem::path("/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf")};
  for (const auto &path : fallbackPaths) {
    std::error_code error;
    if (!path.empty() && std::filesystem::is_regular_file(path, error)) {
      ImFontConfig config;
      config.MergeMode = true;
      io.Fonts->AddFontFromFileTTF(core::PathText(path).c_str(), 16, &config);
    }
  }
  io.FontDefault = base;
}
} // namespace

struct DesktopUi::Impl {
  ImGuiContext *context = nullptr;
  SDL_Renderer *renderer = nullptr;
  DesktopMenu menu;
  PreferencesDialog preferences;
  std::unique_ptr<StateManagerDialog> states;
  std::string status = "Ready", error;
  bool platformReady = false, rendererReady = false, errorOpening = false, errorOpen = false;

  void Activate() const {
    ImGui::SetCurrentContext(context);
  }
  ~Impl() {
    if (!context)
      return;
    Activate();
    if (rendererReady)
      ImGui_ImplSDLRenderer3_Shutdown();
    if (platformReady)
      ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context);
  }
};

DesktopUi::DesktopUi(SDL_Window *window, SDL_Renderer *renderer) : impl_(std::make_unique<Impl>()) {
  auto &self = *impl_;
  IMGUI_CHECKVERSION();
  self.context = ImGui::CreateContext();
  self.renderer = renderer;
  self.states = std::make_unique<StateManagerDialog>(renderer);
  auto &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigNavCaptureKeyboard = false;
  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.FontSizeBase = 16;
  style.WindowRounding = 5;
  style.FrameRounding = 3;
  style.FramePadding = {8, 4};
  style.ItemSpacing = {8, 8};
  style.Colors[ImGuiCol_WindowBg].w = 1;
  LoadFonts();
  self.platformReady = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  if (!self.platformReady)
    throw std::runtime_error("Cannot initialize desktop input");
  self.rendererReady = ImGui_ImplSDLRenderer3_Init(renderer);
  if (!self.rendererReady)
    throw std::runtime_error("Cannot initialize desktop renderer");
}
DesktopUi::~DesktopUi() = default;
void DesktopUi::ProcessEvent(const SDL_Event &event) {
  impl_->Activate();
  if (!impl_->preferences.ProcessEvent(event))
    ImGui_ImplSDL3_ProcessEvent(&event);
}
void DesktopUi::Frame() {
  auto &self = *impl_;
  self.Activate();
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  self.menu.Draw(!self.preferences.Active() && !self.states->Active() && !self.errorOpen);
  const auto *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos({viewport->Pos.x, viewport->Pos.y + viewport->Size.y - BottomInset()});
  ImGui::SetNextWindowSize({viewport->Size.x, float(BottomInset())});
  if (ImGui::Begin("##Status", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoBringToFrontOnFocus))
    ImGui::TextUnformatted(self.status.c_str());
  ImGui::End();
  self.preferences.Draw();
  self.states->Draw();
  if (self.errorOpening) {
    ImGui::OpenPopup("Cupid-SNES error");
    self.errorOpening = false;
  }
  ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
  ImGui::SetNextWindowSize({std::min(500.0f, viewport->Size.x - 24), 0}, ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("Cupid-SNES error", &self.errorOpen,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(self.error.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Close", {80, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      self.errorOpen = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::Render();
}
void DesktopUi::Render() {
  impl_->Activate();
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), impl_->renderer);
}
int DesktopUi::TopInset() const {
  impl_->Activate();
  return int(std::ceil(ImGui::GetFrameHeight()));
}
int DesktopUi::BottomInset() const {
  impl_->Activate();
  return int(std::ceil(ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 2));
}
bool DesktopUi::WantsKeyboard() const {
  impl_->Activate();
  return impl_->preferences.Active() || impl_->states->Active() || impl_->errorOpen ||
         ImGui::GetIO().WantCaptureKeyboard ||
         ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}
bool DesktopUi::WantsPointer() const {
  impl_->Activate();
  return impl_->preferences.Active() || impl_->states->Active() || impl_->errorOpen ||
         ImGui::GetIO().WantCaptureMouse;
}
DesktopMenu &DesktopUi::Menu() {
  return impl_->menu;
}
PreferencesDialog &DesktopUi::PreferencesEditor() {
  return impl_->preferences;
}
StateManagerDialog &DesktopUi::StateManager() {
  return *impl_->states;
}
void DesktopUi::SetStatus(std::string text) {
  impl_->status = std::move(text);
}
void DesktopUi::ShowError(std::string text) {
  impl_->error = std::move(text);
  impl_->errorOpening = impl_->errorOpen = true;
}
bool DesktopUi::ErrorOpen() const {
  return impl_->errorOpen;
}
} // namespace snes::frontend
