#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <proxy/proxy.h>
#include <string>
#include <utility>

namespace onedrive::auth {

PRO_DEF_MEM_DISPATCH(TokenLoadDispatch, load_refresh_token);
PRO_DEF_MEM_DISPATCH(TokenSaveDispatch, save_refresh_token);
PRO_DEF_MEM_DISPATCH(TokenRemoveDispatch, remove_refresh_token);
PRO_DEF_MEM_DISPATCH(TokenPathDispatch, path);

struct TokenStoreFacade : pro::facade_builder
    ::add_convention<
        TokenLoadDispatch,
        std::optional<std::string>() const
    >
    ::add_convention<
        TokenSaveDispatch,
        void(const std::string&) const
    >
    ::add_convention<TokenRemoveDispatch, bool() const>
    ::add_convention<
        TokenPathDispatch,
        const std::filesystem::path&() const noexcept
    >
    ::build {};

class TokenStore {
public:
    template <typename Implementation, typename... Args>
    explicit TokenStore(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              TokenStoreFacade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit TokenStore(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    explicit TokenStore(Implementation& implementation)
        : implementation_{&implementation} {}

    ~TokenStore() = default;
    TokenStore(const TokenStore&) = delete;
    TokenStore& operator=(const TokenStore&) = delete;
    TokenStore(TokenStore&&) noexcept = default;
    TokenStore& operator=(TokenStore&&) noexcept = default;

    [[nodiscard]] std::optional<std::string> load_refresh_token() const {
        return implementation_->load_refresh_token();
    }

    void save_refresh_token(const std::string& refresh_token) const {
        implementation_->save_refresh_token(refresh_token);
    }

    [[nodiscard]] bool remove_refresh_token() const {
        return implementation_->remove_refresh_token();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return implementation_->path();
    }

private:
    pro::proxy<TokenStoreFacade> implementation_;
};

class FileTokenStore final {
public:
    explicit FileTokenStore(const std::filesystem::path& state_directory);

    [[nodiscard]] std::optional<std::string> load_refresh_token() const;
    void save_refresh_token(const std::string& refresh_token) const;
    [[nodiscard]] bool remove_refresh_token() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

}  // namespace onedrive::auth
