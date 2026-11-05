#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -x "${ROOT}/output/app" ]]; then
  echo "尚未构建，请先运行 ./build.sh" >&2
  exit 1
fi
if [[ -n "${RP26_DEPS_ROOT:-}" ]]; then
  case "$(uname -m)" in
    aarch64|arm64) MULTIARCH="aarch64-linux-gnu" ;;
    x86_64|amd64) MULTIARCH="x86_64-linux-gnu" ;;
    *) MULTIARCH="$(uname -m)-linux-gnu" ;;
  esac
  export LD_LIBRARY_PATH="${RP26_DEPS_ROOT}/usr/lib/${MULTIARCH}:${RP26_DEPS_ROOT}/usr/lib:${LD_LIBRARY_PATH:-}"
fi
export RP_DEBUG_DISPLAY_BACKEND="${RP_DEBUG_DISPLAY_BACKEND:-browser_mjpeg}"
export RP_DEBUG_MJPEG_PORT="${RP_DEBUG_MJPEG_PORT:-8088}"
export RP_DEBUG_MJPEG_QUALITY="${RP_DEBUG_MJPEG_QUALITY:-75}"
export RP_DEBUG_MJPEG_FPS="${RP_DEBUG_MJPEG_FPS:-30}"
export RP_DEBUG_MJPEG_SCALE_PERCENT="${RP_DEBUG_MJPEG_SCALE_PERCENT:-50}"
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 1000000
exec "${ROOT}/output/app"
