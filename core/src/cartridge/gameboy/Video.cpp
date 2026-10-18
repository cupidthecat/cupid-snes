#include "snes/core/gameboy/Video.hpp"

#include <algorithm>

namespace snes::core::gameboy {

void Video::Reset(bool secondModel) {
  vram_.fill(0);
  oam_.Reset(secondModel);
  registers_.fill(0);
  registers_[8] = registers_[9] = 0xff;
  fifo_.fill(0);
  objects_.fill({});
  dot_ = transferClocks_ = hblankDot_ = 0;
  line_ = ly_ = mode_ = blocked_ = 0;
  irqMode_ = 0xff;
  comparisonLine_ = -1;
  fifoHead_ = fifoSize_ = fetchPhase_ = fetchX_ = tile_ = low_ = high_ = x_ = 0;
  position_ = -16;
  mapAddress_ = dataAddress_ = 0;
  scrollFine_ = 0;
  objectCount_ = objectStall_ = fetchingObject_ = windowLine_ = 0;
  objectPending_ = windowStarting_ = insertBackground_ = false;
  warm_ = true;
  coincidence_ = irqLine_ = stopped_ = false;
  windowReached_ = windowActive_ = windowUsed_ = lateWindow_ = false;
  windowWrite_ = windowChanged_ = false;
  secondModel_ = secondModel;
  sleeping_ = vramRelease_ = false;
  dmaSource_ = 0;
  dmaPosition_ = 0xa1;
  scanY_ = scanX_ = 0;
}

uint8_t Video::Read(uint16_t address, bool dma) {
  if (address >= 0x8000 && address < 0xa000) {
    if (!dma && (blocked_ & 4))
      return 0xff;
    // The upper address line retains its fetch value for the release clock.
    const auto mask = vramRelease_ && !(dataAddress_ & 0x1000) ? 0x0fff : 0x1fff;
    return vram_[address & mask];
  }
  if (address >= 0xfe00 && address < 0xff00) {
    return dma ? oam_.Read(uint8_t(address))
               : oam_.ReadCpu(uint8_t(address), ScannerRow(), blocked_ & 3);
  }
  if (address == 0xff41)
    return uint8_t(0x80 | registers_[1] | (coincidence_ ? 4 : 0) | mode_);
  if (address == 0xff44)
    return ly_;
  if (address >= 0xff40 && address <= 0xff4b && address != 0xff46)
    return registers_[address - 0xff40];
  return 0xff;
}

uint8_t Video::Write(uint16_t address, uint8_t value, bool dma) {
  if (address >= 0x8000 && address < 0xa000) {
    if (dma || !(blocked_ & 8))
      vram_[address & 0x1fff] = value;
    return 0;
  }
  if (address >= 0xfe00 && address < 0xff00) {
    if (dma)
      oam_.Write(uint8_t(address), value);
    else
      oam_.WriteCpu(uint8_t(address), value, ScannerRow(), (blocked_ & 2) != 0);
    return 0;
  }

  if (address < 0xff40 || address > 0xff4b || address == 0xff44 || address == 0xff46)
    return 0;
  const unsigned reg = address - 0xff40;
  const auto previous = registers_[reg];
  registers_[reg] = reg == 1 ? value & 0x78 : value;
  if (reg == 0) {
    if ((previous ^ value) & 0x80) {
      dot_ = transferClocks_ = hblankDot_ = 0;
      line_ = ly_ = mode_ = blocked_ = 0;
      irqMode_ = 0xff;
      warm_ = true;
      vramRelease_ = false;
      fifoHead_ = fifoSize_ = objectCount_ = objectStall_ = 0;
      windowLine_ = 0;
      windowReached_ = windowActive_ = windowUsed_ = lateWindow_ = false;
    }
    if ((previous & 2) && !(value & 2)) {
      objectStall_ = 0;
      objectPending_ = false;
    }
  }
  if (reg == 5)
    CompareLine(comparisonLine_);
  if (reg == 11)
    windowChanged_ = windowWrite_ && previous != value;
  return UpdateInterrupt();
}

uint8_t Video::ScannerRow() const noexcept {
  if (!(registers_[0] & 0x80) || warm_ || line_ >= 144 || dot_ >= 84)
    return 0xff;
  return dot_ < 6 ? 0 : uint8_t(((dot_ - 2) / 4) * 8);
}

void Video::AddressBus(uint16_t address, bool incremented) {
  if (!incremented && address >= 0xfe00 && address < 0xff00)
    oam_.AddressBus(ScannerRow());
}

void Video::BlockedOamAccess(uint16_t address, bool write) {
  if (address < 0xfe00 || address >= 0xff00 || !(blocked_ & 2))
    return;
  if (write)
    oam_.AddressBus(ScannerRow());
  else
    oam_.BlockedRead(ScannerRow());
}

uint8_t Video::ReadObject(uint8_t address) const noexcept {
  if (dmaPosition_ > 0 && dmaPosition_ < 0xa0)
    address = uint8_t((dmaPosition_ & 0xfe) | (address & 1));
  return oam_.Read(address);
}

uint8_t Video::ReadVram(uint16_t address) {
  SynchronizeDma();
  if (dmaPosition_ >= 1 && dmaPosition_ <= 160 && dmaSource_ >= 0x8000 && dmaSource_ < 0xa000) {
    const auto byte = uint8_t(dmaPosition_ - (sleeping_ ? 0 : 1));
    const auto source = uint16_t(dmaSource_ - (sleeping_ ? 0 : 1));
    // Both bus masters drive high address bits onto the same memory access.
    address |= source & 0x1fff;
    const auto value = vram_[address & 0x1fff];
    oam_.Write(byte, value);
    return value;
  }
  return vram_[address & 0x1fff];
}

void Video::SynchronizeDma() {
  if (dmaSynchronizer_)
    dmaSynchronizer_();
}

void Video::CompareLine(int line) {
  comparisonLine_ = line;
  coincidence_ = line >= 0 && line == registers_[5];
}

uint8_t Video::UpdateInterrupt() {
  if (!(registers_[0] & 0x80))
    return 0;
  const auto enabled = registers_[1];
  bool active = comparisonLine_ >= 0 && comparisonLine_ == registers_[5] && (enabled & 0x40);
  if (irqMode_ == 0)
    active |= enabled & 8;
  if (irqMode_ == 1)
    active |= enabled & 0x10;
  if (irqMode_ == 2)
    active |= enabled & 0x20;
  if (irqMode_ == 4)
    active |= enabled & 0x28;
  if (irqMode_ == 5)
    active |= enabled & 0x30;
  const bool rising = active && !irqLine_;
  irqLine_ = active;
  return rising ? 2 : 0;
}

uint8_t Video::Advance(unsigned clocks) {
  uint8_t interrupts = 0;
  if (!(registers_[0] & 0x80) || stopped_)
    return 0;
  while (clocks--)
    interrupts |= Tick();
  return interrupts;
}

uint8_t Video::Tick() {
  ++dot_;
  uint8_t interrupts = 0;
  if (line_ < 144) {
    if (warm_ && line_ == 0) {
      if (dot_ == 2)
        CompareLine(0);
      if (dot_ == 78)
        blocked_ = 2;
      if (dot_ == 80)
        BeginTransfer();
    } else {
      if (dot_ == 3) {
        ly_ = line_;
        CompareLine(line_ ? -1 : 0);
        mode_ = 0;
        irqMode_ = line_ ? 2 : 1;
        blocked_ = 1;
      }
      if (dot_ == 4) {
        CompareLine(line_);
        mode_ = irqMode_ = 2;
        blocked_ = 3;
      }
      if (dot_ >= 6 && dot_ <= 84 && (dot_ & 1) == 0) {
        const unsigned index = (dot_ - 6) / 2;
        if (dmaPosition_ == 0xa1 || sleeping_) {
          scanY_ = ReadObject(uint8_t(index * 4));
          scanX_ = ReadObject(uint8_t(index * 4 + 1));
        }
        const unsigned row = uint8_t(line_ + 16 - scanY_);
        const unsigned height = registers_[0] & 4 ? 16 : 8;
        if (!(dmaPosition_ != 0xa1 && sleeping_) && row < height &&
            objectCount_ < objects_.size()) {
          auto &object = objects_[objectCount_++];
          object = {};
          object.index = uint8_t(index);
          object.y = scanY_;
          object.x = scanX_;
        }
      }
      if (dot_ == 80)
        blocked_ = 5;
      if (dot_ == 84)
        BeginTransfer();
    }
    if (mode_ == 3 && dot_ != (warm_ ? 80 : 84))
      Transfer();
    if (hblankDot_ && dot_ == hblankDot_ + 1) {
      vramRelease_ = false;
      irqMode_ = 0;
    }
  } else if (line_ == 144) {
    if (dot_ == 2) {
      ly_ = 144;
      CompareLine(-1);
      irqMode_ = 4;
    }
    if (dot_ == 3)
      irqMode_ = 0;
    if (dot_ == 4)
      CompareLine(144);
    if (dot_ == 5) {
      mode_ = 1;
      irqMode_ = 5;
      interrupts |= 1;
    }
    if (dot_ == 6)
      irqMode_ = 1;
  } else {
    if (dot_ == 2) {
      ly_ = line_;
      CompareLine(-1);
    }
    if (line_ != 153 && dot_ == 4)
      CompareLine(line_);
    if (line_ == 153 && dot_ == 6) {
      ly_ = 0;
      CompareLine(153);
    }
    if (line_ == 153 && dot_ == 8)
      CompareLine(-1);
    if (line_ == 153 && dot_ == 12)
      CompareLine(0);
  }
  interrupts |= UpdateInterrupt();
  if (dot_ == (warm_ ? 450 : 456))
    EndLine();
  return interrupts;
}

void Video::EndLine() {
  if (line_ < 144) {
    output_.BeginLine();
    if (windowUsed_ || lateWindow_)
      ++windowLine_;
  }
  dot_ = 0;
  warm_ = false;
  objectCount_ = 0;
  hblankDot_ = 0;
  windowUsed_ = false;
  if (++line_ >= 144 && line_ < 154)
    coincidence_ = false;
  if (line_ == 154) {
    line_ = 0;
    windowLine_ = 0;
    windowReached_ = false;
    output_.BeginFrame();
  }
}

void Video::BeginTransfer() {
  mode_ = irqMode_ = 3;
  blocked_ = 15;
  transferClocks_ = 0;
  fifoHead_ = fifoSize_ = fetchPhase_ = fetchX_ = 0;
  // The initial queue covers hidden pixels before the first tile arrives.
  fifo_.fill(0);
  fifoSize_ = 8;
  position_ = -16;
  scrollFine_ = registers_[3] & 7;
  x_ = objectStall_ = 0;
  objectPending_ = windowStarting_ = insertBackground_ = false;
  windowReached_ |= line_ == registers_[10];
  windowActive_ = lateWindow_ && (registers_[0] & 0x21) == 0x21;
  windowUsed_ = windowActive_;
  if (windowActive_)
    fetchX_ = 1;
  for (unsigned i = 0; i < objectCount_; ++i)
    objects_[i].fetched = false;
}

uint16_t Video::TileAddress() const {
  const unsigned y = windowActive_ ? windowLine_ : uint8_t(line_ + registers_[2]);
  const int tile = tile_ < 128 ? int(tile_) : int(tile_) - 256;
  const int base = registers_[0] & 0x10 ? int(tile_) * 16 : 0x1000 + tile * 16;
  return uint16_t(base + (y & 7) * 2);
}

void Video::FetchBackground() {
  switch (fetchPhase_) {
  case MapAddress: {
    const unsigned y = windowActive_ ? windowLine_ : uint8_t(line_ + registers_[2]);
    const unsigned column = windowActive_    ? fetchX_
                            : position_ < -8 ? registers_[3] / 8
                                             : unsigned(position_ + 8 + registers_[3]) / 8;
    const unsigned map = registers_[0] & (windowActive_ ? 0x40 : 8) ? 0x1c00 : 0x1800;
    mapAddress_ = uint16_t(map + (y / 8) * 32 + (column & 31));
    break;
  }
  case MapRead:
    tile_ = ReadVram(mapAddress_);
    break;
  case LowerAddress:
    dataAddress_ = TileAddress();
    break;
  case LowerRead:
    low_ = ReadVram(dataAddress_);
    break;
  case UpperAddress:
    dataAddress_ = uint16_t(TileAddress() + 1);
    break;
  case UpperRead:
    high_ = ReadVram(dataAddress_);
    if (windowActive_)
      ++fetchX_;
    break;
  }
  if (fetchPhase_ < UpperRead) {
    ++fetchPhase_;
    return;
  }
  fetchPhase_ = QueueWrite;
  if (fifoSize_)
    return;
  for (unsigned bit = 0; bit < 8; ++bit)
    fifo_[(fifoHead_ + bit) & 7] =
        uint8_t(((low_ >> (7 - bit)) & 1) | (((high_ >> (7 - bit)) & 1) << 1));
  fifoSize_ = 8;
  fetchPhase_ = MapAddress;
}

bool Video::StartWindow() {
  if (windowChanged_ || windowActive_ || !windowReached_ || (registers_[0] & 0x21) != 0x21 ||
      registers_[11] >= 166)
    return false;
  const bool left = registers_[11] == 0;
  const bool match = left ? position_ == -7 || (position_ >= -16 && position_ <= -8 &&
                                                (position_ != -16 || (registers_[3] & 7)))
                          : position_ == int(registers_[11]) - 7;
  if (!match)
    return false;
  windowActive_ = windowUsed_ = windowStarting_ = true;
  fifoHead_ = fifoSize_ = fetchPhase_ = fetchX_ = 0;
  return left && (registers_[3] & 7);
}

bool Video::StartObject() {
  if (!(registers_[0] & 2))
    return false;
  const int trigger = std::max(0, int(position_) + 8);
  unsigned choice = objectCount_;
  for (unsigned i = 0; i < objectCount_; ++i) {
    const auto &object = objects_[i];
    if (object.fetched || object.x >= 168 || object.x != trigger)
      continue;
    if (choice == objectCount_ || object.index < objects_[choice].index)
      choice = i;
  }
  if (choice == objectCount_)
    return false;
  objects_[choice].fetched = true;
  fetchingObject_ = uint8_t(choice);
  objectPending_ = true;
  return true;
}

bool Video::AdvanceObject() {
  if (objectPending_) {
    // The background completes its bitplanes before yielding the fetch bus.
    const bool ready = fetchPhase_ >= UpperRead && fifoSize_ != 0;
    FetchBackground();
    if (ready) {
      objectPending_ = false;
      objectStall_ = 6;
    }
    return true;
  }
  if (objectStall_ == 6)
    FetchBackground();
  --objectStall_;
  FetchObject();
  return objectStall_ != 0;
}

void Video::FetchObject() {
  auto &object = objects_[fetchingObject_];
  if (objectStall_ == 5) {
    SynchronizeDma();
    scanY_ = ReadObject(uint8_t(object.index * 4 + 2));
    scanX_ = ReadObject(uint8_t(object.index * 4 + 3));
    object.flags = scanX_;
    object.address = uint16_t(scanY_ * 16);
  }
  if (objectStall_ == 3 || objectStall_ == 1) {
    const unsigned height = registers_[0] & 4 ? 16 : 8;
    unsigned row = uint8_t(line_ + 16 - object.y) & (height - 1);
    if (object.flags & 0x40)
      row = height - 1 - row;
    const unsigned base = height == 16 ? object.address & ~16u : object.address;
    const auto value = ReadVram(uint16_t(base + row * 2 + (objectStall_ == 1 ? 1 : 0)));
    if (objectStall_ == 3)
      object.low = value;
    else
      object.high = value;
  }
  if (objectStall_ == 0) {
    for (unsigned x = 0; x < 8; ++x) {
      const auto bit = object.flags & 0x20 ? x : 7 - x;
      object.pixels[x] = uint8_t(((object.low >> bit) & 1) | (((object.high >> bit) & 1) << 1));
    }
  }
}

uint8_t Video::Shade(uint8_t background) const {
  const auto bg = uint8_t((registers_[7] >> (background * 2)) & 3);
  if (!(registers_[0] & 2))
    return bg;
  const Object *selected = nullptr;
  uint8_t color = 0;
  for (unsigned i = 0; i < objectCount_; ++i) {
    const auto &object = objects_[i];
    const int column = int(x_) - (int(object.x) - 8);
    if (!object.fetched || column < 0 || column >= 8 || !object.pixels[size_t(column)])
      continue;
    if (!selected || object.x < selected->x ||
        (object.x == selected->x && object.index < selected->index)) {
      selected = &object;
      color = object.pixels[size_t(column)];
    }
  }
  if (!selected || ((selected->flags & 0x80) && background != 0))
    return bg;
  const auto palette = registers_[selected->flags & 0x10 ? 9 : 8];
  return uint8_t((palette >> (color * 2)) & 3);
}

void Video::Transfer() {
  ++transferClocks_;
  if (transferClocks_ <= 4)
    return;
  const bool resumingObject = objectPending_ || objectStall_;
  if (resumingObject) {
    if (AdvanceObject())
      return;
  }
  if (!resumingObject) {
    if (StartWindow())
      return;
    if (windowActive_ && !windowStarting_ && int(registers_[11]) == position_ + 7 &&
        fetchPhase_ == MapAddress && fifoSize_ == 8)
      insertBackground_ = true;
  }
  if (StartObject()) {
    AdvanceObject();
    return;
  }
  if (fifoSize_) {
    auto color = insertBackground_ ? uint8_t(0) : fifo_[fifoHead_];
    if (insertBackground_)
      insertBackground_ = false;
    else {
      fifoHead_ = uint8_t((fifoHead_ + 1) & 7);
      --fifoSize_;
    }
    bool advance = true;
    if (position_ < -8) {
      const auto fine = uint8_t(position_) & 7;
      if (fine == (registers_[3] & 7) ||
          (windowStarting_ && fine == 6 && (registers_[3] & 7) == 7)) {
        position_ = -8;
        scrollFine_ = registers_[3] & 7;
      } else if (position_ == -9) {
        position_ = -16;
        advance = false;
      }
    }
    if (position_ >= 0) {
      const bool emptyWindowEdge =
          windowActive_ && lateWindow_ && registers_[11] == 166 && scrollFine_ == 1 && x_ == 159;
      if (!(registers_[0] & 1) || emptyWindowEdge)
        color = 0;
      output_.Pixel(Shade(color));
      ++x_;
    }
    if (advance) {
      ++position_;
      windowStarting_ = false;
    }
  }
  FetchBackground();
  if (x_ == 160) {
    vramRelease_ = true;
    lateWindow_ = windowReached_ && registers_[11] == 166 && (registers_[0] & 0x21) == 0x21;
    mode_ = 0;
    blocked_ = 0;
    hblankDot_ = dot_;
  }
}

} // namespace snes::core::gameboy
