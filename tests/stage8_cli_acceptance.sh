#!/usr/bin/env bash
# AI-CODE-BEGIN: S8-CLI-PROCESS-ACCEPTANCE
# 阶段8真实进程验收：三个后端、正式edgegate、edgegatectl、curl和SIGTERM。
set -euo pipefail

project_dir="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}"
build_dir="${2:-${project_dir}/build-stage8-debug}"
temporary_dir="$(mktemp -d /tmp/edgegate-stage8-manual.XXXXXX)"
backend_pids=()
edgegate_pid=""

cleanup() {
    if [[ -n "${edgegate_pid}" ]] && kill -0 "${edgegate_pid}" 2>/dev/null; then
        kill "${edgegate_pid}" 2>/dev/null || true
        wait "${edgegate_pid}" 2>/dev/null || true
    fi
    for pid in "${backend_pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    # temporary_dir由本脚本的mktemp创建，并先核对固定前缀再递归清理。
    if [[ "${temporary_dir}" == /tmp/edgegate-stage8-manual.* ]]; then
        rm -rf -- "${temporary_dir}"
    fi
}
trap cleanup EXIT

wait_for_path() {
    local path="$1"
    for _ in $(seq 1 100); do
        [[ -S "${path}" ]] && return 0
        sleep 0.02
    done
    return 1
}

wait_for_state() {
    local socket_path="$1"
    local expected="$2"
    for _ in $(seq 1 100); do
        if "${build_dir}/edgegatectl" --socket "${socket_path}" status 2>/dev/null |
           grep -q "State: *${expected}"; then
            return 0
        fi
        sleep 0.02
    done
    return 1
}

for port in 18080 19081 19082 19083; do
    if ss -ltnH | awk '{print $4}' | grep -Eq ":${port}$"; then
        echo "acceptance aborted: TCP port ${port} is already in use" >&2
        exit 1
    fi
done

config_path="${temporary_dir}/edgegate.yaml"
socket_path="${temporary_dir}/edgegate.sock"
log_directory="${temporary_dir}/logs"
cp "${project_dir}/config/edgegate.yaml" "${config_path}"
sed -i "s#/tmp/edgegate.sock#${socket_path}#" "${config_path}"
sed -i "s#/tmp/edgegate-logs#${log_directory}#" "${config_path}"

for item in "backend-a:19081" "backend-b:19082" "backend-c:19083"; do
    name="${item%%:*}"
    port="${item##*:}"
    "${build_dir}/edgegate_test_backend" "${name}" "${port}" \
        >"${temporary_dir}/${name}.log" 2>&1 &
    backend_pids+=("$!")
done

"${build_dir}/edgegate" "${config_path}" \
    >"${temporary_dir}/edgegate.log" 2>&1 &
edgegate_pid="$!"
wait_for_path "${socket_path}"

mode="$(stat -c '%a' "${socket_path}")"
[[ "${mode}" == "600" ]]
echo "SOCKET_MODE=${mode}"

"${build_dir}/edgegatectl" --socket "${socket_path}" status
"${build_dir}/edgegatectl" --socket "${socket_path}" routes
"${build_dir}/edgegatectl" --socket "${socket_path}" upstreams
stats_json="$("${build_dir}/edgegatectl" --socket "${socket_path}" stats --json)"
grep -q '"completed_requests"' <<<"${stats_json}"
echo "STATS_JSON=${stats_json}"

first_body="$(curl --fail --silent --show-error --max-time 2 \
    -H 'Host: api.edgegate.test' http://127.0.0.1:18080/api)"
echo "INITIAL_BODY=${first_body}"

# 合法热重载：把唯一命中的路径从/api改为/changed。
sed -i 's#path_prefix: /api#path_prefix: /changed#' "${config_path}"
cp "${config_path}" "${temporary_dir}/valid-changed.yaml"
"${build_dir}/edgegatectl" --socket "${socket_path}" reload
old_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
    --max-time 2 -H 'Host: api.edgegate.test' http://127.0.0.1:18080/api)"
[[ "${old_status}" == "404" ]]
changed_body="$(curl --fail --silent --show-error --max-time 2 \
    -H 'Host: api.edgegate.test' http://127.0.0.1:18080/changed)"
echo "RELOADED_BODY=${changed_body}"

# 无效热重载：未知根字段必须失败，运行时继续保留上一份有效配置。
sed -i '/^management:/i invalid_key: true' "${config_path}"
set +e
"${build_dir}/edgegatectl" --socket "${socket_path}" reload \
    >"${temporary_dir}/invalid-reload.out" 2>&1
reload_exit="$?"
set -e
[[ "${reload_exit}" == "1" ]]
grep -q 'old configuration kept' "${temporary_dir}/invalid-reload.out"
preserved_body="$(curl --fail --silent --show-error --max-time 2 \
    -H 'Host: api.edgegate.test' http://127.0.0.1:18080/changed)"
echo "PRESERVED_BODY=${preserved_body}"
cp "${temporary_dir}/valid-changed.yaml" "${config_path}"

"${build_dir}/edgegatectl" --socket "${socket_path}" drain
"${build_dir}/edgegatectl" --socket "${socket_path}" drain
wait_for_state "${socket_path}" drained
set +e
curl --silent --max-time 1 -H 'Host: api.edgegate.test' \
    http://127.0.0.1:18080/changed >/dev/null
curl_after_drain="$?"
set -e
[[ "${curl_after_drain}" != "0" ]]
echo "DRAIN_REFUSED_NEW_CONNECTION=1"

"${build_dir}/edgegatectl" --socket "${socket_path}" stop
wait "${edgegate_pid}"
edgegate_pid=""
[[ ! -e "${socket_path}" ]]
[[ -f "${log_directory}/access.log" ]]
[[ -f "${log_directory}/error.log" ]]
[[ -f "${log_directory}/management.log" ]]
echo "CLI_STOP_EXIT=0"

# 单独再启动一次正式服务，用真实SIGTERM验证signalfd也走同一平滑停止流程。
signal_config="${temporary_dir}/signal.yaml"
signal_socket="${temporary_dir}/signal.sock"
signal_logs="${temporary_dir}/signal-logs"
cp "${project_dir}/config/edgegate.yaml" "${signal_config}"
sed -i "s#/tmp/edgegate.sock#${signal_socket}#" "${signal_config}"
sed -i "s#/tmp/edgegate-logs#${signal_logs}#" "${signal_config}"
"${build_dir}/edgegate" "${signal_config}" \
    >"${temporary_dir}/edgegate-signal.log" 2>&1 &
edgegate_pid="$!"
wait_for_path "${signal_socket}"
kill -TERM "${edgegate_pid}"
wait "${edgegate_pid}"
signal_exit="$?"
edgegate_pid=""
[[ "${signal_exit}" == "0" ]]
[[ ! -e "${signal_socket}" ]]
echo "SIGTERM_EXIT=${signal_exit}"
echo "STAGE8_CLI_ACCEPTANCE=PASS"
# AI-CODE-END: S8-CLI-PROCESS-ACCEPTANCE
