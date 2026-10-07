#include "http/proxy.hpp"

#include "util/private_file.hpp"

#include <stdexcept>
#include <sys/stat.h>

namespace onedrive::http::detail {

std::string read_proxy_password(const std::filesystem::path& path) {
    std::string password = onedrive::util::read_private_file(
        path,
        "proxy password",
        {
            .required_owner = std::nullopt,
            .exact_permissions = std::nullopt,
            .forbidden_permissions = S_IRWXG | S_IRWXO,
            .permission_requirement =
                "must not grant group or other permissions",
        }
    );
    if (password.ends_with('\n')) {
        password.pop_back();
        if (password.ends_with('\r')) {
            password.pop_back();
        }
    }
    if (password.empty()) {
        throw std::runtime_error(
            "proxy password file must not be empty: " + path.string()
        );
    }
    if (password.contains('\0')) {
        throw std::runtime_error(
            "proxy password file must not contain NUL bytes: " + path.string()
        );
    }
    return password;
}

std::string join_proxy_bypass_list(const std::vector<std::string>& entries) {
    std::string result;
    for (const auto& entry : entries) {
        if (!result.empty()) {
            result.push_back(',');
        }
        result += entry;
    }
    return result;
}

} // namespace onedrive::http::detail
