#include "snes/core/PowerOnState.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using snes::core::detail::StateArchive;
namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class Operation> void Reject(Operation operation, const char *message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, message);
}

struct Numbers {
  uint8_t byte = 0xa5;
  bool enabled = true;
  uint16_t word = 0x1234;
  int32_t negative = -2;
  float sample = 1.5f;
  double zero = -0.0;
  bool operator==(const Numbers &) const = default;
  void Serialize(StateArchive &archive) {
    archive.Number(byte);
    archive.Number(enabled);
    archive.Number(word);
    archive.Number(negative);
    archive.Number(sample);
    archive.Number(zero);
  }
};
void PortableNumbers() {
  Numbers original;
  StateArchive writer;
  original.Serialize(writer);
  const std::vector<uint8_t> expected{0xa5, 1,    0x34, 0x12, 0xfe, 0xff, 0xff, 0xff, 0, 0,
                                      0xc0, 0x3f, 0,    0,    0,    0,    0,    0,    0, 0x80};
  Check(writer.Bytes() == expected,
        "Numbers use fixed little-endian fields without native padding");
  Numbers restored{2, false, 3, 4, 5, 6};
  const auto untouched = restored;
  StateArchive reader(writer.Bytes());
  restored.Serialize(reader);
  Check(restored == untouched, "Reading a complete state does not mutate live values");
  reader.Commit();
  Check(restored == original && std::signbit(restored.zero),
        "Commit restores exact numeric bits including negative zero");
  Reject([&] { reader.Commit(); }, "A restored archive cannot be committed twice");
  Reject([&] { reader.Number(restored.byte); }, "A restored archive cannot queue more changes");
  for (size_t size = 0; size < expected.size(); ++size) {
    restored = untouched;
    StateArchive truncated(std::span<const uint8_t>(expected).first(size));
    Reject([&] { restored.Serialize(truncated); }, "Every truncated numeric prefix is rejected");
    Reject([&] { truncated.Commit(); }, "A failed parse cannot commit earlier fields");
    Check(restored == untouched, "Truncation preserves the entire previous state");
  }
}

struct Sample {
  uint32_t clock = 0;
  int16_t amplitude = 0;
  bool muted = false;
  bool operator==(const Sample &) const = default;
};
struct Device {
  uint64_t cycles = 0;
  std::array<uint16_t, 3> memory{};
  std::vector<Sample> queue;
  bool operator==(const Device &) const = default;
  void Serialize(StateArchive &archive) {
    archive.Section(0x54455354, [&](auto &section) {
      section.Number(cycles);
      section.Array(memory);
      section.Vector(queue, 4, [](auto &fields, Sample &sample) {
        fields.Number(sample.clock);
        fields.Number(sample.amplitude);
        fields.Number(sample.muted);
      });
    });
  }
};
void StructuredChanges() {
  Device saved{0x123456789abcdef0, {0x12, 0x3456, 0x789a}, {{17, -31, true}, {22, 45, false}}};
  StateArchive writer;
  saved.Serialize(writer);
  const auto &bytes = writer.Bytes();
  Check(bytes.size() == 40 && bytes[0] == 0x54 && bytes[1] == 0x53 && bytes[2] == 0x45 &&
            bytes[3] == 0x54 && bytes[4] == 32 && bytes[22] == 2,
        "Tagged sections and record counts have explicit wire lengths");
  Device live{9, {1, 2, 3}, {{1, 2, false}}};
  const auto previous = live;
  auto *memory = live.memory.data();
  StateArchive reader(bytes);
  live.Serialize(reader);
  Check(live == previous, "Nested arrays and queues stay unchanged before commit");
  reader.Commit();
  Check(live == saved && live.memory.data() == memory,
        "Fixed mapped memory retains its address while queues restore");
  auto trailing = bytes;
  trailing.push_back(0);
  live = previous;
  StateArchive extra(trailing);
  live.Serialize(extra);
  Reject([&] { extra.Commit(); }, "Trailing root data rejects the complete restore");
  Check(live == previous, "Trailing bytes do not publish nested changes");
  auto wrongTag = bytes;
  wrongTag[0] ^= 1;
  StateArchive tag(wrongTag);
  Reject([&] { live.Serialize(tag); }, "A section for other hardware is rejected");
  Reject([&] { tag.Commit(); }, "A rejected section cannot restore earlier fields");
  auto wrongSize = bytes;
  ++wrongSize[4];
  wrongSize.push_back(0);
  StateArchive section(wrongSize);
  Reject([&] { live.Serialize(section); }, "Extra bytes inside a section are rejected");
  Reject([&] { section.Commit(); }, "A malformed section cannot commit");
  Check(live == previous, "Malformed section metadata preserves the device");

  std::vector<uint8_t> mapped{3, 7, 11};
  auto *base = mapped.data();
  StateArchive blobWriter;
  std::array<uint8_t, 3> replacement{19, 23, 29};
  blobWriter.Array(replacement);
  StateArchive blob(blobWriter.Bytes());
  blob.Fixed(std::span<uint8_t>(mapped));
  blob.Commit();
  Check(mapped == std::vector<uint8_t>{19, 23, 29} && mapped.data() == base,
        "A mapped vector can restore bytes without reallocating its bus view");
}

void ValidationAndLimits() {
  enum class Mode : uint8_t { Idle, Running, Stopped };
  uint8_t first = 9;
  bool enabled = false;
  std::array<uint8_t, 2> invalidBoolean{1, 2};
  StateArchive boolean(invalidBoolean);
  boolean.Number(first);
  Reject([&] { boolean.Number(enabled); }, "Boolean values have a canonical representation");
  Reject([&] { boolean.Commit(); },
         "A fully consumed invalid boolean cannot commit earlier changes");
  Check(first == 9 && !enabled, "Invalid boolean data leaves prior values intact");
  Mode mode = Mode::Idle;
  std::array<uint8_t, 1> invalidMode{3};
  StateArchive enumeration(invalidMode);
  Reject([&] { enumeration.Enumeration(mode, Mode::Idle, Mode::Stopped); },
         "Undefined enumeration values are rejected");
  Reject([&] { enumeration.Commit(); }, "Invalid enumeration data cannot commit");
  Check(mode == Mode::Idle, "Enumeration validation preserves the current mode");
  std::array<uint8_t, 4> infinity{0, 0, 0x80, 0x7f};
  float sample = 3;
  StateArchive nonfinite(infinity);
  Reject([&] { nonfinite.Number(sample); }, "Non-finite audio/filter values are rejected");
  Reject([&] { nonfinite.Commit(); }, "Invalid floating-point data cannot commit");
  StateArchive writer;
  sample = std::numeric_limits<float>::quiet_NaN();
  Reject([&] { writer.Number(sample); }, "Non-finite live values cannot be encoded");

  std::vector<uint16_t> values{17, 19};
  std::array<uint8_t, 4> enormous{0xff, 0xff, 0xff, 0xff};
  StateArchive count(enormous);
  Reject([&] { count.Vector(values, 100); },
         "Oversized collection counts are rejected before allocation");
  Reject([&] { count.Commit(); }, "Invalid collection lengths cannot commit");
  Check(values == std::vector<uint16_t>{17, 19},
        "Malformed collections preserve their existing storage");
  struct Large {
    std::array<uint8_t, 4096> bytes;
  };
  std::vector<uint8_t> payload(4 + 32769);
  payload[0] = 1;
  payload[1] = 0x80;
  std::vector<Large> large;
  StateArchive memory(payload);
  Reject(
      [&] {
        memory.Vector(large, 100000,
                      [](auto &fields, auto &value) { fields.Number(value.bytes[0]); });
      },
      "Sparse records cannot exceed the restore memory budget");
  Check(large.empty(), "A memory-limit failure does not resize live collections");

  std::array<uint8_t, 1> one{1};
  StateArchive visitor(one);
  Reject([&] { visitor.Section(1, [](auto &) {}); }, "Short section headers are rejected");
  StateArchive object(one);
  uint8_t live = 7;
  Reject(
      [&] {
        object.Object(live, [](auto &fields, auto &value) {
          fields.Number(value);
          throw std::runtime_error("Invalid device state");
        });
      },
      "A device validation failure cancels its staged object");
  Reject([&] { object.Commit(); }, "A device validation exception poisons the whole restore");
  Check(live == 7, "Device validation does not change live state");
}

void RestorePreparation() {
  std::array<uint8_t, 1> bytes{7};
  uint8_t live = 8, observed = 0;
  unsigned preparations = 0;
  StateArchive reader(bytes);
  reader.BeforeRestore([&]() noexcept {
    ++preparations;
    observed = live;
    live = 99;
  });
  reader.Number(live);
  Check(live == 8 && preparations == 0, "Reading leaves live execution and fields intact");
  reader.Commit();
  Check(live == 7 && observed == 8 && preparations == 1,
        "Execution is discarded after validation and before staged fields are assigned");
  Reject([&] { reader.Commit(); }, "Preparation cannot run twice");
  std::array<uint8_t, 2> trailing{7, 9};
  StateArchive invalid(trailing);
  invalid.BeforeRestore([&]() noexcept { ++preparations; });
  invalid.Number(live);
  Reject([&] { invalid.Commit(); }, "Trailing data prevents execution preparation");
  StateArchive writer;
  writer.BeforeRestore([&]() noexcept { ++preparations; });
  Check(preparations == 1, "Neither rejected input nor a write discards execution");
}

void EntropyContinuation() {
  using namespace snes::core;
  for (const auto mode : {EntropyMode::None, EntropyMode::Low, EntropyMode::High}) {
    PowerOnState original(mode, 0x91827364, 17);
    for (unsigned index = 0; index < 41; ++index)
      original.Value(index);
    StateArchive writer;
    original.SerializeState(writer);
    std::array<uint64_t, 128> expected;
    for (unsigned index = 0; index < expected.size(); ++index)
      expected[index] = original.Value(index);
    PowerOnState restored(EntropyMode::High, 1, 2);
    StateArchive reader(writer.Bytes());
    restored.SerializeState(reader);
    reader.Commit();
    for (unsigned index = 0; index < expected.size(); ++index)
      Check(restored.Value(index) == expected[index],
            "Restored startup entropy continues the same stream in every mode");
    auto invalid = writer.Bytes();
    invalid[12] &= 0xfe;
    StateArchive bad(invalid);
    Reject([&] { restored.SerializeState(bad); }, "An even startup stream increment is rejected");
    Reject([&] { bad.Commit(); }, "Invalid startup entropy cannot be restored");
  }
}
} // namespace

int main() {
  try {
    PortableNumbers();
    StructuredChanges();
    ValidationAndLimits();
    RestorePreparation();
    EntropyContinuation();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  std::puts("State archive byte order, atomic validation, bounds and entropy continuation passed");
  return 0;
}
