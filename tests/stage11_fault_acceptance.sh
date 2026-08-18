#!/usr/bin/env bash
# AI-CODE-BEGIN: S11-FAULT-ACCEPTANCE
# 故障矩阵：畸形/超限/慢客户端、提前断开、上游崩溃、全部不健康、无效热重载和 fd 回收。
set -Eeuo pipefail
trap 'status=$?; echo "STAGE11_FAULT_ACCEPTANCE=FAIL line=${LINENO} command=${BASH_COMMAND} status=${status}" >&2' ERR

project_dir="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}"
build_dir="${2:-${project_dir}/build-stage11-debug}"
artifact_root="${3:-${project_dir}/artifacts/stage11}"
artifact_dir="${artifact_root}/fault-$(date -u +%Y%m%dT%H%M%SZ)"
temporary_dir="$(mktemp -d /tmp/edgegate-stage11-fault.XXXXXX)"
backend_pids=()
edgegate_pid=""

cleanup() {
    if [[ -n "${edgegate_pid}" ]] && kill -0 "${edgegate_pid}" 2>/dev/null; then
        kill "${edgegate_pid}" 2>/dev/null || true
        wait "${edgegate_pid}" 2>/dev/null || true
    fi
    for pid in "${backend_pids[@]:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    [[ "${temporary_dir}" == /tmp/edgegate-stage11-fault.* ]] && rm -rf -- "${temporary_dir}"
}
trap cleanup EXIT

mkdir -p "${artifact_dir}"
for required in edgegate edgegatectl edgegate_test_backend edgegate_loadgen; do
    test -x "${build_dir}/${required}"
done
for port in 18380 18381 19381 19382 19383 18390 18391; do
    if ss -ltnH | awk '{print $4}' | grep -Eq ":${port}$"; then
        echo "stage11 fault port ${port} is already in use" >&2
        exit 1
    fi
done

config_path="${temporary_dir}/edgegate.yaml"
valid_config="${temporary_dir}/edgegate.valid.yaml"
socket_path="${temporary_dir}/edgegate.sock"
log_directory="${temporary_dir}/logs"
sed \
    -e 's/port: 18080/port: 18380/' \
    -e 's/port: 18081/port: 18381/' \
    -e 's/port: 19081/port: 19381/' \
    -e 's/port: 19082/port: 19382/' \
    -e 's/port: 19083/port: 19383/' \
    -e 's/client_header_ms: 10000/client_header_ms: 300/' \
    -e 's/upstream_connect_ms: 2000/upstream_connect_ms: 300/' \
    -e 's/upstream_header_ms: 5000/upstream_header_ms: 500/' \
    -e 's/io_idle_ms: 15000/io_idle_ms: 500/' \
    -e 's/interval_ms: 5000/interval_ms: 200/' \
    -e 's/failure_threshold: 3/failure_threshold: 1/' \
    -e 's/success_threshold: 2/success_threshold: 1/' \
    -e "s#/tmp/edgegate.sock#${socket_path}#" \
    -e "s#/tmp/edgegate-logs#${log_directory}#" \
    "${project_dir}/config/edgegate.yaml" >"${config_path}"
cp "${config_path}" "${valid_config}"

for item in backend-a:19381 backend-b:19382 backend-c:19383; do
    name="${item%%:*}"
    port="${item##*:}"
    "${build_dir}/edgegate_test_backend" "${name}" "${port}" \
        >"${artifact_dir}/${name}.log" 2>&1 &
    backend_pids+=("$!")
done
"${build_dir}/edgegate" "${config_path}" \
    >"${artifact_dir}/edgegate-console.log" 2>&1 &
edgegate_pid="$!"

for _ in {1..100}; do
    "${build_dir}/edgegatectl" --socket "${socket_path}" status >/dev/null 2>&1 && break
    sleep 0.05
done
"${build_dir}/edgegatectl" --socket "${socket_path}" status >/dev/null

# Python 只负责构造 curl 不方便表达的原始字节和慢发送行为。
python3 - 18380 "${artifact_dir}/raw-results.json" <<'PY'
import json
import socket
import sys
import time

port = int(sys.argv[1])

def exchange(payload: bytes, wait: float = 0.0) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.settimeout(2)
        client.sendall(payload)
        if wait:
            time.sleep(wait)
        data = b""
        while True:
            try:
                chunk = client.recv(4096)
            except (ConnectionResetError, socket.timeout):
                break
            if not chunk:
                break
            data += chunk
        return data

def status(response: bytes) -> int:
    return int(response.split(b" ", 2)[1])

malformed = status(exchange(b"GET /api HTTP/1.1\r\nBroken-Header\r\n\r\n"))
oversized = status(exchange(
    b"GET /api HTTP/1.1\r\nHost: api.edgegate.test\r\nX-Large: "
    + b"x" * 9000 + b"\r\n\r\n"
))

with socket.create_connection(("127.0.0.1", port), timeout=2) as slow:
    slow.settimeout(2)
    slow.sendall(b"GET /api HTTP/1.1\r\nHost: api.edgegate.test\r\n")
    time.sleep(0.8)
    try:
        closed = slow.recv(1) == b""
    except ConnectionResetError:
        closed = True

# 提前退出模拟客户端发完请求后不再接收响应。
early = socket.create_connection(("127.0.0.1", port), timeout=2)
early.sendall(b"GET /api/early HTTP/1.1\r\nHost: api.edgegate.test\r\n\r\n")
early.close()

result = {"malformed_status": malformed, "oversized_status": oversized,
          "slow_client_closed": closed}
with open(sys.argv[2], "w", encoding="utf-8") as output:
    json.dump(result, output, indent=2)
assert malformed == 400
assert oversized == 413
assert closed
PY

# 无效配置重载必须失败，但当前有效配置和请求服务继续工作。
sed '0,/port: 18380/s//port: 0/' "${valid_config}" >"${config_path}"
if "${build_dir}/edgegatectl" --socket "${socket_path}" reload \
    >"${artifact_dir}/invalid-reload.out" 2>&1; then
    echo "invalid reload unexpectedly succeeded" >&2
    exit 1
fi
cp "${valid_config}" "${config_path}"
"${build_dir}/edgegate_loadgen" --port 18380 --requests 20 --concurrency 4 \
    --output "${artifact_dir}/after-invalid-reload.json" >/dev/null

# 一轮连接风暴结束后，活动会话与 fd 应回到稳定基线。
baseline_fd="$(find "/proc/${edgegate_pid}/fd" -mindepth 1 -maxdepth 1 | wc -l)"
"${build_dir}/edgegate_loadgen" --port 18380 --requests 2000 --concurrency 25 \
    --output "${artifact_dir}/fd-recovery-load.json" >/dev/null
for _ in {1..100}; do
    active="$("${build_dir}/edgegatectl" --socket "${socket_path}" stats --json | jq -r '.active_sessions')"
    [[ "${active}" == 0 ]] && break
    sleep 0.05
done
test "${active}" = 0
sleep 0.3
final_fd="$(find "/proc/${edgegate_pid}/fd" -mindepth 1 -maxdepth 1 | wc -l)"
test "${final_fd}" -le "$((baseline_fd + 6))"

# Debug 构建和并发流量可能让一次健康探测超过很短的调度窗口。
# 先确认三个上游都已恢复健康，避免把测试前置状态误判成崩溃影响。
all_healthy=false
for _ in {1..100}; do
    if curl -fsS --max-time 1 http://127.0.0.1:18381/api/dashboard 2>/dev/null \
        | jq -e '[.upstreams[] | select(.healthy == true)] | length == 3' >/dev/null; then
        all_healthy=true
        break
    fi
    sleep 0.1
done
test "${all_healthy}" = true

# 杀掉一个上游后，GET 安全重试或健康摘除应保证其余请求仍然成功。
kill "${backend_pids[0]}"
wait "${backend_pids[0]}" 2>/dev/null || true
backend_pids[0]=""
"${build_dir}/edgegate_loadgen" --port 18380 --requests 100 --concurrency 10 \
    --output "${artifact_dir}/one-backend-down.json" >/dev/null

# 再杀掉全部上游，健康检查摘除完成后必须稳定返回 503。
for index in 1 2; do
    kill "${backend_pids[$index]}"
    wait "${backend_pids[$index]}" 2>/dev/null || true
    backend_pids[$index]=""
done
all_unhealthy=false
for _ in {1..100}; do
    if curl -fsS --max-time 1 http://127.0.0.1:18381/api/dashboard 2>/dev/null \
        | jq -e '[.upstreams[] | select(.healthy == false)] | length == 3' >/dev/null; then
        all_unhealthy=true
        break
    fi
    sleep 0.1
done
test "${all_unhealthy}" = true
status="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 2 \
    -H 'Host: api.edgegate.test' http://127.0.0.1:18380/api/all-down 2>/dev/null)"
test "${status}" = 503

# 不可写日志目录应让第二个服务明确失败启动，而不是静默无日志运行。
bad_log_config="${temporary_dir}/bad-log.yaml"
sed \
    -e 's/port: 18380/port: 18390/' \
    -e 's/port: 18381/port: 18391/' \
    -e "s#${log_directory}#/proc/edgegate-stage11-unwritable#" \
    "${valid_config}" >"${bad_log_config}"
if "${build_dir}/edgegate" "${bad_log_config}" \
    >"${artifact_dir}/bad-log-startup.log" 2>&1; then
    echo "service unexpectedly started with unwritable logging directory" >&2
    exit 1
fi
grep -Eiq 'log|directory|filesystem|permission|create' "${artifact_dir}/bad-log-startup.log"

"${build_dir}/edgegatectl" --socket "${socket_path}" stats --json \
    >"${artifact_dir}/final-stats.json"
"${build_dir}/edgegatectl" --socket "${socket_path}" stop >/dev/null
wait "${edgegate_pid}"
edgegate_pid=""

echo "MALFORMED_AND_OVERSIZED_INPUT=PASS"
echo "SLOW_AND_ABORTED_CLIENT=PASS"
echo "INVALID_RELOAD_ROLLBACK=PASS"
echo "FD_SESSION_RECOVERY=PASS"
echo "UPSTREAM_CRASH_FAILOVER=PASS"
echo "ALL_UPSTREAMS_UNHEALTHY_503=PASS"
echo "LOG_STARTUP_FAILURE_VISIBLE=PASS"
echo "STAGE11_FAULT_ARTIFACTS=${artifact_dir}"
echo "STAGE11_FAULT_ACCEPTANCE=PASS"
# AI-CODE-END: S11-FAULT-ACCEPTANCE
