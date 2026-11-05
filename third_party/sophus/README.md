# Vendored Sophus headers

This directory contains the header-only Sophus dependency used by `transform_tools` when a system `SophusConfig.cmake` is unavailable. Sophus is distributed under the MIT license; upstream project: https://github.com/strasdat/Sophus

The fallback is intentionally local to this project so the Jetson can build without root access. If a system Sophus package is installed, CMake uses it first.
