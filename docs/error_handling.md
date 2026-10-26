# Error Handling

Externally callable server and raw-pipeline mutations return `video_server::VideoServerResult`. A result contains:

- `ok()`: whether the operation succeeded
- `error()`: a stable `VideoServerError` value
- `message()`: a human-readable diagnostic
- implicit boolean conversion for compatibility with existing `if (!operation())` call sites

Use the typed code for program behavior and the message for logs:

```cpp
const auto result = server->push_frame("camera-1", frame);
if (!result)
{
    if (result.error() == video_server::VideoServerError::NotFound)
    {
        // Re-register or stop this producer.
    }
    std::cerr << video_server::to_string(result.error())
              << ": " << result.message() << '\n';
}
```

The status-returning public operations include stream registration/removal, raw-frame and encoded-unit submission,
runtime output configuration, server startup/progression, managed stream-group progression, and raw encoder pipeline
startup/frame submission.

`VideoServerError::None` represents success. Other codes distinguish invalid arguments and configuration, missing or
duplicate streams, lifecycle and execution-mode errors, contention, unsupported formats/codecs, backend and transform
failures, encoder failures, and rejected encoded-output sinks.

The optional `std::string *error_message` arguments on raw-pipeline methods remain available for source compatibility;
new code should normally use `VideoServerResult::message()`.
