#pragma once

// AI-CODE-BEGIN: S3-HTTP-COMMON-TYPES
#include <string>

namespace edgegate::http {

enum class ParseStatus {
    kNeedMoreData,
    kMessageComplete,
    kError
};

struct HeaderField {
    std::string name;
    std::string value;
};

} // namespace edgegate::http
// AI-CODE-END: S3-HTTP-COMMON-TYPES
