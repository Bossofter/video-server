#pragma once

#include <string>
#include <utility>

namespace video_server
{

    /** @brief Stable error codes returned by externally callable video-server operations. */
    enum class VideoServerError
    {
        None = 0,
        InvalidArgument,
        InvalidConfiguration,
        AlreadyExists,
        NotFound,
        AlreadyStarted,
        NotStarted,
        WrongExecutionMode,
        ResourceBusy,
        Unsupported,
        BackendFailure,
        TransformFailed,
        EncoderFailure,
        SinkRejected,
        InternalError,
    };

    /** @brief Returns the stable symbolic name for an error code. */
    inline const char *to_string(VideoServerError error) noexcept
    {
        switch (error)
        {
        case VideoServerError::None:
            return "none";
        case VideoServerError::InvalidArgument:
            return "invalid_argument";
        case VideoServerError::InvalidConfiguration:
            return "invalid_configuration";
        case VideoServerError::AlreadyExists:
            return "already_exists";
        case VideoServerError::NotFound:
            return "not_found";
        case VideoServerError::AlreadyStarted:
            return "already_started";
        case VideoServerError::NotStarted:
            return "not_started";
        case VideoServerError::WrongExecutionMode:
            return "wrong_execution_mode";
        case VideoServerError::ResourceBusy:
            return "resource_busy";
        case VideoServerError::Unsupported:
            return "unsupported";
        case VideoServerError::BackendFailure:
            return "backend_failure";
        case VideoServerError::TransformFailed:
            return "transform_failed";
        case VideoServerError::EncoderFailure:
            return "encoder_failure";
        case VideoServerError::SinkRejected:
            return "sink_rejected";
        case VideoServerError::InternalError:
            return "internal_error";
        }
        return "unknown";
    }

    /**
     * @brief Result of an operation that either succeeds or returns a typed error and diagnostic.
     *
     * The implicit boolean conversion preserves existing `if (!operation())` call sites while
     * allowing callers to branch on `error()` and report `message()`.
     */
    class [[nodiscard]] VideoServerResult
    {
    public:
        static VideoServerResult success() { return {}; }

        static VideoServerResult failure(VideoServerError error, std::string message = {})
        {
            return VideoServerResult(error, std::move(message));
        }

        bool ok() const noexcept { return error_ == VideoServerError::None; }
        operator bool() const noexcept { return ok(); }
        VideoServerError error() const noexcept { return error_; }
        const std::string &message() const noexcept { return message_; }

    private:
        VideoServerResult() = default;
        VideoServerResult(VideoServerError error, std::string message)
            : error_(error), message_(std::move(message))
        {
        }

        VideoServerError error_{VideoServerError::None};
        std::string message_;
    };

} // namespace video_server
