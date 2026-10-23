#pragma once
#include "snes/core/Platform.hpp"
#include <vector>

namespace snes::frontend {
std::vector<uint8_t> InspectionBitmap(const core::VideoFrame& picture);
}
