#include "snes/core/gameboy/MotionEeprom.hpp"

#include <algorithm>
#include <cmath>

namespace snes::core::gameboy {
namespace {
uint16_t Sensor(double value) noexcept {
    if (!std::isfinite(value))
        value = 0;
    const double scaled = 33232.0 + 112.0 * value;
    if (!std::isfinite(scaled))
        return 0x81d0;
    double raw = std::fmod(std::trunc(scaled), 65536.0);
    if (raw < 0)
        raw += 65536.0;
    return uint16_t(raw);
}
} // namespace

void MotionEeprom::Reset() noexcept {
    x_ = y_ = 0x8000;
    command_ = 0;
    output_ = 0xffff;
    dataBits_ = 0;
    selected_ = clock_ = input_ = writeEnabled_ = false;
    dataOut_ = true;
}

void MotionEeprom::SetMotion(double x, double y) noexcept {
    motionX_ = std::isfinite(x) ? x : 0;
    motionY_ = std::isfinite(y) ? y : 0;
}

uint8_t MotionEeprom::Read(uint16_t address) const noexcept {
    switch ((address >> 4) & 15) {
    case 2:
        return uint8_t(x_);
    case 3:
        return uint8_t(x_ >> 8);
    case 4:
        return uint8_t(y_);
    case 5:
        return uint8_t(y_ >> 8);
    case 6:
        return 0;
    case 8:
        return uint8_t(unsigned(dataOut_) | (unsigned(input_) << 1) | (unsigned(clock_) << 6) |
                       (unsigned(selected_) << 7));
    default:
        return 0xff;
    }
}

void MotionEeprom::Write(uint16_t address, uint8_t value, std::span<uint8_t> memory) noexcept {
    switch ((address >> 4) & 15) {
    case 0:
        if (value == 0x55)
            x_ = y_ = 0x8000;
        break;
    case 1:
        if (value == 0xaa) {
            x_ = Sensor(motionX_);
            y_ = Sensor(motionY_);
        }
        break;
    case 8: {
        selected_ = value & 0x80;
        input_ = value & 2;
        const bool rising = !clock_ && (value & 0x40);
        clock_ = value & 0x40;
        if (selected_ && rising && memory.size() == 256)
            RisingEdge(memory);
        break;
    }
    }
}

void MotionEeprom::RisingEdge(std::span<uint8_t> memory) noexcept {
    dataOut_ = output_ & 0x8000;
    output_ = uint16_t((output_ << 1) | 1);
    if (dataBits_ != 0) {
        const unsigned bit = --dataBits_;
        dataOut_ = true;
        if (input_) {
            const unsigned start = command_ & 0x100 ? command_ & 127 : 0;
            const unsigned end = command_ & 0x100 ? start + 1 : 127;
            for (unsigned word = start; word < end; ++word)
                memory[word * 2 + bit / 8] |= uint8_t(1u << (bit & 7));
        }
        if (dataBits_ == 0) {
            command_ = 0;
            output_ = 0x3fff;
        }
        return;
    }
    command_ = uint16_t(((command_ << 1) | unsigned(input_)) & 0x7ff);
    if (command_ & 0x400)
        Command(memory);
}

void MotionEeprom::Command(std::span<uint8_t> memory) noexcept {
    const unsigned word = command_ & 127;
    const unsigned operation = (command_ >> 8) & 3;
    if (operation == 2) {
        output_ = uint16_t(memory[word * 2] | (unsigned(memory[word * 2 + 1]) << 8));
    } else if (operation == 1) {
        if (writeEnabled_)
            memory[word * 2] = memory[word * 2 + 1] = 0;
        dataBits_ = 16;
    } else if (operation == 3) {
        if (writeEnabled_) {
            memory[word * 2] = memory[word * 2 + 1] = 0xff;
            output_ = 0x3fff;
        }
    } else {
        switch ((command_ >> 6) & 3) {
        case 0:
            writeEnabled_ = false;
            break;
        case 1:
            if (writeEnabled_)
                std::fill(memory.begin(), memory.end(), 0);
            dataBits_ = 16;
            break;
        case 2:
            if (writeEnabled_) {
                std::fill(memory.begin(), memory.end(), 0xff);
                output_ = 0xff;
            }
            break;
        case 3:
            writeEnabled_ = true;
            break;
        }
    }
    if (dataBits_ == 0)
        command_ = 0;
}
} // namespace snes::core::gameboy
