#pragma once

#include <napi.h>
#include "platform_capture.hpp"

Napi::Value SerializeSharedHandleLegacy(Napi::Env env, const std::optional<SharedHandleInfo>& shared);
Napi::Value SerializeSharedTextureInfo(Napi::Env env, const std::optional<SharedHandleInfo>& shared);
