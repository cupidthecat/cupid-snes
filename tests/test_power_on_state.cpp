#include "snes/core/Emulator.hpp"
#include "snes/core/PowerOnState.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
uint64_t Digest(std::span<const uint8_t> bytes) {
  uint64_t value = 0xcbf29ce484222325ull;
  for (auto byte : bytes)
    value = (value ^ byte) * 0x100000001b3ull;
  return value;
}
void EntropySequences() {
  struct Case {
    EntropyMode mode;
    uint32_t seed, sequence;
    size_t bytes;
    uint64_t memory, following;
  };
  constexpr Case cases[]{
      {EntropyMode::None, 0x00000000, 0x00000000, 0, 0xcbf29ce484222325ull, 0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x00000000, 0x00000000, 512, 0x7da144b97d054b25ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x00000000, 0x00000000, 131072, 0xc74b47c8c74a2325ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x00000000, 0x9abcdef0, 0, 0xcbf29ce484222325ull, 0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x00000000, 0x9abcdef0, 512, 0x7da144b97d054b25ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x00000000, 0x9abcdef0, 131072, 0xc74b47c8c74a2325ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x12345678, 0x00000000, 0, 0xcbf29ce484222325ull, 0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x12345678, 0x00000000, 512, 0x7da144b97d054b25ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x12345678, 0x00000000, 131072, 0xc74b47c8c74a2325ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x12345678, 0x9abcdef0, 0, 0xcbf29ce484222325ull, 0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x12345678, 0x9abcdef0, 512, 0x7da144b97d054b25ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::None, 0x12345678, 0x9abcdef0, 131072, 0xc74b47c8c74a2325ull,
       0x8421ae126c7ced25ull},
      {EntropyMode::Low, 0x00000000, 0x00000000, 0, 0xcbf29ce484222325ull, 0x099cc60586d458a5ull},
      {EntropyMode::Low, 0x00000000, 0x00000000, 512, 0xf5af91b6118b22cdull, 0x0292294fa60d5d28ull},
      {EntropyMode::Low, 0x00000000, 0x00000000, 131072, 0x46250e3b12b53406ull,
       0xdbbbcce6a149f64cull},
      {EntropyMode::Low, 0x00000000, 0x9abcdef0, 0, 0xcbf29ce484222325ull, 0x265d0c77e4e08ee8ull},
      {EntropyMode::Low, 0x00000000, 0x9abcdef0, 512, 0x2469ebce796c7c25ull, 0x1da07fce4f0e62bcull},
      {EntropyMode::Low, 0x00000000, 0x9abcdef0, 131072, 0xb39a8eb1b60de712ull,
       0xe59ffe158b2c073dull},
      {EntropyMode::Low, 0x12345678, 0x00000000, 0, 0xcbf29ce484222325ull, 0x65984396b2369a1eull},
      {EntropyMode::Low, 0x12345678, 0x00000000, 512, 0xc5b16ba4e0a5b5c5ull, 0x92d5454e99c111bbull},
      {EntropyMode::Low, 0x12345678, 0x00000000, 131072, 0xf31bc17d683f6a5full,
       0x3c61073dc0476afbull},
      {EntropyMode::Low, 0x12345678, 0x9abcdef0, 0, 0xcbf29ce484222325ull, 0x8defcba7332ff6f2ull},
      {EntropyMode::Low, 0x12345678, 0x9abcdef0, 512, 0x4a5b7ca8322e2cf9ull, 0xe45f5087c8b6eb93ull},
      {EntropyMode::Low, 0x12345678, 0x9abcdef0, 131072, 0xc09cc6fbb0281073ull,
       0xab8f42cf738fe8f9ull},
      {EntropyMode::High, 0x00000000, 0x00000000, 0, 0xcbf29ce484222325ull, 0x210210adfeab78bbull},
      {EntropyMode::High, 0x00000000, 0x00000000, 512, 0xdcf58170afa38b48ull,
       0x2336b1e835448e04ull},
      {EntropyMode::High, 0x00000000, 0x00000000, 131072, 0xd3cee51ee0090e85ull,
       0x62b4cc8b925e9898ull},
      {EntropyMode::High, 0x00000000, 0x9abcdef0, 0, 0xcbf29ce484222325ull, 0x659c2a57ffb80c70ull},
      {EntropyMode::High, 0x00000000, 0x9abcdef0, 512, 0x830426a5497c5aa7ull,
       0x05392626ee6f07d6ull},
      {EntropyMode::High, 0x00000000, 0x9abcdef0, 131072, 0xb7ca18667e19751aull,
       0xc2fd2ddfd19716b1ull},
      {EntropyMode::High, 0x12345678, 0x00000000, 0, 0xcbf29ce484222325ull, 0x71bf0368a430416bull},
      {EntropyMode::High, 0x12345678, 0x00000000, 512, 0x9496e7461af02030ull,
       0x9ef2c566c73509f5ull},
      {EntropyMode::High, 0x12345678, 0x00000000, 131072, 0xe61382832d839678ull,
       0x0a99980d38cb41e0ull},
      {EntropyMode::High, 0x12345678, 0x9abcdef0, 0, 0xcbf29ce484222325ull, 0xc59998fdf435d641ull},
      {EntropyMode::High, 0x12345678, 0x9abcdef0, 512, 0x6fb9eab774907071ull,
       0xa88ea04ce4f3052dull},
      {EntropyMode::High, 0x12345678, 0x9abcdef0, 131072, 0x1603bd3573c02853ull,
       0x0960512940e79481ull},
  };
  for (const auto &test : cases) {
    PowerOnState state(test.mode, test.seed, test.sequence);
    std::vector<uint8_t> memory(test.bytes, 0xa5);
    state.Fill(memory);
    Check(Digest(memory) == test.memory,
          "Startup entropy fills memory from the selected mode and seed");
    std::array<uint8_t, 128> following{};
    for (unsigned index = 0; index < 16; ++index) {
      const auto value = state.Value();
      for (unsigned byte = 0; byte < 8; ++byte)
        following[index * 8 + byte] = uint8_t(value >> (byte * 8));
    }
    Check(Digest(following) == test.following,
          "Memory filling retains the entropy sequence used by later registers");
  }
  PowerOnState none(EntropyMode::None, 123);
  Check(none.Value(0xff) == 0xff && none.Value(1) == 1 && none.Value() == 0,
        "Deterministic startup retains register fallback bits");
}
std::vector<uint8_t> Rom() {
  std::vector<uint8_t> rom(32768, 0xea);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0] = 0xdb;
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 0;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 0;
  rom[0x7fd9] = 1;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffc] = 0;
  rom[0x7ffd] = 0x80;
  return rom;
}
uint64_t RegisterDigest(const Ppu &ppu) {
  const auto &i = ppu.GetIO();
  const auto &l = ppu.GetLatch();
  std::vector<uint64_t> values{1,
                               ppu.Configuration().revision2,
                               ppu.Configuration().vramBytes / 2 - 1,
                               l.ppu1.mdr,
                               l.ppu2.mdr,
                               l.vram,
                               l.oam,
                               l.cgram,
                               l.ppu1.bgofs,
                               l.ppu2.bgofs,
                               l.mode7,
                               i.oamBaseAddress,
                               i.oamAddress,
                               i.oamPriority,
                               i.mode7.hoffset,
                               i.mode7.voffset,
                               i.vramIncrementMode,
                               i.vramMapping,
                               i.vramIncrementSize,
                               i.vramAddress,
                               i.mode7.repeat,
                               i.mode7.vflip,
                               i.mode7.hflip,
                               i.mode7.a,
                               i.mode7.b,
                               i.mode7.c,
                               i.mode7.d,
                               i.mode7.x,
                               i.mode7.y,
                               i.cgramAddress,
                               i.cgramAddressLatch,
                               i.extbg,
                               i.pseudoHires,
                               i.mosaic.size};
  for (const auto *bg : {&i.bg1, &i.bg2, &i.bg3, &i.bg4})
    values.insert(values.end(),
                  {bg->tiledataAddress, bg->screenAddress, bg->screenSize, bg->tileSize,
                   bg->aboveEnable, bg->belowEnable, bg->hoffset, bg->voffset, bg->mosaicEnable});
  const auto &o = i.obj;
  values.insert(values.end(), {o.aboveEnable, o.belowEnable, o.interlace, o.baseSize, o.nameselect,
                               o.tiledataAddress});
  for (const auto *w : {&i.bg1.window, &i.bg2.window, &i.bg3.window, &i.bg4.window, &i.obj.window})
    values.insert(values.end(), {w->oneEnable, w->oneInvert, w->twoEnable, w->twoInvert, w->mask,
                                 w->aboveEnable, w->belowEnable});
  const auto &w = i.col.window;
  values.insert(values.end(), {w.oneEnable, w.oneInvert, w.twoEnable, w.twoInvert, w.mask,
                               w.aboveMask, w.belowMask, i.window.oneLeft, i.window.oneRight,
                               i.window.twoLeft, i.window.twoRight});
  const auto &s = i.col;
  values.insert(values.end(),
                {s.blendMode, s.directColor, s.mathMode, s.halve, s.enable[snes::core::Ppu::BG1],
                 s.enable[snes::core::Ppu::BG2], s.enable[snes::core::Ppu::BG3],
                 s.enable[snes::core::Ppu::BG4], s.enable[snes::core::Ppu::OBJ1],
                 s.enable[snes::core::Ppu::COL], uint64_t((s.fixedColor >> 10) & 31),
                 uint64_t((s.fixedColor >> 5) & 31), uint64_t(s.fixedColor & 31)});
  std::vector<uint8_t> bytes;
  for (auto value : values)
    for (unsigned byte = 0; byte < 8; ++byte)
      bytes.push_back(uint8_t(value >> (byte * 8)));
  return Digest(bytes);
}
void ConsoleStartup() {
  struct Snapshot {
    std::array<uint64_t, 4> memory;
    uint64_t registers;
  };
  struct Case {
    EntropyMode mode;
    uint32_t seed;
    std::array<Snapshot, 2> states;
  };
  constexpr Case cases[]{
      {EntropyMode::None,
       0x00000000,
       {{{{0xc74b47c8c74a2325ull, 0xc74b47c8c74a2325ull, 0x7da144b97d054b25ull,
           0x9986ba9af2021da5ull},
          0x4c934ee319cfd84cull},
         {{0xe24d3c0ca022bc6eull, 0x7fbe2e766bb48705ull, 0x7da144b97d054b25ull,
           0x9986ba9af2021da5ull},
          0x4c934ee319cfd84cull}}}},
      {EntropyMode::None,
       0x12345678,
       {{{{0xc74b47c8c74a2325ull, 0xc74b47c8c74a2325ull, 0x7da144b97d054b25ull,
           0x9986ba9af2021da5ull},
          0x4c934ee319cfd84cull},
         {{0xe24d3c0ca022bc6eull, 0x7fbe2e766bb48705ull, 0x7da144b97d054b25ull,
           0x9986ba9af2021da5ull},
          0x4c934ee319cfd84cull}}}},
      {EntropyMode::Low,
       0x00000000,
       {{{{0x46250e3b12b53406ull, 0xc31e6df3548f37a8ull, 0x941aad37806da0a2ull,
           0x9986ba9af2021da5ull},
          0x37c735354519584dull},
         {{0x4f37530e9023861cull, 0x8346d86ef25576d4ull, 0x2e73d3304590df1dull,
           0x9986ba9af2021da5ull},
          0x4dd8f8529ea2f656ull}}}},
      {EntropyMode::Low,
       0x12345678,
       {{{{0xf31bc17d683f6a5full, 0x0ed110929a942443ull, 0x2f381415aab22c25ull,
           0x9986ba9af2021da5ull},
          0xfd662a015fab26d6ull},
         {{0x968bf595ddf939c7ull, 0xc049f4893a79f937ull, 0x1969238ba5a3a995ull,
           0x9986ba9af2021da5ull},
          0x1eac1b9e2f598489ull}}}},
      {EntropyMode::High,
       0x00000000,
       {{{{0xd3cee51ee0090e85ull, 0xbe148e1e701beb6dull, 0x27c16c690715138cull,
           0x9986ba9af2021da5ull},
          0xe63fdfcf46042927ull},
         {{0x866f57bf51dc379bull, 0x4a1eb6d8081ae02cull, 0x2302ff0e27476e83ull,
           0x9986ba9af2021da5ull},
          0x5259c4ecd8f13bd4ull}}}},
      {EntropyMode::High,
       0x12345678,
       {{{{0xe61382832d839678ull, 0xef720b0320e18a5aull, 0x3beed847bb68fbf6ull,
           0x9986ba9af2021da5ull},
          0x0727be8161f0e874ull},
         {{0xe27a5e54d524aea4ull, 0x366e032068d6a1a9ull, 0x2b47e824305a1a58ull,
           0x9986ba9af2021da5ull},
          0xbb48c2868e0eb5efull}}}},
  };
  for (const auto &test : cases) {
    Emulator::Config config;
    config.entropyMode = test.mode;
    config.entropySeed = test.seed;
    config.ppu.model = Ppu::RenderModel::Progressive;
    config.ppu.vramBytes = 131072;
    auto machine = std::make_unique<Emulator>(config);
    Check(machine->LoadCartridge(Rom()), "Startup fixture loads");
    for (unsigned warm = 0; warm < 2; ++warm) {
      if (warm) {
        auto &bus = machine->GetBus();
        for (const auto [address, value] : std::array<std::pair<uint32_t, uint8_t>, 21>{
                 {{0x7e0000, 0xa5}, {0x7fffff, 0x5a}, {0x2100, 0x80}, {0x2115, 0x80},
                  {0x2116, 0},      {0x2117, 0},      {0x2118, 0x34}, {0x2119, 0x12},
                  {0x2116, 0},      {0x2117, 0x80},   {0x2118, 0x78}, {0x2119, 0x56},
                  {0x2121, 0},      {0x2122, 0x67},   {0x2122, 0x45}, {0x2102, 0},
                  {0x2103, 0},      {0x2104, 0xa5},   {0x2104, 0x5a}, {0x420d, 0},
                  {0x4200, 0}}})
          bus.Write(address, value);
        Check(machine->Reset(), "Seeded warm reset succeeds");
      }
      const auto &ppu = machine->GetPpu();
      const auto &expected = test.states[warm];
      Check(Digest({machine->GetBus().WramData(), MemoryBus::WramSize}) == expected.memory[0],
            "Console startup applies the selected WRAM entropy and retains WRAM on reset");
      Check(Digest({reinterpret_cast<const uint8_t *>(ppu.VramData()),
                    Ppu::VramStorageWords * 2}) == expected.memory[1],
            "Video power initializes physical VRAM and warm reset retains both banks");
      Check(Digest({reinterpret_cast<const uint8_t *>(ppu.CgramData()), Ppu::CgramColors * 2}) ==
                expected.memory[2],
            "Each video reset initializes palette memory from the shared entropy sequence");
      Check(Digest({ppu.OamData(), Ppu::OamSize}) == expected.memory[3],
            "Progressive video reset clears sprite memory");
      Check(RegisterDigest(ppu) == expected.registers,
            "Startup register entropy retains field widths and memory-generation order");
    }
  }
  for (uint32_t sequence : {0u, 0x9abcdef0u}) {
    Emulator::Config defaults;
    defaults.entropySeed = 0;
    defaults.entropySequence = sequence;
    auto machine = std::make_unique<Emulator>(defaults);
    Check(machine->LoadCartridge(Rom()), "Default startup fixture loads");
    const uint64_t expected = sequence == 0 ? 0x46250e3b12b53406ull : 0xb39a8eb1b60de712ull;
    Check(Digest({machine->GetBus().WramData(), MemoryBus::WramSize}) == expected,
          "Default console power uses low entropy with the selected seed and sequence");
  }
}

} // namespace
int main() {
  try {
    EntropySequences();
    ConsoleStartup();
    std::puts("Console startup entropy and reset checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
