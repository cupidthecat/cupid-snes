#include "snes/core/BsMemory.hpp"
#include "snes/core/Obc1.hpp"
#include "snes/core/SetaProtocol.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void BsMemory::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(image_.size()));
  archive.Expect(readOnly_);
  archive.Expect(blockCount_);
  archive.Expect(masterFrequency_);
  const auto writable = archive.Number(writable_);
  archive.Validate(!readOnly_ || !writable, "Read-only pack cannot enable flash writes");
  archive.Number(vendor_);
  archive.Number(device_);
  archive.Bounded(serial_, uint64_t{0}, uint64_t{0xffffffffffff});
  archive.Array(blocks_, [](auto &fields, auto &block) {
    fields.Number(block.erasures);
    fields.Number(block.locked);
    fields.Number(block.erasing);
    fields.Number(block.status);
  });
  archive.Array(pages_, [](auto &fields, auto &page) { fields.Array(page); });
  const auto maximum =
      image_.empty()
          ? 0u
          : uint32_t(std::min<size_t>(image_.size() - 1, std::numeric_limits<uint32_t>::max()));
  archive.Array(queue_, [maximum](auto &fields, auto &entry) {
    fields.Bounded(entry.address, 0u, maximum);
    fields.Number(entry.value);
  });
  archive.Bounded(queued_, 0u, 3u);
  archive.Bounded(readyMode_, 0u, 3u);
  archive.Enumeration(mode_, Mode::Array, Mode::ExtendedStatus);
  archive.Number(status_);
  archive.Number(global_);
  archive.Bounded(waitingErase_, -1, int(blockCount_) - 1);
  constexpr auto margin = int64_t(std::numeric_limits<uint32_t>::max()) * 2'000'000;
  archive.Bounded(clock_, std::numeric_limits<int64_t>::min() + margin,
                  int64_t(masterFrequency_) * 1'000'000);
}

void Obc1::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(ram_.size()));
  const auto base = archive.Number(base_);
  archive.Validate(base == 0x1800 || base == 0x1c00, "Invalid object controller base");
  archive.Bounded(object_, 0u, 127u);
}

void St011::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(ram_.size()));
  archive.Array(work_);
  archive.Array(packet_);
  archive.Array(board_);
  const auto command = archive.Number(command_);
  const auto received = archive.Number(received_);
  const auto required = archive.Number(required_);
  archive.Validate(required
                       ? ((command == 1 && required == 128) || (command == 2 && required == 4)) &&
                             received < required
                       : received == 0,
                   "Invalid board command packet");
}

void St018::SerializeState(detail::StateArchive &archive) {
  archive.Expect(uint64_t(ram_.size()));
  archive.Bounded(command_, 0u, 0xffffffu);
  archive.Bounded(commandBytes_, 0u, 2u);
  archive.Bounded(pass_, 0u, 3u);
  archive.Bounded(outputCount_, 0u, 3u);
  archive.Number(awaitingParameter_);
}
} // namespace snes::core
