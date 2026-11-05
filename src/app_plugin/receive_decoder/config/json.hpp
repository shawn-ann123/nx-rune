#pragma once

#include <filesystem>

#include "json/ReJson.hpp"

inline const std::filesystem::path RECEIVE_DECODER_CONFIG_DIR =
    std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "config";
inline ReJson J_RECEIVE_DECODER(
    (RECEIVE_DECODER_CONFIG_DIR / "can.json").string());
