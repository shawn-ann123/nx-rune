#pragma once

#include <filesystem>

#include "json/ReJson.hpp"

inline const std::filesystem::path DETECTOR_CONFIG_DIR =
    std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "config";
inline ReJson J_DETECT((DETECTOR_CONFIG_DIR / "detect.json").string());
