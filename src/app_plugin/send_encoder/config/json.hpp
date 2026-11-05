#pragma once

#include <filesystem>
#include "json/ReJson.hpp"

inline const std::filesystem::path SEND_ENCODER_CONFIG_DIR =
    std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "config";
inline ReJson J_SEND_ENCODER((SEND_ENCODER_CONFIG_DIR / "can.json").string());
