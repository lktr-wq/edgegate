<!-- AI-CODE-BEGIN: S10-TROUBLESHOOTING -->
# EdgeGate 故障排查

排查顺序不是立即改配置，而是先确定问题在哪一段：

```text
客户端 → 18080 监听 → 路由 → 上游健康/端口 → 上游 HTTP 响应
                  ↘ /run/edgegate/edgegate.sock
                  ↘ 127.0.0.1:18081 Dashboard
```

## 1. 服务无法启动

先收集：

```bash
sudo systemctl status edgegate --no-pager
sudo journalctl -u edgegate -n 100 --no-pager
sudo -u edgegate /usr/local/bin/edgegate /etc/edgegate/edgegate.yaml
```

常见原因：

- YAML 缩进、字段名、数值范围或引用关系非法。
- `18080` 或 `18081` 已被其他程序监听。
- 配置权限导致 `edgegate` 用户无法读取。
- 手动改成了 systemd 禁止写入的日志/Socket 目录。

检查端口和权限：

```bash
sudo ss -ltnp | grep -E ':(18080|18081)\b'
sudo namei -l /etc/edgegate/edgegate.yaml
sudo ls -ld /run/edgegate /var/log/edgegate
```

## 2. `edgegatectl` 报 cannot connect

```bash
sudo systemctl is-active edgegate
sudo ls -l /run/edgegate/edgegate.sock
sudo edgegatectl --socket /run/edgegate/edgegate.sock status
```

- Socket 不存在：服务未运行、启动失败或配置中路径不同。
- `Permission denied`：该 Socket 设计为 `0600`，请使用 `sudo`，不要放宽为任意本机用户可写。
- `Connection refused`：可能是服务异常退出后的短暂窗口，结合 `systemctl status` 和 journal 判断。

## 3. 代理返回 `404`

`404` 表示 EdgeGate 没找到 Host + 路径路由，不代表上游崩溃。

```bash
curl -v -H 'Host: api.edgegate.test' http://127.0.0.1:18080/api/demo
sudo edgegatectl --socket /run/edgegate/edgegate.sock routes
```

检查 Host 是否被 curl 正确设置，路径是否以配置前缀开头。

## 4. 代理返回 `502`、`503` 或 `504`

- `502 Bad Gateway`：连接被拒绝、上游提前断开或返回非法 HTTP。
- `503 Service Unavailable`：路由存在，但当前没有健康上游。
- `504 Gateway Timeout`：上游连接或响应超时。

```bash
sudo edgegatectl --socket /run/edgegate/edgegate.sock upstreams
sudo ss -ltnp | grep -E ':(19081|19082|19083)\b'
curl -v http://127.0.0.1:19081/health
sudo tail -n 50 /var/log/edgegate/error.log
```

先直连上游验证它本身，再判断 EdgeGate 路径；否则会把后端未启动误判为代理算法故障。

## 5. reload 失败

```bash
sudo systemctl reload edgegate
sudo journalctl -u edgegate -n 30 --no-pager
sudo tail -n 30 /var/log/edgegate/management.log
sudo tail -n 30 /var/log/edgegate/error.log
```

原子重载失败时，已运行的旧配置不会被部分替换。如果修改了 `listen`、management Socket 或 Dashboard 监听地址/端口，v1 会明确拒绝热重载，需要安排服务重启。

## 6. Dashboard 在 Windows 打不开

在 Ubuntu 先验证：

```bash
curl -I http://127.0.0.1:18081/
curl -sS http://127.0.0.1:18081/api/dashboard | jq '.service'
```

如果 Ubuntu 正常但 Windows 失败，检查 PowerShell 中的 SSH 隧道是否仍保持连接：

```powershell
ssh -L 18081:127.0.0.1:18081 <ubuntu-user>@<ubuntu-host>
```

不要为解决这个问题而把 Dashboard 直接改成对外网卡监听。

## 7. 日志或文件描述符持续增长

```bash
sudo ls -lh /var/log/edgegate
service_pid="$(systemctl show -p MainPID --value edgegate)"
sudo ls "/proc/${service_pid}/fd" | wc -l
sudo systemctl show edgegate -p MemoryCurrent -p TasksCurrent
```

日志文件应受 `max_file_size` 和 `max_files` 限制。指标中的最近错误和慢请求也是有界队列。v1.0.0 已完成 12 小时长稳和文件描述符泄漏验收；若修改网络状态机、缓冲区或资源生命周期，仍应重新执行对应验收，不能沿用旧结果。

## 8. 最小证据包

如果问题需要别人协助，请提供：

```bash
sudo systemctl status edgegate --no-pager
sudo journalctl -u edgegate -n 100 --no-pager
sudo edgegatectl --socket /run/edgegate/edgegate.sock status
sudo edgegatectl --socket /run/edgegate/edgegate.sock upstreams
sudo ss -ltnp | grep -E ':(18080|18081|19081|19082|19083)\b'
```

分享前删除主机名、私有 IP、账号、Cookie、Authorization 和配置中的敏感上游信息。
<!-- AI-CODE-END: S10-TROUBLESHOOTING -->
