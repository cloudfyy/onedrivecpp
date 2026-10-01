#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::logging {

struct Options {
    std::string level{"info"};
    std::optional<std::filesystem::path> file;
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
