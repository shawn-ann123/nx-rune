#!/usr/bin/env bash
# 在载板上运行：校准系统时间（载板无 RTC 电池，重启会回到 1970）
#   1) 局域网时间源（笔记本上跑 ./serve_time.sh，返回纯 epoch，最可靠）
#   2) 代理端口取外网 Date 头（ssh 的 RemoteForward 10808）
#   3) 直连外网 Date 头
# 用法：
#   ./sync_time.sh
#   TIME_PROXY= ./sync_time.sh                                  # 禁用代理
#   TIME_URL=http://192.168.137.1:8899/ ./sync_time.sh
# 注意：外网这条路依赖 HTTP Date 头，代理/网关的错误页也会带 Date（本地时间却标 GMT），
# 所以局域网时间源优先；只有它不通时才用外网。
set -uo pipefail

TIME_URL="${TIME_URL-http://192.168.137.1:8899/}"
TIME_WEB="${TIME_WEB-http://www.baidu.com/}"
TIME_PROXY="${TIME_PROXY-socks5h://127.0.0.1:10808}"
SANITY_MIN_EPOCH=1600000000

fetch_epoch() {
    local url="$1" proxy="$2" out body date_hdr epoch
    [[ -n "${url}" ]] || return 1
    local args=(curl -fsS -m 6 -D -)
    [[ -n "${proxy}" ]] && args+=(--proxy "${proxy}")
    out="$("${args[@]}" "${url}" 2>/dev/null)" || return 1

    body="$(printf '%s' "${out}" | tail -n 1 | tr -d '[:space:]')"
    if [[ "${body}" =~ ^[0-9]{9,11}$ ]]; then
        epoch="${body}"
    else
        date_hdr="$(printf '%s' "${out}" | tr -d '\r' | sed -n 's/^[Dd]ate: //p' | head -n 1)"
        [[ -n "${date_hdr}" ]] || return 1
        epoch="$(date -u -d "${date_hdr}" +%s 2>/dev/null)" || return 1
    fi

    [[ "${epoch}" =~ ^[0-9]+$ ]] || return 1
    (( epoch >= SANITY_MIN_EPOCH )) || return 1
    printf '%s\n' "${epoch}"
}

try_source() {
    local label="$1" url="$2" proxy="$3" epoch
    epoch="$(fetch_epoch "${url}" "${proxy}")"
    [[ -n "${epoch}" ]] || return 1
    echo "时间来源: ${label} (${url})" >&2
    printf '%s\n' "${epoch}"
}

EPOCH=""
EPOCH="$(try_source "局域网时间源" "${TIME_URL}" "")" \
    || EPOCH="$(try_source "代理端口" "${TIME_WEB}" "${TIME_PROXY}")" \
    || EPOCH="$(try_source "直连外网" "${TIME_WEB}" "")" \
    || true

if [[ -z "${EPOCH}" ]]; then
    echo "取时间失败：${TIME_URL}、代理 ${TIME_PROXY:-无}、${TIME_WEB} 都不通" >&2
    echo "先在笔记本上跑 ./serve_time.sh；或确认 ssh 连接带着 RemoteForward 10808" >&2
    exit 1
fi

echo "epoch=${EPOCH} -> $(date -d "@${EPOCH}" '+%F %T %Z')"
sudo date -s "@${EPOCH}"
sudo hwclock -w 2>/dev/null || true
date '+载板当前时间: %F %T %Z'
