#!/usr/bin/env bash
# AI-CODE-BEGIN: S9-DASHBOARD-PROCESS-ACCEPTANCE
# 阶段9真实进程验收：独立本机端口、静态页面、JSON、聚合指标和有界慢请求。
set -euo pipefail

project_dir="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}"
build_dir="${2:-${project_dir}/build-stage9-debug}"
temporary_dir="$(mktemp -d /tmp/edgegate-stage9-manual.XXXXXX)"
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
    if [[ "${temporary_dir}" == /tmp/edgegate-stage9-manual.* ]]; then
        rm -rf -- "${temporary_dir}"
    fi
}
trap cleanup EXIT

for port in 18080 18081 19081 19082 19083; do
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
sed -i 's/slow_request_threshold_ms: 500/slow_request_threshold_ms: 100/' "${config_path}"

# backend-a 固定延迟150ms，另外两个保持快速，用于同时产生正常与慢请求。
"${build_dir}/edgegate_test_backend" backend-a 19081 150 \
    >"${temporary_dir}/backend-a.log" 2>&1 &
backend_pids+=("$!")
for item in "backend-b:19082" "backend-c:19083"; do
    name="${item%%:*}"
    port="${item##*:}"
    "${build_dir}/edgegate_test_backend" "${name}" "${port}" \
        >"${temporary_dir}/${name}.log" 2>&1 &
    backend_pids+=("$!")
done

"${build_dir}/edgegate" "${config_path}" \
    >"${temporary_dir}/edgegate.log" 2>&1 &
edgegate_pid="$!"

for _ in $(seq 1 100); do
    if curl --fail --silent --max-time 1 http://127.0.0.1:18081/ \
        >"${temporary_dir}/dashboard.html"; then
        break
    fi
    sleep 0.03
done
grep -q 'EDGEGATE / OBSERVABILITY' "${temporary_dir}/dashboard.html"
curl --fail --silent --max-time 2 http://127.0.0.1:18081/app.css \
    >"${temporary_dir}/app.css"
curl --fail --silent --max-time 2 http://127.0.0.1:18081/app.js \
    >"${temporary_dir}/app.js"
grep -q 'status-bars' "${temporary_dir}/app.css"
grep -q '/api/dashboard' "${temporary_dir}/app.js"

for index in $(seq 1 6); do
    curl --fail --silent --max-time 3 \
        -H 'Host: api.edgegate.test' \
        "http://127.0.0.1:18080/api?token=must-not-appear&request=${index}" \
        >/dev/null
done
missing_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
    --max-time 2 -H 'Host: missing.edgegate.test' \
    http://127.0.0.1:18080/no-route)"
[[ "${missing_status}" == "404" ]]

curl --fail --silent --max-time 2 http://127.0.0.1:18081/api/dashboard \
    >"${temporary_dir}/snapshot.json"
python3 - "${temporary_dir}/snapshot.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    data = json.load(source)
metrics = data["metrics"]
assert data["schema_version"] == 1
assert data["service"]["state"] == "running"
assert metrics["requests"]["total"] == 7
assert metrics["requests"]["status_classes"]["2xx"] == 6
assert metrics["requests"]["status_codes"]["404"] == 1
assert metrics["latency_ms"]["samples"] == 7
assert metrics["latency_ms"]["approximate"] is True
assert sum(item["attempts"] for item in metrics["upstreams"]) == 6
assert len(metrics["slow_requests"]) >= 1
assert all("?" not in item["path"] for item in metrics["slow_requests"])
assert len(metrics["recent_errors"]) >= 1
serialized = json.dumps(data)
assert "must-not-appear" not in serialized
PY

post_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
    --request POST --max-time 2 http://127.0.0.1:18081/api/dashboard)"
reload_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
    --max-time 2 http://127.0.0.1:18081/reload)"
[[ "${post_status}" == "405" ]]
[[ "${reload_status}" == "404" ]]

"${build_dir}/edgegatectl" --socket "${socket_path}" stop >/dev/null
wait "${edgegate_pid}"
edgegate_pid=""
echo "DASHBOARD_PAGE_ASSETS=PASS"
echo "DASHBOARD_JSON_METRICS=PASS"
echo "DASHBOARD_READ_ONLY=PASS"
echo "SLOW_REQUEST_PRIVACY=PASS"
echo "STAGE9_DASHBOARD_ACCEPTANCE=PASS"
# AI-CODE-END: S9-DASHBOARD-PROCESS-ACCEPTANCE
