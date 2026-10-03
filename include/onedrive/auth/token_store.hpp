#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace onedrive::auth {

class TokenStore {
public:
    TokenStore() = default;
    virtual ~TokenStore() = default;
    TokenStore(const TokenStore&) = delete;
    TokenStore& operator=(const TokenStore&) = delete;
    TokenStore(TokenStore&&) = delete;
    TokenStore& operator=(TokenStore&&) = delete;

    [[nodiscard]] virtual std::optional<std::string> load_refresh_token() const = 0;
    virtual void save_refresh_token(const std::string& refresh_token) const = 0;
    [[nodiscard]] virtual bool remove_refresh_token() const = 0;
    [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
};

class FileTokenStore final : public TokenStore {
public:
    explicit FileTokenStore(const std::filesystem::path& state_directory);

    [[nodiscard]] std::optional<std::string> load_refresh_token() const override;
    void save_refresh_token(const std::string& refresh_token) const override;
    [[nodiscard]] bool remove_refresh_token() const override;
    [[nodiscard]] const std::filesystem::path& path() const noexcept override;

private:
    std::filesystem::path path_;
};

}  // namespace onedrive::auth
