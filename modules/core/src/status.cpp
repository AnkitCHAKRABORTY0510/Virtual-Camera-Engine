#include "vcam/core/status.hpp"

namespace vcam {

const char* status_code_name(StatusCode code) {
    switch (code) {
        case StatusCode::Ok:                return "OK";
        case StatusCode::InvalidArgument:   return "INVALID_ARGUMENT";
        case StatusCode::NotFound:          return "NOT_FOUND";
        case StatusCode::PermissionDenied:  return "PERMISSION_DENIED";
        case StatusCode::Unsupported:       return "UNSUPPORTED";
        case StatusCode::InvalidData:       return "INVALID_DATA";
        case StatusCode::IoError:           return "IO_ERROR";
        case StatusCode::DeviceError:       return "DEVICE_ERROR";
        case StatusCode::ResourceExhausted: return "RESOURCE_EXHAUSTED";
        case StatusCode::Cancelled:         return "CANCELLED";
        case StatusCode::Internal:          return "INTERNAL";
    }
    return "UNKNOWN";
}

std::string Status::to_string() const {
    if (ok()) {
        return "OK";
    }
    return std::string(status_code_name(code_)) + ": " + message_;
}

}  // namespace vcam
