#pragma once

#include <filesystem>

#include "json/ReJson.hpp"

inline const std::filesystem::path TRACKER_CONFIG_DIR =
    std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "config";
inline ReJson J_TRACK((TRACKER_CONFIG_DIR / "track.json").string());
