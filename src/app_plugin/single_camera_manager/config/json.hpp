#pragma once

#include <filesystem>

#include "json/ReJson.hpp"

inline const std::filesystem::path CAMERA_CONFIG_DIR =
    std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "config";
inline ReJson J_CAMERA((CAMERA_CONFIG_DIR / "camera.json").string());
