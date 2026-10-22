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
constexpr std::array keyNames{"up", "down", "left", "right", "a",     "b",
                              "x",  "y",    "l",    "r",     "start", "select"};
constexpr std::array keyMembers{
    &SdlInputProvider::KeyMap::up,    &SdlInputProvider::KeyMap::down,
    &SdlInputProvider::KeyMap::left,  &SdlInputProvider::KeyMap::right,
    &SdlInputProvider::KeyMap::a,     &SdlInputProvider::KeyMap::b,
    &SdlInputProvider::KeyMap::x,     &SdlInputProvider::KeyMap::y,
    &SdlInputProvider::KeyMap::l,     &SdlInputProvider::KeyMap::r,
    &SdlInputProvider::KeyMap::start, &SdlInputProvider::KeyMap::select};

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
      {"path.firmware", p.firmwareDirectory},
      {"path.screenshots", p.screenshotDirectory},
      {"recent.count", std::to_string(p.recent.size())}};
  for (unsigned index = 0; index < keyNames.size(); ++index)
    result["key." + std::string(keyNames[index])] = std::to_string(p.keyboard.*keyMembers[index]);
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
  if (scale < 1 || scale > 6 || volume < 0 || volume > 200 || int(focus) < 0 || int(focus) > 2 ||
      recent.size() > recentLimit)
    throw std::invalid_argument("Preferences contain an unsupported setting");
  for (const auto member : keyMembers)
    if (keyboard.*member < SDL_SCANCODE_UNKNOWN || keyboard.*member >= SDL_SCANCODE_COUNT)
      throw std::invalid_argument("Keyboard preference contains an invalid scancode");
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
  p.focus = FocusBehavior(number("input.focus", int(p.focus), 0, 2));
  for (unsigned index = 0; index < keyNames.size(); ++index)
    p.keyboard.*keyMembers[index] =
        SDL_Scancode(number("key." + std::string(keyNames[index]), p.keyboard.*keyMembers[index], 0,
                            SDL_SCANCODE_COUNT - 1));
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
