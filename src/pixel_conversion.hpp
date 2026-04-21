#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

std::vector<uint8_t> ConvertPixelBuffer(
    std::span<const uint8_t> src,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t srcPixelFormat,
    std::string_view desiredPixelFormat);
