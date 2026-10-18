#include "snes/core/gameboy/Video.hpp"

#include <algorithm>

namespace snes::core::gameboy {

void Video::Reset(bool secondModel) {
  vram_.fill(0);
  oam_.fill(0);
  registers_.fill(0);
  registers_[8] = registers_[9] = 0xff;
  fifo_.fill(0);
  objects_.fill({});
  dot_ = transferClocks_ = hblankDot_ = 0;
  line_ = ly_ = mode_ = blocked_ = 0;
  irqMode_ = 0xff;
  comparisonLine_ = -1;
  fifoHead_ = fifoSize_ = fetchPhase_ = fetchX_ = tile_ = low_ = high_ = fine_ = x_ = 0;
  scrollFine_ = 0;
  scrollDiscard_ = 0;
  scrollLatched_ = false;
  windowFine_ = 0;
  leftWindowScan_ = leftWindowExtra_ = false;
  objectCount_ = objectStall_ = fetchingObject_ = windowLine_ = windowStall_ = 0;
  lastObjectX_ = -1;
  warm_ = true;
  coincidence_ = irqLine_ = stopped_ = false;
  windowReached_ = windowActive_ = windowUsed_ = lateWindow_ = false;
  secondModel_ = secondModel;
}

uint8_t Video::Read(uint16_t address, bool dma) const {
  if (address >= 0x8000 && address < 0xa000)
    return !dma && (blocked_ & 4) ? 0xff : vram_[address & 0x1fff];
  if (address >= 0xfe00 && address < 0xff00) {
    if (!dma && (blocked_ & 3))
      return 0xff;
    return address < 0xfea0 ? oam_[address - 0xfe00] : 0;
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
  if (address >= 0xfe00 && address < 0xfea0) {
    if (dma || !(blocked_ & 2))
      oam_[address - 0xfe00] = value;
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
      comparisonLine_ = -1;
      coincidence_ = irqLine_ = false;
      warm_ = true;
      fifoHead_ = fifoSize_ = objectCount_ = objectStall_ = 0;
      windowLine_ = 0;
      windowReached_ = windowActive_ = windowUsed_ = lateWindow_ = false;
    }
    if ((previous & 2) && !(value & 2))
      objectStall_ = 0;
  }
  if (reg == 5)
    CompareLine(comparisonLine_);
  return UpdateInterrupt();
}

void Video::CompareLine(int line) {
  comparisonLine_ = line;
  coincidence_ = line >= 0 && line == registers_[5];
}

uint8_t Video::UpdateInterrupt() {
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
  active &= (registers_[0] & 0x80) != 0;
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
      if (dot_ >= 4 && dot_ < 84 && (dot_ & 1) == 0) {
        const unsigned index = (dot_ - 4) / 2;
        const unsigned row = uint8_t(line_ + 16 - oam_[index * 4]);
        const unsigned height = registers_[0] & 4 ? 16 : 8;
        if (row < height && objectCount_ < objects_.size()) {
          auto &object = objects_[objectCount_++];
          object = {};
          object.index = uint8_t(index);
          object.y = oam_[index * 4];
          object.x = oam_[index * 4 + 1];
        }
      }
      if (dot_ == 80)
        blocked_ = 5;
      if (dot_ == 84)
        BeginTransfer();
    }
    if (mode_ == 3 && dot_ != (warm_ ? 80 : 84))
      Transfer();
    if (hblankDot_ && dot_ == hblankDot_ + 1)
      irqMode_ = 0;
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
  fine_ = scrollDiscard_ = 0;
  scrollFine_ = registers_[3] & 7;
  scrollLatched_ = false;
  windowFine_ = 0;
  leftWindowScan_ = leftWindowExtra_ = false;
  x_ = objectStall_ = windowStall_ = 0;
  lastObjectX_ = -1;
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
  const auto push = [&] {
    for (unsigned bit = 0; bit < 8; ++bit) {
      const auto color = uint8_t(((low_ >> (7 - bit)) & 1) | (((high_ >> (7 - bit)) & 1) << 1));
      fifo_[(fifoHead_ + fifoSize_) & 15] = color;
      ++fifoSize_;
    }
    ++fetchX_;
    fetchPhase_ = 0;
  };
  if (fetchPhase_ == 8) {
    if (fifoSize_ <= 8)
      push();
    return;
  }
  ++fetchPhase_;
  if (fetchPhase_ == 2) {
    const unsigned y = windowActive_ ? windowLine_ : uint8_t(line_ + registers_[2]);
    const unsigned x = windowActive_ ? fetchX_
                                     : unsigned((int(registers_[3]) + int(fetchX_) * 8 -
                                                 (fetchX_ ? int(scrollDiscard_) : 0)) >>
                                                3);
    const unsigned map = registers_[0] & (windowActive_ ? 0x40 : 8) ? 0x1c00 : 0x1800;
    tile_ = vram_[map + (y / 8) * 32 + (x & 31)];
  }
  if (fetchPhase_ == 4)
    low_ = vram_[TileAddress()];
  if (fetchPhase_ == 6)
    high_ = vram_[TileAddress() + 1];
  if (fetchPhase_ == 8 && fifoSize_ <= 8)
    push();
}

bool Video::StartWindow(int fine) {
  if (windowActive_ || !windowReached_ || (registers_[0] & 0x21) != 0x21 || registers_[11] >= 166 ||
      x_ < std::max(0, int(registers_[11]) - 7))
    return false;
  windowActive_ = windowUsed_ = true;
  const unsigned clip =
      fine >= 0 ? unsigned(8 + fine - (fine == 7)) : unsigned(std::max(0, 7 - int(registers_[11])));
  fifoHead_ = fifoSize_ = 0;
  fetchX_ = uint8_t(clip / 8);
  fetchPhase_ = 1;
  FetchBackground();
  if (clip & 7) {
    // The hidden left edge is fetched before visible window output starts.
    for (unsigned phase = 0; phase < 6; ++phase)
      FetchBackground();
    fifoHead_ = uint8_t(clip & 7);
    fifoSize_ = uint8_t(fifoSize_ - (clip & 7));
    fetchPhase_ = 1;
  }
  windowStall_ = uint8_t(fine >= 0 ? 8 : 6 + (registers_[11] == 0 && leftWindowExtra_ ? 1 : 0));
  return true;
}

bool Video::StartObject() {
  if (!(registers_[0] & 2))
    return false;
  unsigned choice = objectCount_;
  for (unsigned i = 0; i < objectCount_; ++i) {
    const auto &object = objects_[i];
    if (object.fetched || object.x >= 168 || int(object.x) - 8 > x_)
      continue;
    if (choice == objectCount_ || object.x < objects_[choice].x ||
        (object.x == objects_[choice].x && object.index < objects_[choice].index))
      choice = i;
  }
  if (choice == objectCount_)
    return false;
  auto &object = objects_[choice];
  object.fetched = true;
  fetchingObject_ = uint8_t(choice);
  const unsigned phase =
      (windowActive_ ? object.x + 7 - registers_[11] : object.x + registers_[3]) & 7;
  const unsigned align = int(object.x) == lastObjectX_ ? 0 : phase < 5 ? 5 - phase : 0;
  lastObjectX_ = object.x;
  objectStall_ = uint8_t(6 + align);
  FetchObject();
  return true;
}

void Video::FetchObject() {
  auto &object = objects_[fetchingObject_];
  if (objectStall_ == 6) {
    object.flags = oam_[object.index * 4 + 3];
    unsigned tile = oam_[object.index * 4 + 2];
    const unsigned height = registers_[0] & 4 ? 16 : 8;
    unsigned row = uint8_t(line_ + 16 - object.y) & (height - 1);
    if (object.flags & 0x40)
      row = height - 1 - row;
    if (height == 16)
      tile &= 0xfe;
    object.address = uint16_t(tile * 16 + row * 2);
  }
  if (objectStall_ == 4)
    object.low = vram_[object.address];
  if (objectStall_ == 2)
    object.high = vram_[object.address + 1];
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
  if (objectStall_) {
    --objectStall_;
    FetchObject();
    if (objectStall_)
      return;
  }
  const bool leftWindow =
      !windowActive_ && windowReached_ && registers_[11] == 0 && (registers_[0] & 0x21) == 0x21;
  if (transferClocks_ == 5 && leftWindow && (registers_[3] & 7)) {
    leftWindowScan_ = true;
    scrollLatched_ = true;
    fine_ = scrollDiscard_ = 0;
  }
  if (transferClocks_ == 6 && leftWindow)
    leftWindowExtra_ = leftWindowScan_ || (registers_[3] & 7);
  if (!scrollLatched_ && transferClocks_ >= 5) {
    if ((fine_ & 7) == (registers_[3] & 7)) {
      scrollLatched_ = true;
      scrollFine_ = registers_[3] & 7;
    } else {
      ++fine_;
    }
    scrollDiscard_ = fine_;
  }
  if (transferClocks_ > 3)
    FetchBackground();
  if (windowStall_) {
    if (--windowStall_)
      return;
  }
  if (leftWindowScan_ && !windowActive_) {
    if (transferClocks_ >= 12) {
      if ((windowFine_ & 7) == (registers_[3] & 7))
        StartWindow(int(windowFine_));
      else
        ++windowFine_;
    }
    return;
  }
  if (!scrollLatched_ || transferClocks_ <= 12 + scrollDiscard_ || fifoSize_ == 0)
    return;
  while (fine_ && fifoSize_) {
    --fine_;
    fifoHead_ = uint8_t((fifoHead_ + 1) & 15);
    --fifoSize_;
  }
  if (fifoSize_ == 0)
    return;
  if (StartWindow() || StartObject())
    return;
  const bool emptyWindowEdge =
      windowActive_ && lateWindow_ && registers_[11] == 166 && scrollFine_ == 1 && x_ == 159;
  const auto color = (registers_[0] & 1) && !emptyWindowEdge ? fifo_[fifoHead_] : 0;
  output_.Pixel(Shade(color));
  fifoHead_ = uint8_t((fifoHead_ + 1) & 15);
  --fifoSize_;
  if (++x_ == 160) {
    lateWindow_ = windowReached_ && registers_[11] == 166 && (registers_[0] & 0x21) == 0x21;
    mode_ = 0;
    blocked_ = 0;
    hblankDot_ = dot_;
  }
}

} // namespace snes::core::gameboy
