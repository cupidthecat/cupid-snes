#include "snes/core/gameboy/Cartridge.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using snes::core::gameboy::Cartridge;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<uint8_t> Image(unsigned banks = 2, uint8_t ram = 4) {
  std::vector<uint8_t> rom(banks * 0x4000);
  for (unsigned bank = 0; bank < banks; ++bank)
    std::fill_n(rom.begin() + bank * 0x4000, 0x4000, uint8_t(bank));
  rom[0x147] = 0xfc;
  rom[0x149] = ram;
  return rom;
}

void Register(Cartridge &cartridge, unsigned index, uint8_t value) {
  cartridge.Write(0x4000, 0x10);
  cartridge.Write(uint16_t(0xa000 + index), value);
}

void BankingAndBattery() {
  Cartridge cartridge;
  Check(cartridge.Load(Image(512)), "Load the generated Camera cartridge");
  Check(cartridge.Read(0x4000) == 1, "Camera starts on ROM bank one");
  cartridge.Write(0x2000, 0);
  Check(cartridge.Read(0x4000) == 0, "Camera permits ROM bank zero");
  cartridge.Write(0x3000, 0x81);
  Check(cartridge.Read(0x4000) == 0x81,
        "Both ROM selector addresses replace the same eight-bit bank");
  cartridge.Write(0x4000, 15);
  cartridge.Write(0xa123, 0x51);
  Check(cartridge.Read(0xa123) == 0xff, "Disabled RAM rejects writes but remains readable");
  cartridge.Write(0, 0x1a);
  cartridge.Write(0xa123, 0x51);
  cartridge.Write(0, 0);
  Check(cartridge.Read(0xa123) == 0x51, "RAM reads ignore the write-enable gate");
  cartridge.Write(0x4000, 0x8f);
  Check(cartridge.Read(0xa123) == 0x51, "RAM selection retains four bank bits");
  Register(cartridge, 0, 0x86);
  for (unsigned address = 0xa000; address < 0xc000; address += 128)
    Check(cartridge.Read(uint16_t(address)) == 6, "The control register mirrors every 128 bytes");
  Register(cartridge, 1, 0xff);
  Register(cartridge, 0x36, 0xff);
  Check(cartridge.Read(0xa001) == 0 && cartridge.Read(0xa036) == 0 && cartridge.Read(0xa000) == 6,
        "Other registers read zero and invalid writes leave control flags intact");

  cartridge.Write(0, 10);
  cartridge.Write(0x4000, 0);
  cartridge.Write(0xa100, 0x5a);
  Check(cartridge.Read(0xa100) == 0, "The live image overlays the stored RAM byte");
  const auto saved = cartridge.SaveBattery();
  Check(saved.size() == 131072 && saved[0x100] == 0x5a && saved[15 * 8192 + 0x123] == 0x51,
        "Battery data contains raw RAM without replacing it with image pixels");
  cartridge.Reset();
  cartridge.LoadBattery(saved);
  cartridge.Write(0x4000, 15);
  Check(cartridge.Read(0xa123) == 0x51, "Reset and battery reload retain Camera RAM");
  Check(cartridge.Load(Image(2, 0)), "Load a Camera header without RAM");
  Check(!cartridge.Read(0xa000), "Absent RAM leaves the bus undriven");
  Register(cartridge, 0, 6);
  Check(cartridge.Read(0xa000) == 6, "Register mapping remains accessible without RAM");
}

void CaptureTiming() {
  for (unsigned alignment = 0; alignment < 8; ++alignment)
    for (unsigned highGain : {0u, 128u})
      for (unsigned exposure : {0u, 1u, 4096u, 65535u}) {
        Cartridge cartridge;
        Check(cartridge.Load(Image()), "Load the capture timing cartridge");
        cartridge.Advance(alignment);
        Register(cartridge, 1, uint8_t(highGain));
        Register(cartridge, 2, uint8_t(exposure >> 8));
        Register(cartridge, 3, uint8_t(exposure));
        Register(cartridge, 0, 1);
        const unsigned duration = 129792 + (highGain ? 0 : 2048) + exposure * 64 + (alignment & 4);
        cartridge.Advance(100000, true);
        Register(cartridge, 0, 6);
        Check(cartridge.Read(0xa000) == 7, "Sleep and cancellation keep capture active");
        Register(cartridge, 2, 0);
        Register(cartridge, 3, 0);
        cartridge.Write(0, 10);
        cartridge.Write(0x4000, 1);
        cartridge.Write(0xa000, 0x71);
        Check(cartridge.Read(0xa000) == 0, "Capture blocks RAM reads and writes in every bank");
        cartridge.Advance(duration - 1);
        cartridge.Write(0x4000, 0x10);
        Check(cartridge.Read(0xa000) == 7, "Capture retains its original exposure deadline");
        cartridge.Advance(1);
        Check(cartridge.Read(0xa000) == 6, "The final capture clock clears only the busy flag");
        cartridge.Write(0x4000, 1);
        Check(cartridge.Read(0xa000) == 0xff, "A write attempted during capture leaves RAM intact");
      }
}

void NoiseAndFrameInput() {
  Cartridge cartridge;
  Check(cartridge.Load(Image()), "Load the input cartridge");
  std::array<uint8_t, 128 * 112> frame{};
  Check(cartridge.SetCameraFrame(frame) && !cartridge.SetCameraFrame(std::array<uint8_t, 3>{}),
        "Sensor input requires one complete frame or an empty span");
  unsigned requests = 0;
  cartridge.SetCameraNoiseSource([&] {
    ++requests;
    return uint8_t(42);
  });
  cartridge.SetCameraEntropy(0, true);
  Register(cartridge, 0, 0);
  Register(cartridge, 0, 1);
  Register(cartridge, 0, 0);
  Register(cartridge, 1, 0);
  Check(requests == 3, "Every control write draws one noise byte, including cancellation");
  cartridge.SetCameraEntropy(0, false);
  Register(cartridge, 0, 0);
  Check(requests == 3, "Disabled entropy does not consume the supplied noise source");
  cartridge.Reset();
  cartridge.SetCameraEntropy(0, true);
  Register(cartridge, 0, 0);
  Check(requests == 4 && cartridge.Read(0xa000) == 0,
        "Reset retains the input provider while clearing capture registers");
}

void ImageConversion() {
  constexpr std::array<uint64_t, 64> expected = {
      0xecc40dacd46edfadull, 0xadf59562041d5c85ull, 0xba249b266daea675ull, 0xad8c6ec769ad9fe7ull,
      0x592c0363ddaef772ull, 0x7453c0fc88bd1221ull, 0xc234d40572f5f4d7ull, 0x27d88ce936f20b82ull,
      0xc6dce240cd857bdaull, 0xee003e06e68ec6d4ull, 0x689388d59a57a4c4ull, 0x18e5fb195bc4c1c6ull,
      0x37ddf42aea05f9e0ull, 0x09631305e1b1a06bull, 0x4944d1ad1d3f87d1ull, 0xc50dd2577f909a8cull,
      0x33d82dcf0fc6c53eull, 0x3c6dc4d7f30c1406ull, 0x7aa0319c2acaf362ull, 0x11ae8289870b57a2ull,
      0xbc250d2e88b5bac4ull, 0x9ebd02274bacc28aull, 0xc336fd7dfc8c58a0ull, 0x215e5e89806bb351ull,
      0x064d3a6a4f481c79ull, 0xb978b0578754ce62ull, 0x5a144d879b4ea7d1ull, 0x95dae77807feaea2ull,
      0x86f40a21733fc975ull, 0xb858ab33c3a78d79ull, 0xac3334588b1058b8ull, 0xa1ff3240ecffd551ull,
      0xeddfc6b732bb37c1ull, 0xc7844d223b5a5d4full, 0x0428f438e9142471ull, 0x453f1a1c1760cce0ull,
      0xa2522a6821f16081ull, 0x835ba14789735587ull, 0x96d339431ee110d4ull, 0x46b45590411bfa2eull,
      0xe8d1e4a93e01ca3full, 0x3f614b24596a03d9ull, 0x731114a6aa559e20ull, 0x1f90ff260e213661ull,
      0x0304158645c7e574ull, 0xe0e3885d70d3727full, 0x6a8232c297abec80ull, 0xedd1152c909ead6aull,
      0x5ac4149efbe06b57ull, 0x78524d4e17c2f716ull, 0x60bcafa1b8960affull, 0xc53086916954fb70ull,
      0xec424d5e298fc265ull, 0x53f065c9a5e031cbull, 0xd20c72dbd65cca5aull, 0x110242d1dc5cf3baull,
      0x70e9f96278daeaa8ull, 0x39a40dfc3ee52eefull, 0xedc4092787584778ull, 0xe0f4567243b928abull,
      0x54e550c8b93376faull, 0xb78879c65f94dbbfull, 0xbe5c62093b7b3e49ull, 0x519251271d8fd07aull,
  };
  for (unsigned fixture = 0; fixture < expected.size(); ++fixture) {
    Cartridge cartridge;
    Check(cartridge.Load(Image()), "Load the image conversion cartridge");
    cartridge.SetCameraEntropy(0x123456789abcdef0ull + fixture, fixture % 3 != 0);
    if (fixture < 48) {
      std::array<uint8_t, 128 * 112> frame;
      for (unsigned y = 0; y < 112; ++y)
        for (unsigned x = 0; x < 128; ++x)
          frame[y * 128 + x] = uint8_t(x * 3 + y * 5 + (x * y) % 23 + fixture * 11);
      Check(cartridge.SetCameraFrame(frame), "Provide a generated sensor frame");
    }
    Register(cartridge, 0, uint8_t((fixture & 3) * 2));
    Register(cartridge, 1, uint8_t(fixture < 32 ? fixture : fixture < 48 ? 0xe4 : fixture & 31));
    constexpr std::array<unsigned, 4> exposures = {2047, 2048, 65535, 4096};
    const unsigned exposure = fixture < 32 ? 4096 : exposures[fixture % 4];
    Register(cartridge, 2, uint8_t(exposure >> 8));
    Register(cartridge, 3, uint8_t(exposure));
    Register(cartridge, 4, uint8_t(((fixture & 7) << 4) | (fixture & 8)));
    for (unsigned cell = 0; cell < 16; ++cell) {
      Register(cartridge, 6 + cell * 3, uint8_t(31 + cell * 7));
      Register(cartridge, 7 + cell * 3, uint8_t(99 + cell * 5));
      Register(cartridge, 8 + cell * 3, uint8_t(177 + cell * 3));
    }
    cartridge.Write(0x4000, 0);
    uint64_t hash = 14695981039346656037ull;
    for (unsigned i = 0; i < 3584; ++i)
      hash = (hash ^ cartridge.Read(uint16_t(0xa100 + i)).value()) * 1099511628211ull;
    Check(hash == expected[fixture],
          "Gain, exposure, edge rounding, dither, noise, and tile bytes match");
  }
}
} // namespace

int main() {
  try {
    BankingAndBattery();
    CaptureTiming();
    NoiseAndFrameInput();
    ImageConversion();
    std::puts("Handheld Camera registers, capture timing, images and battery checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
