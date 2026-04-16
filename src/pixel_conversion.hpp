#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::vector<uint8_t> ConvertPixelBuffer(
    const uint8_t* src,
    size_t sourceSize,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t srcPixelFormat,
    const std::string& desiredPixelFormat);
