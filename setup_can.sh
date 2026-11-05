#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:?用法: sudo ./setup_can.sh <can接口> [波特率]}"
BITRATE="${2:-1000000}"

ip link set "${IFACE}" down 2>/dev/null || true
ip link set "${IFACE}" type can bitrate "${BITRATE}" restart-ms 100
ip link set "${IFACE}" txqueuelen 1000
ip link set "${IFACE}" up
ip -details link show "${IFACE}"
