#include "edgegate/runtime/dashboard_server.h"

// AI-CODE-BEGIN: S9-DASHBOARD-SERVER-IMPLEMENTATION
#include "edgegate/net/unique_fd.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace edgegate::runtime {

namespace {

constexpr std::size_t kMaximumRequestBytes = 8192;
constexpr int kDashboardBacklog = 32;

constexpr std::string_view kIndexHtml = R"html(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="color-scheme" content="dark">
  <title>EdgeGate Dashboard</title>
  <link rel="stylesheet" href="/app.css">
</head>
<body>
  <main class="shell">
    <header class="hero">
      <div><p class="eyebrow">EDGEGATE / OBSERVABILITY</p><h1>运行状态</h1></div>
      <div class="live"><span id="live-dot"></span><strong id="service-state">连接中</strong><small id="updated-at">等待首个快照</small></div>
    </header>
    <section class="cards" aria-label="核心指标">
      <article><span>运行时间</span><strong id="uptime">—</strong><small>当前进程</small></article>
      <article><span>活动连接</span><strong id="active">—</strong><small id="accepted">累计接受 —</small></article>
      <article><span>完成请求</span><strong id="requests">—</strong><small id="response-bytes">响应 —</small></article>
      <article><span>错误响应</span><strong id="errors">—</strong><small>4xx + 5xx</small></article>
    </section>
    <section class="grid two">
      <article class="panel"><div class="panel-title"><h2>请求延迟</h2><span>固定区间估算</span></div><div class="latency"><div><small>P50</small><strong id="p50">—</strong></div><div><small>P95</small><strong id="p95">—</strong></div><div><small>P99</small><strong id="p99">—</strong></div><div><small>MAX</small><strong id="pmax">—</strong></div></div></article>
      <article class="panel"><div class="panel-title"><h2>状态码分布</h2><span id="status-total">0 samples</span></div><div id="status-bars" class="status-bars"></div></article>
    </section>
    <section class="panel"><div class="panel-title"><h2>路由</h2><span>匹配与请求结果</span></div><div class="table-wrap"><table><thead><tr><th>ID</th><th>Host</th><th>路径前缀</th><th>上游数</th><th>请求</th><th>错误</th></tr></thead><tbody id="routes-body"></tbody></table></div></section>
    <section class="panel"><div class="panel-title"><h2>上游健康</h2><span>业务请求统计不含健康探测</span></div><div class="table-wrap"><table><thead><tr><th>节点</th><th>地址</th><th>健康</th><th>尝试</th><th>成功</th><th>失败</th><th>超时</th></tr></thead><tbody id="upstreams-body"></tbody></table></div></section>
    <section class="grid two detail-grid">
      <article class="panel"><div class="panel-title"><h2>慢请求</h2><span id="slow-rule">—</span></div><div id="slow-list" class="event-list"></div></article>
      <article class="panel"><div class="panel-title"><h2>最近错误</h2><span>内存有界摘要</span></div><div id="error-list" class="event-list"></div></article>
    </section>
    <footer>只读本机界面 · 不保存请求体、Cookie、认证头或查询参数</footer>
  </main>
  <script src="/app.js" defer></script>
</body>
</html>)html";

constexpr std::string_view kAppCss = R"css(:root{--bg:#081015;--panel:#101b22;--line:#22343e;--muted:#8da2ad;--text:#edf7f5;--mint:#64e3bd;--cyan:#5dc9ef;--amber:#f2b864;--red:#ff7d7d;font-family:Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 80% -10%,#173646 0,transparent 35%),var(--bg);color:var(--text);min-height:100vh}.shell{width:min(1440px,calc(100% - 40px));margin:auto;padding:34px 0 28px}.hero{display:flex;justify-content:space-between;align-items:flex-end;margin-bottom:24px}.eyebrow{color:var(--mint);letter-spacing:.18em;font-size:12px;font-weight:750;margin:0 0 8px}.hero h1{font-size:clamp(30px,4vw,52px);letter-spacing:-.04em;margin:0}.live{display:grid;grid-template-columns:auto auto;align-items:center;gap:2px 9px;text-align:right}.live span{width:9px;height:9px;border-radius:50%;background:var(--amber);box-shadow:0 0 14px currentColor}.live span.ok{background:var(--mint)}.live span.bad{background:var(--red)}.live small{grid-column:1/3;color:var(--muted)}.cards,.grid{display:grid;gap:14px}.cards{grid-template-columns:repeat(4,1fr)}.cards article,.panel{background:linear-gradient(145deg,rgba(18,32,40,.96),rgba(13,24,30,.96));border:1px solid var(--line);border-radius:15px;box-shadow:0 14px 35px rgba(0,0,0,.18)}.cards article{padding:20px;display:flex;flex-direction:column;min-height:126px}.cards span,.cards small,.panel-title span,footer{color:var(--muted)}.cards strong{font-size:32px;margin:auto 0 4px;letter-spacing:-.03em}.two{grid-template-columns:1fr 1fr;margin:14px 0}.panel{padding:19px;margin-top:14px;overflow:hidden}.grid .panel{margin-top:0}.panel-title{display:flex;align-items:baseline;justify-content:space-between;gap:14px;margin-bottom:18px}.panel-title h2{font-size:16px;margin:0}.panel-title span{font-size:12px}.latency{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}.latency div{background:#0b151b;border:1px solid #1d3039;border-radius:11px;padding:14px}.latency small{display:block;color:var(--muted);font-weight:700}.latency strong{display:block;font-size:22px;margin-top:5px}.status-bars{display:grid;gap:9px}.status-row{display:grid;grid-template-columns:42px 1fr 58px;gap:10px;align-items:center;font-size:12px}.bar{height:9px;background:#0a1419;border-radius:9px;overflow:hidden}.bar i{display:block;height:100%;min-width:2px;border-radius:9px;background:var(--cyan)}.status-row.warn .bar i{background:var(--amber)}.status-row.bad .bar i{background:var(--red)}table{width:100%;border-collapse:collapse;min-width:720px}th,td{text-align:left;padding:11px 10px;border-bottom:1px solid #1d3039;font-size:13px}th{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.08em}tbody tr:last-child td{border-bottom:0}.table-wrap{overflow-x:auto}.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 8px;border-radius:99px;background:rgba(100,227,189,.1);color:var(--mint);font-size:11px;font-weight:750}.pill.down{color:var(--red);background:rgba(255,125,125,.1)}.event-list{display:grid;gap:9px;max-height:340px;overflow:auto}.event{border:1px solid #1d3039;border-left:3px solid var(--amber);border-radius:9px;padding:11px 12px;background:#0b151b}.event.error{border-left-color:var(--red)}.event b{font-size:13px}.event p{margin:5px 0;color:#c8d7db;font-size:12px;overflow-wrap:anywhere}.event small{color:var(--muted)}.empty{color:var(--muted);font-size:13px;padding:25px 0;text-align:center}footer{text-align:center;font-size:12px;margin-top:22px}@media(max-width:900px){.cards{grid-template-columns:repeat(2,1fr)}.two{grid-template-columns:1fr}.detail-grid{grid-template-columns:1fr}}@media(max-width:540px){.shell{width:min(100% - 22px,1440px);padding-top:22px}.hero{align-items:flex-start}.hero h1{font-size:34px}.live{margin-top:8px}.cards{grid-template-columns:1fr 1fr}.cards article{padding:15px;min-height:110px}.cards strong{font-size:24px}.latency{grid-template-columns:1fr 1fr}.panel{padding:14px}})css";

constexpr std::string_view kAppJs = R"js('use strict';
const el=id=>document.getElementById(id);const fmt=n=>new Intl.NumberFormat('zh-CN').format(Number(n||0));
const bytes=n=>{n=Number(n||0);for(const u of ['B','KiB','MiB','GiB']){if(n<1024||u==='GiB')return `${n.toFixed(u==='B'?0:1)} ${u}`;n/=1024;}};
const duration=ms=>{ms=Number(ms||0);if(ms<1000)return `${ms} ms`;const s=Math.floor(ms/1000);if(s<60)return `${s} s`;const m=Math.floor(s/60);if(m<60)return `${m}m ${s%60}s`;const h=Math.floor(m/60);return `${h}h ${m%60}m`;};
const td=(tr,value)=>{const cell=document.createElement('td');cell.textContent=value;tr.appendChild(cell);};
function indexed(list,key){const out=new Map();for(const item of list||[])out.set(item[key],item);return out;}
function renderTable(body,rows,emptyText){body.replaceChildren();if(!rows.length){const tr=document.createElement('tr'),cell=document.createElement('td');cell.colSpan=8;cell.className='empty';cell.textContent=emptyText;tr.appendChild(cell);body.appendChild(tr);return;}for(const row of rows)body.appendChild(row);}
function eventNode(title,detail,time,isError){const box=document.createElement('div');box.className=`event${isError?' error':''}`;const b=document.createElement('b');b.textContent=title;const p=document.createElement('p');p.textContent=detail;const small=document.createElement('small');small.textContent=time||'—';box.append(b,p,small);return box;}
function render(data){const service=data.service||{},metrics=data.metrics||{},req=metrics.requests||{},lat=metrics.latency_ms||{},classes=req.status_classes||{};el('service-state').textContent=service.state||'unknown';el('live-dot').className=service.state==='running'?'ok':(service.state==='draining'?'':'bad');el('updated-at').textContent=`快照 ${new Date(data.generated_at).toLocaleTimeString()}`;el('uptime').textContent=duration(service.uptime_ms);el('active').textContent=fmt(service.active_sessions);el('accepted').textContent=`累计接受 ${fmt(data.runtime_stats?.accepted)}`;el('requests').textContent=fmt(req.total);el('response-bytes').textContent=`响应 ${bytes(req.response_bytes)}`;const errorCount=Number(classes['4xx']||0)+Number(classes['5xx']||0);el('errors').textContent=fmt(errorCount);for(const key of ['p50','p95','p99'])el(key).textContent=`${fmt(lat[key])} ms`;el('pmax').textContent=`${fmt(lat.max)} ms`;el('status-total').textContent=`${fmt(lat.samples)} samples`;
 const bars=el('status-bars');bars.replaceChildren();const total=Math.max(1,Number(req.total||0));for(const key of ['1xx','2xx','3xx','4xx','5xx']){const count=Number(classes[key]||0),row=document.createElement('div');row.className=`status-row${key==='4xx'?' warn':key==='5xx'?' bad':''}`;const label=document.createElement('span');label.textContent=key;const bar=document.createElement('span');bar.className='bar';const fill=document.createElement('i');fill.style.width=`${count/total*100}%`;bar.appendChild(fill);const value=document.createElement('strong');value.textContent=fmt(count);row.append(label,bar,value);bars.appendChild(row);}
 const routeMetrics=indexed(metrics.routes,'id');const routeRows=(data.routes||[]).map(route=>{const m=routeMetrics.get(route.id)||{},tr=document.createElement('tr');for(const value of [route.id,route.host,route.path_prefix,fmt(route.upstream_count),fmt(m.requests),fmt(m.errors)])td(tr,value);return tr;});const unmatched=routeMetrics.get('unmatched');if(unmatched){const tr=document.createElement('tr');for(const value of ['unmatched','—','—','—',fmt(unmatched.requests),fmt(unmatched.errors)])td(tr,value);routeRows.push(tr);}renderTable(el('routes-body'),routeRows,'尚无路由数据');
 const upMetrics=new Map((metrics.upstreams||[]).map(x=>[`${x.address}:${x.port}`,x]));const upRows=(data.upstreams||[]).map(up=>{const m=upMetrics.get(`${up.address}:${up.port}`)||{},tr=document.createElement('tr');td(tr,up.id);td(tr,`${up.address}:${up.port}`);const health=document.createElement('td'),pill=document.createElement('span');pill.className=`pill${up.healthy?'':' down'}`;pill.textContent=up.healthy?'HEALTHY':'DOWN';health.appendChild(pill);tr.appendChild(health);for(const value of [m.attempts,m.successes,m.failures,m.timeouts])td(tr,fmt(value));return tr;});renderTable(el('upstreams-body'),upRows,'尚无上游数据');
 const settings=metrics.settings||{};el('slow-rule').textContent=`≥ ${fmt(settings.slow_request_threshold_ms)} ms，最近 ${fmt(settings.slow_request_limit)} 条`;const slow=el('slow-list');slow.replaceChildren();for(const item of [...(metrics.slow_requests||[])].reverse()){const upstream=item.upstream?`${item.upstream.id} · ${item.upstream.address}:${item.upstream.port}`:'无上游';slow.appendChild(eventNode(`${item.method} ${item.path} · ${item.latency_ms} ms`,`路由 ${item.route} · ${upstream} · HTTP ${item.status} · 尝试 ${item.attempts}`,item.time,false));}if(!slow.children.length)slow.appendChild(eventNode('暂时没有慢请求','超过阈值的请求会显示在这里','',false));const errors=el('error-list');errors.replaceChildren();for(const item of [...(metrics.recent_errors||[])].reverse())errors.appendChild(eventNode(item.category,item.detail,item.time,true));if(!errors.children.length)errors.appendChild(eventNode('暂时没有错误','代理错误与管理错误摘要会显示在这里','',false));return Math.max(250,Number(settings.refresh_interval_ms||1000));}
async function refresh(){let delay=1000;try{const response=await fetch('/api/dashboard',{cache:'no-store'});if(!response.ok)throw new Error(`HTTP ${response.status}`);delay=render(await response.json());}catch(error){el('service-state').textContent='数据不可用';el('live-dot').className='bad';el('updated-at').textContent=String(error);}setTimeout(refresh,delay);}refresh();
)js";

std::system_error system_error_from_errno(const char* operation)
{
    return {errno, std::generic_category(), operation};
}

std::string make_http_response(
    int status,
    std::string_view reason,
    std::string_view content_type,
    std::string body,
    bool head_only,
    bool no_store = false)
{
    std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
        "\r\nContent-Type: " + std::string(content_type) +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; connect-src 'self'; "
        "style-src 'self'; script-src 'self'; frame-ancestors 'none'\r\n";
    response += no_store
        ? "Cache-Control: no-store\r\n\r\n"
        : "Cache-Control: public, max-age=300\r\n\r\n";
    if (!head_only) {
        response += std::move(body);
    }
    return response;
}

class DashboardConnection final : public edgegate::net::EventHandler {
public:
    DashboardConnection(
        edgegate::net::UniqueFd socket,
        DashboardSnapshotHandler handler)
        : socket_(std::move(socket)), handler_(std::move(handler))
    {
    }

    int fd() const noexcept override { return socket_.get(); }
    std::uint32_t interests() const noexcept override
    {
        return response_.empty() ? EPOLLIN : EPOLLOUT;
    }

    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override
    {
        if ((events & EPOLLERR) != 0U) {
            static_cast<void>(loop.remove(fd()));
            return;
        }
        if (response_.empty() && (events & EPOLLIN) != 0U) {
            read_request(loop);
        }
        if (!response_.empty() && (events & EPOLLOUT) != 0U) {
            write_response(loop);
            return;
        }
        if ((events & EPOLLHUP) != 0U && response_.empty()) {
            static_cast<void>(loop.remove(fd()));
        }
    }

private:
    void prepare_response(edgegate::net::EventLoop& loop) noexcept
    {
        try {
            const std::size_t line_end = request_.find("\r\n");
            if (line_end == std::string::npos) {
                response_ = make_http_response(
                    400, "Bad Request", "text/plain; charset=utf-8",
                    "400 Bad Request\n", false, true);
            } else {
                const std::string_view line(request_.data(), line_end);
                const std::size_t first = line.find(' ');
                const std::size_t second = first == std::string_view::npos
                    ? std::string_view::npos : line.find(' ', first + 1);
                if (first == std::string_view::npos ||
                    second == std::string_view::npos ||
                    line.find(' ', second + 1) != std::string_view::npos) {
                    response_ = make_http_response(
                        400, "Bad Request", "text/plain; charset=utf-8",
                        "400 Bad Request\n", false, true);
                } else {
                    const std::string_view method = line.substr(0, first);
                    std::string_view target = line.substr(first + 1, second - first - 1);
                    const std::string_view version = line.substr(second + 1);
                    const bool head = method == "HEAD";
                    if ((method != "GET" && !head) ||
                        (version != "HTTP/1.1" && version != "HTTP/1.0") ||
                        target.empty() || target.front() != '/') {
                        response_ = make_http_response(
                            method != "GET" && !head ? 405 : 400,
                            method != "GET" && !head ? "Method Not Allowed" : "Bad Request",
                            "text/plain; charset=utf-8",
                            method != "GET" && !head
                                ? "405 Method Not Allowed\n" : "400 Bad Request\n",
                            head, true);
                    } else {
                        target = target.substr(0, target.find('?'));
                        route_request(target, head);
                    }
                }
            }
            static_cast<void>(loop.modify(fd(), EPOLLOUT));
        } catch (...) {
            response_ = make_http_response(
                500, "Internal Server Error", "text/plain; charset=utf-8",
                "500 Internal Server Error\n", false, true);
            static_cast<void>(loop.modify(fd(), EPOLLOUT));
        }
    }

    void route_request(std::string_view target, bool head)
    {
        if (target == "/" || target == "/index.html") {
            response_ = make_http_response(
                200, "OK", "text/html; charset=utf-8",
                std::string(kIndexHtml), head);
        } else if (target == "/app.css") {
            response_ = make_http_response(
                200, "OK", "text/css; charset=utf-8",
                std::string(kAppCss), head);
        } else if (target == "/app.js") {
            response_ = make_http_response(
                200, "OK", "text/javascript; charset=utf-8",
                std::string(kAppJs), head);
        } else if (target == "/api/dashboard") {
            response_ = make_http_response(
                200, "OK", "application/json; charset=utf-8",
                handler_().dump(), head, true);
        } else {
            response_ = make_http_response(
                404, "Not Found", "text/plain; charset=utf-8",
                "404 Not Found\n", head, true);
        }
    }

    void read_request(edgegate::net::EventLoop& loop) noexcept
    {
        std::array<char, 2048> bytes{};
        for (;;) {
            const ssize_t received = ::recv(fd(), bytes.data(), bytes.size(), 0);
            if (received > 0) {
                request_.append(bytes.data(), static_cast<std::size_t>(received));
                if (request_.size() > kMaximumRequestBytes) {
                    response_ = make_http_response(
                        413, "Payload Too Large", "text/plain; charset=utf-8",
                        "413 Payload Too Large\n", false, true);
                    static_cast<void>(loop.modify(fd(), EPOLLOUT));
                    return;
                }
                if (request_.find("\r\n\r\n") != std::string::npos) {
                    prepare_response(loop);
                    return;
                }
                continue;
            }
            if (received == 0) {
                static_cast<void>(loop.remove(fd()));
                return;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            static_cast<void>(loop.remove(fd()));
            return;
        }
    }

    void write_response(edgegate::net::EventLoop& loop) noexcept
    {
        while (response_offset_ < response_.size()) {
            const ssize_t sent = ::send(
                fd(), response_.data() + response_offset_,
                response_.size() - response_offset_, MSG_NOSIGNAL);
            if (sent > 0) {
                response_offset_ += static_cast<std::size_t>(sent);
            } else if (sent == -1 && errno == EINTR) {
                continue;
            } else if (sent == -1 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            } else {
                static_cast<void>(loop.remove(fd()));
                return;
            }
        }
        static_cast<void>(loop.remove(fd()));
    }

    edgegate::net::UniqueFd socket_;
    DashboardSnapshotHandler handler_;
    std::string request_;
    std::string response_;
    std::size_t response_offset_{0};
};

class DashboardListener final : public edgegate::net::EventHandler {
public:
    DashboardListener(
        edgegate::net::UniqueFd socket,
        DashboardSnapshotHandler handler)
        : socket_(std::move(socket)), handler_(std::move(handler))
    {
    }

    int fd() const noexcept override { return socket_.get(); }
    std::uint32_t interests() const noexcept override { return EPOLLIN; }

    void on_event(
        edgegate::net::EventLoop& loop,
        std::uint32_t events) noexcept override
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
            return;
        }
        for (;;) {
            edgegate::net::UniqueFd connection(::accept4(
                socket_.get(), nullptr, nullptr,
                SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!connection) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            try {
                loop.add(std::make_unique<DashboardConnection>(
                    std::move(connection), handler_));
            } catch (...) {
            }
        }
    }

private:
    edgegate::net::UniqueFd socket_;
    DashboardSnapshotHandler handler_;
};

} // namespace

std::unique_ptr<edgegate::net::EventHandler> make_dashboard_listener(
    const std::string& address,
    std::uint16_t port,
    DashboardSnapshotHandler handler,
    std::uint16_t& actual_port)
{
    if (!handler) {
        throw std::invalid_argument("dashboard snapshot handler is required");
    }
    edgegate::net::UniqueFd socket(::socket(
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket) {
        throw system_error_from_errno("socket dashboard");
    }
    int reuse = 1;
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse)) == -1) {
        throw system_error_from_errno("setsockopt dashboard SO_REUSEADDR");
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        throw std::invalid_argument("dashboard address must be numeric IPv4");
    }
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint),
               sizeof(endpoint)) == -1) {
        throw system_error_from_errno("bind dashboard");
    }
    if (::listen(socket.get(), kDashboardBacklog) == -1) {
        throw system_error_from_errno("listen dashboard");
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&bound),
                      &length) == -1) {
        throw system_error_from_errno("getsockname dashboard");
    }
    actual_port = ntohs(bound.sin_port);
    return std::make_unique<DashboardListener>(
        std::move(socket), std::move(handler));
}

} // namespace edgegate::runtime
// AI-CODE-END: S9-DASHBOARD-SERVER-IMPLEMENTATION
