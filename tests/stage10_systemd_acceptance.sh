#!/usr/bin/env bash
# AI-CODE-BEGIN: S10-SYSTEMD-ACCEPTANCE
set -Eeuo pipefail

# 任何断言失败都显示行号和命令，避免 set -e 只静默退出。
trap 'status=$?; echo "STAGE10_SYSTEMD_ACCEPTANCE=FAIL line=${LINENO} command=${BASH_COMMAND} status=${status}" >&2' ERR

socket_path="/run/edgegate/edgegate.sock"
backend_binary="/usr/local/libexec/edgegate/edgegate_test_backend"
temporary_dir="$(mktemp -d /tmp/edgegate-stage10-systemd.XXXXXX)"
backend_pids=()

cleanup() {
    for pid in "${backend_pids[@]:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
        fi
    done
    if [[ "${temporary_dir}" == /tmp/edgegate-stage10-systemd.* ]]; then
        rm -rf -- "${temporary_dir}"
    fi
    # 验收中途失败也尽量恢复可展示的运行状态。
    if systemctl is-enabled --quiet edgegate.service 2>/dev/null &&
       ! systemctl is-active --quiet edgegate.service 2>/dev/null; then
        sudo -n systemctl start edgegate.service 2>/dev/null || true
    fi
}
trap cleanup EXIT

wait_for_edgegate_ready() {
    for attempt in {1..50}; do
        if sudo -n /usr/local/bin/edgegatectl \
            --socket "${socket_path}" status >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "EdgeGate management socket did not become ready" >&2
    return 1
}

# 只在脚本开始时请求一次 sudo，避免验收进行到一半才等待密码。
sudo -v

for required_path in \
    /usr/local/bin/edgegate \
    /usr/local/bin/edgegatectl \
    "${backend_binary}" \
    /usr/local/sbin/edgegate-service-install; do
    test -e "${required_path}"
done

# 正式配置所在目录是 0750 root:edgegate，zy 用户无法直接遍历才是预期安全结果。
sudo test -f /etc/edgegate/edgegate.yaml

getent passwd edgegate | grep -Fq '/usr/sbin/nologin'

"${backend_binary}" backend-a 19081 >"${temporary_dir}/backend-a.log" 2>&1 &
backend_pids+=("$!")
"${backend_binary}" backend-b 19082 >"${temporary_dir}/backend-b.log" 2>&1 &
backend_pids+=("$!")
"${backend_binary}" backend-c 19083 >"${temporary_dir}/backend-c.log" 2>&1 &
backend_pids+=("$!")

for port in 19081 19082 19083; do
    backend_ready=false
    for attempt in {1..50}; do
        if curl -fsS "http://127.0.0.1:${port}/health" >/dev/null; then
            backend_ready=true
            break
        fi
        sleep 0.1
    done
    if [[ "${backend_ready}" != true ]]; then
        echo "test backend on port ${port} did not become ready" >&2
        exit 1
    fi
done

sudo systemctl restart edgegate.service
test "$(systemctl is-active edgegate.service)" = "active"
wait_for_edgegate_ready

# systemctl 返回后再给进程表一个很短的稳定窗口。比较数字 UID
# 而不是 ps 的定宽用户名文本，避免显示截断或瞬时竞态。
edgegate_uid="$(id -u edgegate)"
service_identity_ready=false
for attempt in {1..50}; do
    service_pid="$(systemctl show -p MainPID --value edgegate.service)"
    if [[ "${service_pid}" =~ ^[0-9]+$ ]] &&
       (( service_pid > 1 )) &&
       [[ -d "/proc/${service_pid}" ]] &&
       [[ "$(stat -c '%u' "/proc/${service_pid}")" = "${edgegate_uid}" ]]; then
        service_identity_ready=true
        break
    fi
    sleep 0.1
done
test "${service_identity_ready}" = true

test "$(sudo stat -c '%U:%G:%a' /etc/edgegate/edgegate.yaml)" = "root:edgegate:640"
test "$(sudo stat -c '%U:%G:%a' /run/edgegate)" = "edgegate:edgegate:750"
test "$(sudo stat -c '%U:%G:%a' /var/log/edgegate)" = "edgegate:edgegate:750"
test "$(sudo stat -c '%U:%G:%a' "${socket_path}")" = "edgegate:edgegate:600"

for attempt in {1..50}; do
    if curl -fsS -o /dev/null -H 'Host: api.edgegate.test' \
        http://127.0.0.1:18080/api/health; then
        break
    fi
    sleep 0.1
done

responses="${temporary_dir}/responses.txt"
for request in {1..6}; do
    curl -fsS -H 'Host: api.edgegate.test' \
        http://127.0.0.1:18080/api/demo >>"${responses}"
done
test "$(grep -xc 'backend-a' "${responses}")" -eq 2
test "$(grep -xc 'backend-b' "${responses}")" -eq 2
test "$(grep -xc 'backend-c' "${responses}")" -eq 2

sudo /usr/local/bin/edgegatectl --socket "${socket_path}" status \
    | grep -Fq 'State:'
sudo /usr/local/bin/edgegatectl --socket "${socket_path}" routes \
    | grep -Fq 'api-route'
sudo /usr/local/bin/edgegatectl --socket "${socket_path}" upstreams \
    | grep -Fq 'backend-a'
sudo /usr/local/bin/edgegatectl --socket "${socket_path}" stats --json \
    | jq -e '.observability.requests.total >= 6' >/dev/null

curl -fsS http://127.0.0.1:18081/ | grep -Fq 'EdgeGate'
curl -fsS http://127.0.0.1:18081/api/dashboard \
    | jq -e '.service.state == "running"' >/dev/null

# systemd 的 ExecReload 实际通过 Unix Socket 执行原子 reload。
sudo systemctl reload edgegate.service
test "$(systemctl is-active edgegate.service)" = "active"

# 重复运行安装器不得覆盖使用者的正式配置。
config_hash_before="$(sudo sha256sum /etc/edgegate/edgegate.yaml | awk '{print $1}')"
sudo /usr/local/sbin/edgegate-service-install >/dev/null
config_hash_after="$(sudo sha256sum /etc/edgegate/edgegate.yaml | awk '{print $1}')"
test "${config_hash_before}" = "${config_hash_after}"

sudo systemctl restart edgegate.service
test "$(systemctl is-active edgegate.service)" = "active"
wait_for_edgegate_ready

# systemctl stop 会发送 SIGTERM，EdgeGate 通过 signalfd 进入排空停止。
management_lines_before="$(sudo wc -l /var/log/edgegate/management.log | awk '{print $1}')"
sudo systemctl stop edgegate.service
if systemctl is-active --quiet edgegate.service; then
    echo "edgegate.service remained active after systemctl stop" >&2
    exit 1
fi
sudo tail -n "+$((management_lines_before + 1))" \
    /var/log/edgegate/management.log \
    | grep -Fq 'event=stop detail=source=SIGTERM'

# 验收后重新启动，保留可供用户展示的产品状态。
sudo systemctl start edgegate.service
test "$(systemctl is-active edgegate.service)" = "active"
wait_for_edgegate_ready

echo "SYSTEMD_LOW_PRIVILEGE=PASS"
echo "SYSTEMD_PROXY_DASHBOARD=PASS"
echo "SYSTEMD_RELOAD_RESTART=PASS"
echo "SYSTEMD_SIGTERM=PASS"
echo "CONFIG_NO_OVERWRITE=PASS"
echo "STAGE10_SYSTEMD_ACCEPTANCE=PASS"
# AI-CODE-END: S10-SYSTEMD-ACCEPTANCE
