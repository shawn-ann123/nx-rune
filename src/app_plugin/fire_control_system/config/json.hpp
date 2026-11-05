#pragma once

#include <filesystem>

#include "json/ReJson.hpp"

inline const std::filesystem::path FIRE_CONTROL_CONFIG_DIR =
    std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "config";
inline ReJson J_FIRE_CONTROL(
    (FIRE_CONTROL_CONFIG_DIR / "fire_control_system.json").string());
