#pragma once

// AI-CODE-BEGIN: S6-HEADER-REWRITER-API
#include "edgegate/http/request_parser.h"
#include "edgegate/http/response_parser.h"

#include <string>
#include <string_view>

namespace edgegate::http {

/*
 * 重新生成发给上游的请求 Header：保留端到端信息，删除只属于客户端
 * 连接的 Header，并用真实连接信息生成 X-Forwarded-*。
 */
[[nodiscard]] std::string rewrite_request_for_upstream(
    const RequestParser& request,
    std::string_view client_address);

/*
 * 重新生成发给客户端的响应。当前代理已经完整缓存并解析响应，因此会把
 * chunked/关闭定界正文转换为明确的 Content-Length，隔离两侧连接语义。
 */
[[nodiscard]] std::string rewrite_response_for_client(
    const ResponseParser& response,
    bool close_client_connection);

// AI-CODE-BEGIN: S7-STREAMING-HEADER-REWRITER-API
// 只重新生成 Header，不附加 Body；Body 将由阶段7缓冲区边读边转发。
[[nodiscard]] std::string rewrite_request_head_for_upstream(
    const RequestParser& request,
    std::string_view client_address);

[[nodiscard]] std::string rewrite_response_head_for_client(
    const ResponseParser& response,
    bool close_client_connection);
// AI-CODE-END: S7-STREAMING-HEADER-REWRITER-API

} // namespace edgegate::http
// AI-CODE-END: S6-HEADER-REWRITER-API
