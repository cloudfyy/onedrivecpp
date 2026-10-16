#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace onedrive::logging {

enum class Severity {
    trace,
    debug,
    information,
    warning,
    error,
    critical,
};

using MessageSink = std::function<void(Severity, std::string_view)>;

// Structured business events already reach the UI through its observer.
// Retain ordinary log sinks without echoing through MessageSink.
void write_event(Severity severity, std::string_view message);

struct Options {
    std::string level{"info"};
    std::optional<std::filesystem::path> file;
    MessageSink message_sink;
};

class Session {
public:
    explicit Session(const Options& options);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
};

}  // namespace onedrive::logging
