#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${RP26_BUILD_DIR:-${ROOT}/build}"
CMAKE_PLATFORM_ARGS=()

# Jetson images commonly install CUDA here without adding nvcc to PATH.
if [[ -x /usr/local/cuda/bin/nvcc ]]; then
  export PATH="/usr/local/cuda/bin:${PATH}"
  CMAKE_PLATFORM_ARGS+=("-DCUDAToolkit_ROOT=/usr/local/cuda")
fi

cmake -S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
  "${CMAKE_PLATFORM_ARGS[@]}" "$@"
cmake --build "${BUILD_DIR}" -j"$(nproc)"
echo "构建完成：${ROOT}/output/app"
