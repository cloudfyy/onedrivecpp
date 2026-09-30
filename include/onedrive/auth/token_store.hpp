#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::auth {

class TokenStore {
public:
    explicit TokenStore(std::filesystem::path state_directory);

    [[nodiscard]] std::optional<std::string> load_refresh_token() const;
    void save_refresh_token(const std::string& refresh_token) const;
    [[nodiscard]] bool remove_refresh_token() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

}  // namespace onedrive::auth
