#pragma once

#include <napi.h>
#include <vector>
#include "platform_capture.hpp"

Napi::Value SerializeSharedHandleLegacy(Napi::Env env, const std::optional<SharedHandleInfo>& shared);
Napi::Value SerializeSharedTextureInfo(Napi::Env env, const std::optional<SharedHandleInfo>& shared);
Napi::Value SerializePixelData(Napi::Env env, const std::optional<std::vector<uint8_t>>& pixels);
