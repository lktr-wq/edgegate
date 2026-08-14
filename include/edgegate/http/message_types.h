#pragma once

// AI-CODE-BEGIN: S3-HTTP-COMMON-TYPES
#include <string>

namespace edgegate::http {

enum class ParseStatus {// 解析工作目前进行到什么状态
    kNeedMoreData,
    kMessageComplete,
    kError
};
//请求行仅一行，由方法+url+版本构成(简述核心意图)
//请求行后，\r\n\r\n前为请求头header，由若干键值对构成，补充部分附加信息
//\r\n\r\n之后为请求体body(一般POST/PUT)才有，携带数据信息(不同于附加信息)
struct HeaderField {//保存一个 Header 的名称和值
    std::string name;
    std::string value;
};

} // namespace edgegate::http
// AI-CODE-END: S3-HTTP-COMMON-TYPES
