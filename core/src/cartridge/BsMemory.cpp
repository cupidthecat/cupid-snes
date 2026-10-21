#include "snes/core/BsMemory.hpp"
#include "snes/core/PowerOnState.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
#include <bit>

namespace snes::core {

BsMemory::BsMemory(std::span<uint8_t> image, bool readOnly)
    : image_(image), readOnly_(readOnly), masterFrequency_(Timing::kMasterClockHz) {
  if (!readOnly && image.size() != 0x100000 && image.size() != 0x200000 && image.size() != 0x400000)
    image_ = {};
  blockCount_ = unsigned(std::min<size_t>(image_.size() >> 16, blocks_.size()));
  device_ = image_.size() == 0x100000 ? 0x66a8 : 0x6688;
  Reset();
}

void BsMemory::InitializePageBuffers(PowerOnState &state) {
  if (readOnly_ || image_.empty())
    return;
  for (auto &page : pages_)
    for (auto &byte : page)
      byte = uint8_t(state.Value());
}

void BsMemory::Reset() {
  for (auto &block : blocks_) {
    block.erasing = false;
    block.status = 0x80;
  }
  queued_ = 0;
  readyMode_ = 3;
  mode_ = Mode::Array;
  status_ = 0x80;
  global_ = 0x86;
  waitingErase_ = -1;
  clock_ = 0;
}

void BsMemory::SetWritable(bool writable) {
  writable_ = writable && !readOnly_;
}

void BsMemory::SetPal(bool pal) {
  masterFrequency_ = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
}

size_t BsMemory::Mirror(uint32_t address) const {
  size_t size = image_.size(), base = 0;
  while (address >= size) {
    const auto bit = std::bit_floor(address);
    address -= bit;
    if (size > bit) {
      base += bit;
      size -= bit;
    }
  }
  return base + address;
}

unsigned BsMemory::BlockIndex(uint32_t address) const {
  return (address >> 16) & (blockCount_ - 1);
}

uint8_t BsMemory::Read(uint32_t address, uint8_t openBus) const {
  if (image_.empty())
    return openBus;
  address = uint32_t(Mirror(address));
  if (readOnly_)
    return image_[address];
  switch (mode_) {
  case Mode::Identifier:
    if (address == 0)
      return uint8_t(vendor_);
    if (address == 1)
      return uint8_t(device_);
    return (address & 7) == 2 ? 0x63 : 0x20;
  case Mode::Page:
    return pages_[global_ & 1][address & 255];
  case Mode::Status:
    return status_;
  case Mode::ExtendedStatus:
    if (uint16_t(address) == 2)
      return blocks_[BlockIndex(address)].status;
    if (uint16_t(address) == 4)
      return global_;
    return 0;
  case Mode::Array:
    // The flash block identity has four bits even on larger allocations.
    return image_[((BlockIndex(address) & 15) << 16) | (address & 0xffff)];
  }
  return openBus;
}

void BsMemory::Fail() {
  status_ |= 0x30;
  global_ |= 0x20;
}

void BsMemory::Program(uint32_t address, uint8_t value) {
  auto &block = blocks_[BlockIndex(address)];
  if (!writable_ && !(block.status & 0x40)) {
    block.status |= 0x20;
    Fail();
    return;
  }
  image_[((BlockIndex(address) & 15) << 16) | (address & 0xffff)] &= value;
}

void BsMemory::BeginErase(unsigned index) {
  blocks_[index].erasing = true;
  blocks_[index].status &= ~uint8_t(0x80);
  status_ &= ~uint8_t(0x80);
  global_ &= ~uint8_t(0x80);
}

void BsMemory::FinishErase(unsigned index) {
  auto &block = blocks_[index];
  block.erasing = false;
  if (!writable_ && !(block.status & 0x40))
    return;
  const size_t start = (index & 15) << 16;
  std::fill_n(image_.begin() + start, 0x10000, 0xff);
  ++block.erasures;
  block.locked = false;
  block.status |= 0x40;
}

void BsMemory::RunClock() {
  if (waitingErase_ >= 0) {
    FinishErase(unsigned(waitingErase_));
    waitingErase_ = -1;
  }
  for (unsigned index = 0; index < blockCount_; ++index) {
    if (blocks_[index].erasing) {
      waitingErase_ = int(index);
      clock_ += int64_t(masterFrequency_) * 300'000;
      return;
    }
    blocks_[index].status |= 0x80;
  }
  status_ |= 0x80;
  global_ |= 0x80;
  clock_ += int64_t(masterFrequency_) * 10'000;
}

void BsMemory::Advance(uint32_t masterClocks, bool synchronizeProcessor) {
  if (image_.empty())
    return;
  clock_ -= int64_t(masterClocks) * 1'000'000;
  if (!synchronizeProcessor)
    return;
  if (readOnly_) {
    if (clock_ < 0) {
      const int64_t period = int64_t(masterFrequency_) * 1'000'000;
      clock_ += ((-clock_ + period - 1) / period) * period;
    }
    return;
  }
  while (clock_ < 0)
    RunClock();
}

void BsMemory::Write(uint32_t address, uint8_t value) {
  if (image_.empty() || readOnly_)
    return;
  if (queued_ < queue_.size())
    queue_[queued_++] = {uint32_t(Mirror(address)), value};
  Execute();
}

void BsMemory::Execute() {
  const uint8_t command = queue_[0].value;
  unsigned required = 1;
  switch (command) {
  case 0x0c:
  case 0xfb:
    required = 3;
    break;
  case 0xe0:
    required = 4;
    break;
  case 0x10:
  case 0x20:
  case 0x38:
  case 0x40:
  case 0x74:
  case 0x77:
  case 0x96:
  case 0x97:
  case 0x99:
  case 0xa7:
  case 0xb0:
    required = 2;
    break;
  }
  if (queued_ < required)
    return;
  auto &page = pages_[global_ & 1];
  const auto word = [&]() {
    return uint16_t((queue_[1].address & 1)
                        ? uint16_t(queue_[2].value) | (uint16_t(queue_[1].value) << 8)
                        : uint16_t(queue_[1].value) | (uint16_t(queue_[2].value) << 8));
  };
  switch (command) {
  case 0x0c:
    for (uint32_t index = 0, count = uint32_t(word()) + 1; index < count; ++index) {
      const uint32_t address = queue_[2].address + index;
      Program(address, page[address & 255]);
    }
    global_ ^= 1;
    mode_ = Mode::Status;
    break;
  case 0x10:
  case 0x40:
    Program(queue_[1].address, queue_[1].value);
    mode_ = Mode::Status;
    break;
  case 0x20:
  case 0x38:
  case 0x77:
  case 0x97:
  case 0x99:
  case 0xa7:
  case 0xb0:
    if (queue_[1].value != 0xd0) {
      Fail();
      break;
    }
    if (command == 0x20) {
      BeginErase(BlockIndex(queue_[1].address));
      mode_ = Mode::Status;
    } else if (command == 0x38) {
      page[0] = 'M';
      page[2] = 'P';
      page[4] = 4;
      page[6] = uint8_t(0x10 | (std::bit_width(image_.size() >> 10) - 1));
      for (unsigned byte = 0; byte < 6; ++byte)
        page[8 + byte * 2] = uint8_t(serial_ >> ((5 - byte) * 8));
      global_ ^= 1;
    } else if (command == 0x77) {
      auto &block = blocks_[BlockIndex(queue_[1].address)];
      if (!writable_) {
        block.status |= 0x20;
        Fail();
      } else {
        block.locked = true;
        block.status &= ~uint8_t(0x40);
      }
    } else if (command == 0x97) {
      for (unsigned index = 0; index < blockCount_; ++index)
        blocks_[index].status =
            uint8_t((blocks_[index].status & ~0x40) | (blocks_[index].locked ? 0 : 0x40));
    } else if (command == 0x99) {
      page[6] = 6;
      page[7] = 0;
      for (unsigned index = 0; index < blockCount_; ++index) {
        const unsigned address = (index & 3) * 8 + ((index >> 2) & 3) * 64 +
                                 ((index >> 4) & 1) * 32 + ((index >> 5) & 1) * 4;
        const uint32_t value = 0x80000000u | blocks_[index].erasures;
        for (unsigned byte = 0; byte < 4; ++byte)
          page[(address + byte) & 255] = uint8_t(value >> (byte * 8));
      }
      global_ ^= 1;
    } else if (command == 0xa7) {
      for (unsigned index = 0; index < blockCount_; ++index)
        BeginErase(index);
      mode_ = Mode::Status;
    } else {
      mode_ = Mode::Status;
    }
    break;
  case 0x50:
    for (unsigned index = 0; index < blockCount_; ++index)
      blocks_[index].status &= ~uint8_t(0x24);
    status_ &= ~uint8_t(0x38);
    global_ &= ~uint8_t(0x20);
    break;
  case 0x70:
    mode_ = Mode::Status;
    break;
  case 0x71:
    mode_ = Mode::ExtendedStatus;
    break;
  case 0x72:
    global_ ^= 1;
    break;
  case 0x74:
    page[queue_[1].address & 255] = queue_[1].value;
    break;
  case 0x75:
    mode_ = Mode::Page;
    break;
  case 0x80:
  case 0xf0:
    global_ |= 0x10;
    break;
  case 0x90:
    mode_ = Mode::Identifier;
    break;
  case 0x96:
    if (queue_[1].value >= 1 && queue_[1].value <= 4)
      readyMode_ = queue_[1].value - 1;
    break;
  case 0xe0: {
    page[queue_[3].address & 255] = queue_[3].value;
    uint16_t remaining = uint16_t(queue_[1].value | (uint16_t(queue_[2].value) << 8));
    if (remaining) {
      --remaining;
      queue_[1].value = uint8_t(remaining);
      queue_[2].value = uint8_t(remaining >> 8);
      queued_ = 3;
      return;
    }
    break;
  }
  case 0xfb: {
    const uint16_t value = word();
    Program(queue_[2].address, uint8_t(value));
    Program(queue_[2].address ^ 1, uint8_t(value >> 8));
    mode_ = Mode::Status;
    break;
  }
  case 0xff:
    mode_ = Mode::Array;
    break;
  }
  queued_ = 0;
}

std::vector<uint8_t> BsMemory::Metadata() const {
  if (readOnly_ || image_.empty())
    return {};
  std::vector<uint8_t> bytes{'B',
                             'S',
                             'M',
                             '1',
                             uint8_t(std::bit_width(image_.size()) - 1),
                             uint8_t(blockCount_),
                             uint8_t(vendor_),
                             uint8_t(vendor_ >> 8),
                             uint8_t(device_),
                             uint8_t(device_ >> 8)};
  for (unsigned byte = 0; byte < 6; ++byte)
    bytes.push_back(uint8_t(serial_ >> (byte * 8)));
  for (unsigned index = 0; index < blockCount_; ++index) {
    for (unsigned byte = 0; byte < 4; ++byte)
      bytes.push_back(uint8_t(blocks_[index].erasures >> (byte * 8)));
    bytes.push_back(uint8_t(blocks_[index].locked));
  }
  return bytes;
}

bool BsMemory::LoadMetadata(std::span<const uint8_t> bytes) {
  if (readOnly_ || image_.empty() || bytes.size() != 16 + blockCount_ * 5 || bytes[0] != 'B' ||
      bytes[1] != 'S' || bytes[2] != 'M' || bytes[3] != '1' ||
      bytes[4] != std::bit_width(image_.size()) - 1 || bytes[5] != blockCount_)
    return false;
  for (unsigned index = 0; index < blockCount_; ++index)
    if (bytes[20 + index * 5] > 1)
      return false;
  vendor_ = uint16_t(bytes[6] | (uint16_t(bytes[7]) << 8));
  device_ = uint16_t(bytes[8] | (uint16_t(bytes[9]) << 8));
  serial_ = 0;
  for (unsigned byte = 0; byte < 6; ++byte)
    serial_ |= uint64_t(bytes[10 + byte]) << (byte * 8);
  for (unsigned index = 0; index < blockCount_; ++index) {
    blocks_[index].erasures = 0;
    for (unsigned byte = 0; byte < 4; ++byte)
      blocks_[index].erasures |= uint32_t(bytes[16 + index * 5 + byte]) << (byte * 8);
    blocks_[index].locked = bytes[20 + index * 5] != 0;
  }
  return true;
}

} // namespace snes::core
