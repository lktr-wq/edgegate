#!/usr/bin/env bash
# AI-CODE-BEGIN: S11-LOAD-ACCEPTANCE
# 100并发/100000请求验收。所有进程使用临时端口，不干扰已安装的 systemd 服务。
set -Eeuo pipefail
trap 'status=$?; echo "STAGE11_LOAD_ACCEPTANCE=FAIL line=${LINENO} command=${BASH_COMMAND} status=${status}" >&2' ERR

project_dir="${1:-/home/zy/projects/edgegate}"
build_dir="${2:-${project_dir}/build-stage11-release}"
artifact_root="${3:-${project_dir}/artifacts/stage11}"
run_id="load-$(date -u +%Y%m%dT%H%M%SZ)"
artifact_dir="${artifact_root}/${run_id}"
temporary_dir="$(mktemp -d /tmp/edgegate-stage11-load.XXXXXX)"
backend_pids=()
edgegate_pid=""
sampler_pid=""

cleanup() {
    if [[ -n "${edgegate_pid}" ]] && kill -0 "${edgegate_pid}" 2>/dev/null; then
        kill "${edgegate_pid}" 2>/dev/null || true
        wait "${edgegate_pid}" 2>/dev/null || true
    fi
    if [[ -n "${sampler_pid}" ]] && kill -0 "${sampler_pid}" 2>/dev/null; then
        kill "${sampler_pid}" 2>/dev/null || true
        wait "${sampler_pid}" 2>/dev/null || true
    fi
    for pid in "${backend_pids[@]:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    [[ "${temporary_dir}" == /tmp/edgegate-stage11-load.* ]] && rm -rf -- "${temporary_dir}"
}
trap cleanup EXIT

for required in edgegate edgegatectl edgegate_test_backend edgegate_loadgen; do
    test -x "${build_dir}/${required}"
done
mkdir -p "${artifact_dir}"

for port in 18180 18181 19181 19182 19183; do
    if ss -ltnH | awk '{print $4}' | grep -Eq ":${port}$"; then
        echo "stage11 load port ${port} is already in use" >&2
        exit 1
    fi
done

config_path="${temporary_dir}/edgegate.yaml"
socket_path="${temporary_dir}/edgegate.sock"
log_directory="${temporary_dir}/logs"
sed \
    -e 's/port: 18080/port: 18180/' \
    -e 's/port: 18081/port: 18181/' \
    -e 's/port: 19081/port: 19181/' \
    -e 's/port: 19082/port: 19182/' \
    -e 's/port: 19083/port: 19183/' \
    -e "s#/tmp/edgegate.sock#${socket_path}#" \
    -e "s#/tmp/edgegate-logs#${log_directory}#" \
    "${project_dir}/config/edgegate.yaml" >"${config_path}"

for item in backend-a:19181 backend-b:19182 backend-c:19183; do
    name="${item%%:*}"
    port="${item##*:}"
    "${build_dir}/edgegate_test_backend" "${name}" "${port}" \
        >"${artifact_dir}/${name}.log" 2>&1 &
    backend_pids+=("$!")
done

"${build_dir}/edgegate" "${config_path}" \
    >"${artifact_dir}/edgegate-console.log" 2>&1 &
edgegate_pid="$!"

ready=false
for _ in {1..100}; do
    if "${build_dir}/edgegatectl" --socket "${socket_path}" status \
        >/dev/null 2>&1; then
        ready=true
        break
    fi
    sleep 0.05
done
test "${ready}" = true

bash "${project_dir}/bench/sample_process_resources.sh" \
    "${edgegate_pid}" "${artifact_dir}/resources.csv" 0.2 &
sampler_pid="$!"

"${build_dir}/edgegate_loadgen" \
    --address 127.0.0.1 --port 18180 \
    --host api.edgegate.test --path /api/benchmark \
    --requests 100000 --concurrency 100 --timeout-ms 5000 \
    --output "${artifact_dir}/load.json" \
    >"${artifact_dir}/load.stdout.json"

# 客户端关闭连接后，EventLoop 应回收全部会话，而不是留下 fd 泄漏。
sessions_drained=false
for _ in {1..100}; do
    "${build_dir}/edgegatectl" --socket "${socket_path}" stats --json \
        >"${artifact_dir}/edgegate-stats.json"
    if jq -e '.active_sessions == 0' "${artifact_dir}/edgegate-stats.json" >/dev/null; then
        sessions_drained=true
        break
    fi
    sleep 0.05
done
test "${sessions_drained}" = true

jq -e '.results.successes == 100000 and .results.failures == 0 and
       .results.status_codes["200"] == 100000 and
       .timing.latency_samples == 100000' \
    "${artifact_dir}/load.json" >/dev/null
jq -e '.completed_requests == 100000 and .client_errors == 0 and
       .observability.requests.status_classes["5xx"] == 0' \
    "${artifact_dir}/edgegate-stats.json" >/dev/null

jq -c . "${artifact_dir}/load.json" >"${artifact_dir}/runs.jsonl"
sleep 1
"${build_dir}/edgegatectl" --socket "${socket_path}" stop >/dev/null
wait "${edgegate_pid}"
edgegate_pid=""
wait "${sampler_pid}" || true
sampler_pid=""

python3 "${project_dir}/bench/analyze_stage11.py" \
    --resources "${artifact_dir}/resources.csv" \
    --runs "${artifact_dir}/runs.jsonl" \
    --output "${artifact_dir}/resource-report.json" \
    >"${artifact_dir}/resource-report.stdout.json"

echo "LOAD_100_CONCURRENCY=PASS"
echo "LOAD_100000_REQUESTS=PASS"
echo "LOAD_NO_LOSS_OR_5XX=PASS"
echo "LOAD_SESSIONS_DRAINED=PASS"
echo "STAGE11_LOAD_ARTIFACTS=${artifact_dir}"
echo "STAGE11_LOAD_ACCEPTANCE=PASS"
# AI-CODE-END: S11-LOAD-ACCEPTANCE
