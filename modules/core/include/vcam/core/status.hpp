// =============================================================================
// status.hpp — error reporting without exceptions
//
// Every operation that can fail returns either:
//   * Status      — "ok" or an error code + human-readable message, or
//   * Result<T>   — a value of type T, or an error Status.
//
// Why not exceptions? Failures here are expected events (missing file,
// corrupted frame, busy device). Returning them explicitly keeps the control
// flow visible in the code and lets the caller decide what to do.
// (C++23 has std::expected; this project targets C++20, so we use this small
// equivalent.)
// =============================================================================
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace vcam {

enum class StatusCode {
    Ok = 0,
    InvalidArgument,   // caller passed a bad value (e.g. width 0, unknown pixel format)
    NotFound,          // file or device does not exist
    PermissionDenied,  // file or device exists but cannot be opened
    Unsupported,       // valid request that this build/version does not support
    InvalidData,       // input exists but its content is malformed or corrupted
    IoError,           // read/write failure from the OS
    DeviceError,       // V4L2 / virtual camera device failure
    ResourceExhausted, // out of memory / buffer budget exceeded
    Cancelled,         // stopped by the user (Ctrl+C)
    Internal,          // a bug: should never happen
};

// Short upper-case name, e.g. "NOT_FOUND". Useful in logs.
const char* status_code_name(StatusCode code);

class Status {
public:
    // Default-constructed Status means success.
    Status() = default;
    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

    static Status ok_status() { return Status(); }

    bool ok() const { return code_ == StatusCode::Ok; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

    // "NOT_FOUND: cannot open 'x.mp4'" — or "OK".
    std::string to_string() const;

private:
    StatusCode code_ = StatusCode::Ok;
    std::string message_;
};

// Holds either a value or an error. Typical use:
//
//     Result<Rational> fps = parse_rational("30");
//     if (!fps.ok()) { print(fps.status()); return; }
//     use(fps.value());
template <typename T>
class Result {
public:
    // Implicit on purpose: lets a function write `return value;` or `return status;`.
    Result(T value) : value_(std::move(value)) {}                       // NOLINT
    Result(Status error) : status_(std::move(error)) { assert(!status_.ok()); }  // NOLINT

    bool ok() const { return value_.has_value(); }
    const Status& status() const { return status_; }

    T& value() { assert(ok()); return *value_; }
    const T& value() const { assert(ok()); return *value_; }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    std::optional<T> value_;
    Status status_;  // stays Ok when a value is present
};

}  // namespace vcam
