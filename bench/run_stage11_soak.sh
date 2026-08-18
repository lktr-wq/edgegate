#!/usr/bin/env bash
# AI-CODE-BEGIN: S11-SOAK-RUNNER
# 无人值守长稳测试。默认 43200 秒（12小时），每批请求完成后持续记录结果。
# 用法：run_stage11_soak.sh [PROJECT] [BUILD] [DURATION_SECONDS] [ARTIFACT_ROOT]
set -Eeuo pipefail

project_dir="${1:-/home/zy/projects/edgegate}"
build_dir="${2:-${project_dir}/build-stage11-release}"
duration_seconds="${3:-43200}"
artifact_root="${4:-${project_dir}/artifacts/stage11}"
run_id="soak-$(date -u +%Y%m%dT%H%M%SZ)"
artifact_dir="${artifact_root}/${run_id}"
temporary_dir="$(mktemp -d /tmp/edgegate-stage11-soak.XXXXXX)"
backend_pids=()
edgegate_pid=""
sampler_pid=""
final_status="FAIL"

mkdir -p "${artifact_dir}"
printf '%s\n' "RUNNING" >"${artifact_dir}/STATUS"
printf '%s\n' "${artifact_dir}" >"${artifact_root}/LATEST_SOAK"

cleanup() {
    local exit_status=$?
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
    [[ "${temporary_dir}" == /tmp/edgegate-stage11-soak.* ]] && rm -rf -- "${temporary_dir}"
    if [[ "${final_status}" != PASS ]]; then
        printf 'FAIL exit=%s time=%s\n' "${exit_status}" "$(date -u +%FT%TZ)" \
            >"${artifact_dir}/STATUS"
    fi
}
trap cleanup EXIT
trap 'status=$?; echo "STAGE11_SOAK=FAIL line=${LINENO} command=${BASH_COMMAND} status=${status}" >&2' ERR

if ! [[ "${duration_seconds}" =~ ^[0-9]+$ ]] || (( duration_seconds < 10 )); then
    echo "duration must be an integer of at least 10 seconds" >&2
    exit 2
fi
for required in edgegate edgegatectl edgegate_test_backend edgegate_loadgen; do
    test -x "${build_dir}/${required}"
done
for port in 18280 18281 19281 19282 19283; do
    if ss -ltnH | awk '{print $4}' | grep -Eq ":${port}$"; then
        echo "stage11 soak port ${port} is already in use" >&2
        exit 1
    fi
done

config_path="${temporary_dir}/edgegate.yaml"
socket_path="${temporary_dir}/edgegate.sock"
log_directory="${temporary_dir}/logs"
sed \
    -e 's/port: 18080/port: 18280/' \
    -e 's/port: 18081/port: 18281/' \
    -e 's/port: 19081/port: 19281/' \
    -e 's/port: 19082/port: 19282/' \
    -e 's/port: 19083/port: 19283/' \
    -e "s#/tmp/edgegate.sock#${socket_path}#" \
    -e "s#/tmp/edgegate-logs#${log_directory}#" \
    "${project_dir}/config/edgegate.yaml" >"${config_path}"
cp "${config_path}" "${artifact_dir}/effective-config.yaml"

for item in backend-a:19281 backend-b:19282 backend-c:19283; do
    name="${item%%:*}"
    port="${item##*:}"
    "${build_dir}/edgegate_test_backend" "${name}" "${port}" \
        >"${artifact_dir}/${name}.log" 2>&1 &
    backend_pids+=("$!")
done
"${build_dir}/edgegate" "${config_path}" \
    >"${artifact_dir}/edgegate-console.log" 2>&1 &
edgegate_pid="$!"
printf '%s\n' "${edgegate_pid}" >"${artifact_dir}/edgegate.pid"

ready=false
for _ in {1..100}; do
    if "${build_dir}/edgegatectl" --socket "${socket_path}" status >/dev/null 2>&1; then
        ready=true
        break
    fi
    sleep 0.05
done
test "${ready}" = true

bash "${project_dir}/bench/sample_process_resources.sh" \
    "${edgegate_pid}" "${artifact_dir}/resources.csv" 5 &
sampler_pid="$!"

started_epoch="$(date +%s)"
deadline="$((started_epoch + duration_seconds))"
batch=0
: >"${artifact_dir}/runs.jsonl"
: >"${artifact_dir}/progress.log"

while (( $(date +%s) < deadline )); do
    batch=$((batch + 1))
    batch_path="${temporary_dir}/batch-${batch}.json"
    "${build_dir}/edgegate_loadgen" \
        --address 127.0.0.1 --port 18280 \
        --host api.edgegate.test --path /api/soak \
        --requests 5000 --concurrency 20 --timeout-ms 5000 \
        --output "${batch_path}" >/dev/null
    jq -c . "${batch_path}" >>"${artifact_dir}/runs.jsonl"
    printf 'time=%s batch=%s total_successes=%s\n' \
        "$(date -u +%FT%TZ)" "${batch}" \
        "$(jq -s 'map(.results.successes) | add' "${artifact_dir}/runs.jsonl")" \
        >>"${artifact_dir}/progress.log"
    sleep 1
done

# 资源采样间隔是5秒；保留一个完整间隔，确保最后一个样本覆盖目标时长。
sleep 5
"${build_dir}/edgegatectl" --socket "${socket_path}" stats --json \
    >"${artifact_dir}/edgegate-final-stats.json"
active="$(jq -r '.active_sessions' "${artifact_dir}/edgegate-final-stats.json")"
test "${active}" = 0

"${build_dir}/edgegatectl" --socket "${socket_path}" stop >/dev/null
wait "${edgegate_pid}"
edgegate_pid=""
wait "${sampler_pid}" || true
sampler_pid=""

python3 "${project_dir}/bench/analyze_stage11.py" \
    --resources "${artifact_dir}/resources.csv" \
    --runs "${artifact_dir}/runs.jsonl" \
    --output "${artifact_dir}/soak-report.json" \
    --minimum-duration-seconds "${duration_seconds}"

jq -e '.pass == true' "${artifact_dir}/soak-report.json" >/dev/null
final_status="PASS"
printf 'PASS time=%s\n' "$(date -u +%FT%TZ)" >"${artifact_dir}/STATUS"
echo "STAGE11_SOAK_ARTIFACTS=${artifact_dir}"
echo "STAGE11_SOAK=PASS"
# AI-CODE-END: S11-SOAK-RUNNER
