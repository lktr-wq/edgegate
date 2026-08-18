#!/usr/bin/env bash
# AI-CODE-BEGIN: S11-RESOURCE-SAMPLER
# 周期性读取 /proc 和 ps，记录 EdgeGate 的 CPU、内存、线程及 fd 数量。
# 用法：sample_process_resources.sh PID OUTPUT.csv [INTERVAL_SECONDS]
set -euo pipefail

pid="${1:?PID is required}"
output_path="${2:?output CSV path is required}"
interval_seconds="${3:-1}"

mkdir -p "$(dirname "${output_path}")"
printf '%s\n' 'timestamp_epoch,cpu_percent,rss_kib,threads,fd_count' >"${output_path}"

while kill -0 "${pid}" 2>/dev/null; do
    timestamp="$(date +%s)"
    cpu="$(ps -p "${pid}" -o %cpu= 2>/dev/null | tr -d ' ' || true)"
    rss="$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status" 2>/dev/null || true)"
    threads="$(awk '/^Threads:/ {print $2}' "/proc/${pid}/status" 2>/dev/null || true)"
    fd_count="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
    if [[ -n "${rss}" && -n "${threads}" ]]; then
        printf '%s,%s,%s,%s,%s\n' \
            "${timestamp}" "${cpu:-0}" "${rss}" "${threads}" "${fd_count}" \
            >>"${output_path}"
    fi
    sleep "${interval_seconds}"
done
# AI-CODE-END: S11-RESOURCE-SAMPLER
