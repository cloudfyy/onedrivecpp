#pragma once

#include "onedrive/proxy_service.hpp"

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

class TokenStore : private detail::ProxyService<TokenStoreFacade> {
    using Base = detail::ProxyService<TokenStoreFacade>;

public:
    using Base::Base;

    [[nodiscard]] std::optional<std::string> load_refresh_token() const {
        return implementation()->load_refresh_token();
    }

    void save_refresh_token(const std::string& refresh_token) const {
        implementation()->save_refresh_token(refresh_token);
    }

    [[nodiscard]] bool remove_refresh_token() const {
        return implementation()->remove_refresh_token();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return implementation()->path();
    }
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
