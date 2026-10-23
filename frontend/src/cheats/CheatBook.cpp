#include "CheatBook.hpp"
#include "storage/AtomicFile.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace snes::frontend {
namespace {
constexpr size_t maximumBytes = 1024 * 1024;
const char *SystemName(core::CheatSystem system) {
  return system == core::CheatSystem::Console ? "console" : "handheld";
}
std::string Canonical(const CheatEntry &entry, core::CheatSystem system) {
  std::string text;
  for (const auto &code : core::DecodeCheats(entry.codes, system)) {
    if (!text.empty())
      text += '+';
    text += core::FormatCheat(code, system);
  }
  return text;
}
void End(std::istream &stream) {
  stream >> std::ws;
  if (!stream.eof())
    throw std::runtime_error("Unexpected text in cheat file");
}
} // namespace

std::vector<core::CheatCode> CheatBook::Compile(core::CheatSystem system) const {
  if (entries.size() > 1024)
    throw std::invalid_argument("At most 1024 cheat groups are supported");
  std::vector<core::CheatCode> active;
  size_t total = 0;
  for (const auto &entry : entries) {
    if (entry.name.empty() || entry.name.size() > 4096)
      throw std::invalid_argument("Each cheat needs a name of at most 4096 bytes");
    for (unsigned char ch : entry.name)
      if (ch < 32 || ch == 127)
        throw std::invalid_argument("Cheat names cannot contain control characters");
    auto codes = core::DecodeCheats(entry.codes, system);
    total += codes.size();
    if (total > 4096)
      throw std::invalid_argument("At most 4096 cheat codes are supported per game");
    if (enabled && entry.enabled)
      active.insert(active.end(), codes.begin(), codes.end());
  }
  return active;
}
CheatBook CheatStorage::Read(const std::filesystem::path &path, core::CheatSystem system) {
  const auto bytes = ReadFile(path, maximumBytes);
  if (bytes.empty()) {
    if (std::filesystem::exists(path))
      throw std::runtime_error("Cheat file is empty");
    return {};
  }
  if (std::find(bytes.begin(), bytes.end(), uint8_t(0)) != bytes.end())
    throw std::runtime_error("Cheat file contains a null character");
  std::istringstream stream(std::string(bytes.begin(), bytes.end()));
  std::string line;
  std::getline(stream, line);
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  if (line != std::string("CupidCheats 1 ") + SystemName(system))
    throw std::runtime_error("Unsupported cheat file version or system");
  std::getline(stream, line);
  std::istringstream flags(line);
  std::string key;
  int enabled = -1;
  if (!(flags >> key >> enabled) || key != "enabled" || (enabled != 0 && enabled != 1))
    throw std::runtime_error("Invalid global cheat flag");
  End(flags);
  CheatBook book;
  book.enabled = enabled != 0;
  while (std::getline(stream, line)) {
    std::istringstream row(line);
    CheatEntry entry;
    if (!(row >> key >> enabled >> std::quoted(entry.name) >> std::quoted(entry.codes)) ||
        key != "cheat" || (enabled != 0 && enabled != 1))
      throw std::runtime_error("Invalid cheat entry");
    End(row);
    entry.enabled = enabled != 0;
    book.entries.push_back(std::move(entry));
    if (book.entries.size() > 1024)
      throw std::runtime_error("Too many cheat groups");
  }
  book.Compile(system);
  return book;
}
void CheatStorage::Write(const std::filesystem::path &path, const CheatBook &book,
                         core::CheatSystem system) {
  book.Compile(system);
  std::ostringstream stream;
  stream << "CupidCheats 1 " << SystemName(system) << "\nenabled " << int(book.enabled) << '\n';
  for (const auto &entry : book.entries)
    stream << "cheat " << int(entry.enabled) << ' ' << std::quoted(entry.name) << ' '
           << std::quoted(Canonical(entry, system)) << '\n';
  const auto text = stream.str();
  if (text.size() > maximumBytes)
    throw std::invalid_argument("Cheat file exceeds 1 MiB");
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  WriteFileAtomically(path, {reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
} // namespace snes::frontend
