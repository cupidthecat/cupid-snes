#include "snes/core/debug/Cheat.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace snes::core {
namespace {
uint32_t Hex(std::string_view text) {
  uint32_t value = 0;
  for (const auto ch : text) {
    const auto digit = std::string_view("0123456789abcdef").find(ch);
    if (digit == std::string_view::npos)
      throw std::invalid_argument("Cheat codes must use hexadecimal digits");
    value = (value << 4) | uint32_t(digit);
  }
  return value;
}
std::string HexText(uint32_t value, unsigned digits) {
  std::string result(digits, '0');
  for (auto it = result.rbegin(); it != result.rend(); ++it, value >>= 4)
    *it = "0123456789ABCDEF"[value & 15];
  return result;
}
CheatCode Decode(std::string text, CheatSystem system) {
  for (auto &ch : text)
    if (ch >= 'A' && ch <= 'Z')
      ch = char(ch + ('a' - 'A'));
  const size_t digits = system == CheatSystem::Console ? 6 : 4;
  if ((text.size() == digits + 3 || text.size() == digits + 6) && text[digits] == '=') {
    CheatCode code{Hex(std::string_view(text).substr(0, digits)),
                   uint8_t(Hex(std::string_view(text).substr(digits + 1, 2))),
                   {}};
    if (text.size() == digits + 6) {
      if (text[digits + 3] != '?')
        throw std::invalid_argument("Conditional cheats use address=compare?value");
      code.compare = code.value;
      code.value = uint8_t(Hex(std::string_view(text).substr(digits + 4, 2)));
    }
    return code;
  }
  if (system == CheatSystem::Console && text.size() == 9 && text[4] == '-') {
    text.erase(4, 1);
    uint32_t decoded = 0;
    for (auto ch : text) {
      const auto digit = std::string_view("df4709156bc8a23e").find(ch);
      if (digit == std::string_view::npos)
        throw std::invalid_argument("Invalid Game Genie code");
      decoded = (decoded << 4) | uint32_t(digit);
    }
    // Each entry names the encoded bit supplying one address bit, low to high.
    constexpr std::array<unsigned, 24> positions{6,  7,  8,  9,  16, 17, 18, 19, 14, 15, 0,  1,
                                                 20, 21, 22, 23, 2,  3,  4,  5,  10, 11, 12, 13};
    uint32_t address = 0;
    for (unsigned bit = 0; bit < positions.size(); ++bit)
      address |= ((decoded >> positions[bit]) & 1) << bit;
    return {address, uint8_t(decoded >> 24), {}};
  }
  if (system == CheatSystem::Handheld &&
      ((text.size() == 7 && text[3] == '-') ||
       (text.size() == 11 && text[3] == '-' && text[7] == '-'))) {
    text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
    const auto first = Hex(std::string_view(text).substr(0, 6));
    CheatCode code{((first & 15) ^ 15) << 12 | ((first >> 4) & 0xfff), uint8_t(first >> 16), {}};
    if (text.size() == 9) {
      Hex(std::string_view(text).substr(6));
      const auto encoded = uint8_t((Hex(std::string_view(text).substr(6, 1)) << 4) |
                                   Hex(std::string_view(text).substr(8, 1)));
      code.compare = uint8_t(((encoded >> 2) | (encoded << 6)) ^ 0xba);
    }
    return code;
  }
  if (text.size() == 8) {
    const auto value = Hex(text);
    if (system == CheatSystem::Console)
      return {value >> 8, uint8_t(value), {}};
    if ((value >> 24) == 1)
      return {((value & 255) << 8) | ((value >> 8) & 255), uint8_t(value >> 16), {}};
  }
  throw std::invalid_argument(system == CheatSystem::Console
                                  ? "Use Game Genie, PAR, address=value or address=compare?value"
                                  : "Use handheld Game Genie, GameShark 01, or address=value");
}
} // namespace

std::vector<CheatCode> DecodeCheats(std::string_view text, CheatSystem system) {
  if (text.size() > 65536)
    throw std::invalid_argument("A cheat group may contain at most 65536 bytes");
  std::vector<CheatCode> result;
  bool requireCode = false;
  while (!text.empty()) {
    const auto end = text.find_first_of("+\r\n");
    auto part = text.substr(0, end);
    const auto first = part.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
      part = part.substr(first, part.find_last_not_of(" \t") - first + 1);
      result.push_back(Decode(std::string(part), system));
      requireCode = false;
      if (result.size() > 4096)
        throw std::invalid_argument("A cheat group may contain at most 4096 codes");
    } else if (end != std::string_view::npos && text[end] == '+') {
      throw std::invalid_argument("Missing code before '+'");
    }
    if (end == std::string_view::npos)
      break;
    if (text[end] == '+')
      requireCode = true;
    text.remove_prefix(end + 1);
  }
  if (requireCode)
    throw std::invalid_argument("Missing code after '+'");
  if (result.empty())
    throw std::invalid_argument("Enter at least one cheat code");
  return result;
}

std::string FormatCheat(const CheatCode &code, CheatSystem system) {
  const unsigned digits = system == CheatSystem::Console ? 6 : 4;
  if (code.address >= (uint32_t(1) << (digits * 4)))
    throw std::invalid_argument("Cheat address is outside the selected system");
  return HexText(code.address, digits) + "=" +
         (code.compare ? HexText(*code.compare, 2) + "?" : std::string{}) + HexText(code.value, 2);
}

void CheatEngine::Set(std::span<const CheatCode> codes, const Reader &read, const Writer &write) {
  if (codes.size() > 4096)
    throw std::invalid_argument("At most 4096 active cheat codes are supported");
  std::vector<Entry> next;
  next.reserve(codes.size());
  for (const auto &code : codes) {
    if (code.address > 0xffffff)
      throw std::invalid_argument("Cheat address is outside the console bus");
    if (std::none_of(next.begin(), next.end(),
                     [&](const auto &entry) { return entry.code == code; }))
      next.push_back({code});
  }
  if (next.size() == entries_.size() &&
      std::equal(next.begin(), next.end(), entries_.begin(),
                 [](const auto &a, const auto &b) { return a.code == b.code; }))
    return;
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
    if (it->active)
      write(it->code.address, it->original);
  for (auto &entry : next) {
    entry.original = read(entry.code.address);
    const auto old = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const auto &item) { return item.code == entry.code; });
    entry.active = old != entries_.end()
                       ? old->active
                       : !entry.code.compare || *entry.code.compare == entry.original;
    if (entry.active)
      write(entry.code.address, entry.code.value);
  }
  entries_.swap(next);
}
void CheatEngine::Refresh(const Writer &write) const {
  for (const auto &entry : entries_)
    if (entry.active)
      write(entry.code.address, entry.code.value);
}
void CheatEngine::Forget() noexcept {
  entries_.clear();
}
} // namespace snes::core
