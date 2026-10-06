#include "support.hpp"

namespace {

using namespace onedrive::test::config;

int test_proxy() {
    ConfigFixture fixture;
    const auto& path = fixture.path;
    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"socks5h://127.0.0.1:1080\"\n";
    }
    try {
        const auto socks_proxy = onedrive::config::Config::load(path);
        if (socks_proxy.proxy.url !=
            std::optional<std::string>{"socks5h://127.0.0.1:1080"}) {
            std::filesystem::remove(path);
            std::cerr << "SOCKS5H proxy URL was not parsed correctly\n";
            return EXIT_FAILURE;
        }
    } catch (const std::runtime_error& error) {
        std::filesystem::remove(path);
        std::cerr << "SOCKS5H proxy URL was rejected: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    constexpr std::array proxy_auth_modes{
        std::pair{"auto", onedrive::http::ProxyAuth::automatic},
        std::pair{"basic", onedrive::http::ProxyAuth::basic},
        std::pair{"digest", onedrive::http::ProxyAuth::digest},
        std::pair{"ntlm", onedrive::http::ProxyAuth::ntlm},
        std::pair{"negotiate", onedrive::http::ProxyAuth::negotiate},
    };
    for (const auto& [name, expected] : proxy_auth_modes) {
        {
            std::ofstream output{path};
            output << "config_version = 2\n"
                   << "[proxy]\n"
                   << "url = \"https://proxy.example.test\"\n"
                   << "auth = \"" << name << "\"\n";
        }
        try {
            const auto proxy_auth = onedrive::config::Config::load(path);
            if (proxy_auth.proxy.auth != expected) {
                std::filesystem::remove(path);
                std::cerr << "proxy authentication mode was parsed "
                             "incorrectly\n";
                return EXIT_FAILURE;
            }
        } catch (const std::runtime_error& error) {
            std::filesystem::remove(path);
            std::cerr << "valid proxy authentication mode was rejected: "
                      << error.what() << '\n';
            return EXIT_FAILURE;
        }
    }

    const auto check_enum_modes = [&](std::string_view section,
                                      std::string_view key,
                                      const auto& modes,
                                      auto extract) {
        for (const auto& [name, expected] : modes) {
            {
                std::ofstream output{path};
                output << "config_version = 2\n"
                       << '[' << section << "]\n"
                       << key << " = \"" << name << "\"\n";
            }
            try {
                const auto parsed = onedrive::config::Config::load(path);
                if (extract(parsed) != expected) {
                    std::cerr << "configuration enum '" << section << '.' << key
                              << "' was parsed incorrectly\n";
                    return false;
                }
            } catch (const std::runtime_error& error) {
                std::cerr << "valid configuration enum '" << section << '.'
                          << key << "' was rejected: " << error.what() << '\n';
                return false;
            }
        }
        return true;
    };
    constexpr std::array filesystem_metadata_modes{
        std::pair{"auto", onedrive::config::FilesystemMetadataMode::automatic},
        std::pair{"xattr", onedrive::config::FilesystemMetadataMode::xattr},
        std::pair{
            "database", onedrive::config::FilesystemMetadataMode::database
        },
    };
    constexpr std::array http_versions{
        std::pair{"auto", onedrive::http::HttpVersion::automatic},
        std::pair{"1.1", onedrive::http::HttpVersion::http_1_1},
        std::pair{"2", onedrive::http::HttpVersion::http_2},
    };
    constexpr std::array ip_versions{
        std::pair{"auto", onedrive::http::IpVersion::automatic},
        std::pair{"4", onedrive::http::IpVersion::ipv4},
        std::pair{"6", onedrive::http::IpVersion::ipv6},
    };
    constexpr std::array download_validation_modes{
        std::pair{"strict", onedrive::config::DownloadValidationMode::strict},
        std::pair{"relaxed", onedrive::config::DownloadValidationMode::relaxed},
    };
    constexpr std::array transfer_orders{
        std::pair{"default", onedrive::config::TransferOrder::default_order},
        std::pair{"size_asc", onedrive::config::TransferOrder::size_ascending},
        std::pair{"size_dsc", onedrive::config::TransferOrder::size_descending},
        std::pair{"name_asc", onedrive::config::TransferOrder::name_ascending},
        std::pair{"name_dsc", onedrive::config::TransferOrder::name_descending},
    };
    constexpr std::array permission_modes{
        std::pair{
            "private", onedrive::config::SyncPermissionsMode::private_access
        },
        std::pair{"umask", onedrive::config::SyncPermissionsMode::umask},
    };
    constexpr std::array conflict_policies{
        std::pair{"block", onedrive::config::LocalConflictPolicy::block},
        std::pair{"backup", onedrive::config::LocalConflictPolicy::backup},
    };
    if (!check_enum_modes(
            "filesystem",
            "metadata",
            filesystem_metadata_modes,
            [](const auto& config) { return config.filesystem_metadata; }
        ) ||
        !check_enum_modes(
            "transfer",
            "http_version",
            http_versions,
            [](const auto& config) {
                return config.transfer_transport.http_version;
            }
        ) ||
        !check_enum_modes(
            "transfer",
            "ip_version",
            ip_versions,
            [](const auto& config) {
                return config.transfer_transport.ip_version;
            }
        ) ||
        !check_enum_modes(
            "download",
            "validation",
            download_validation_modes,
            [](const auto& config) { return config.download_validation; }
        ) ||
        !check_enum_modes(
            "transfer",
            "order",
            transfer_orders,
            [](const auto& config) { return config.transfer_order; }
        ) ||
        !check_enum_modes(
            "sync",
            "permissions",
            permission_modes,
            [](const auto& config) { return config.sync_permissions; }
        ) ||
        !check_enum_modes(
            "sync",
            "local_conflict",
            conflict_policies,
            [](const auto& config) { return config.local_conflict; }
        )) {
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test\"\n"
               << "no_proxy = []\n";
    }
    try {
        const auto empty_no_proxy = onedrive::config::Config::load(path);
        if (!empty_no_proxy.proxy.no_proxy ||
            !empty_no_proxy.proxy.no_proxy->empty()) {
            std::filesystem::remove(path);
            std::cerr << "empty proxy bypass list was not preserved\n";
            return EXIT_FAILURE;
        }
    } catch (const std::runtime_error& error) {
        std::filesystem::remove(path);
        std::cerr << "empty proxy bypass list was rejected: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test\"\n"
               << "no_proxy = [\"localhost,example.test\"]\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "ambiguous proxy bypass entry was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.no_proxy")) {
            std::filesystem::remove(path);
            std::cerr << "invalid proxy bypass entry reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test\"\n"
               << "auth = \"oauth\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unsupported proxy authentication mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.auth")) {
            std::filesystem::remove(path);
            std::cerr << "invalid proxy auth mode reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"socks5h://127.0.0.1:1080\"\n"
               << "password_file = \"proxy-password\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "proxy password without username was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.username")) {
            std::filesystem::remove(path);
            std::cerr << "missing proxy username reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"socks5h://127.0.0.1:1080\"\n"
               << "ca_file = \"proxy-ca.pem\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "proxy CA file with non-HTTPS URL was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.ca_file")) {
            std::filesystem::remove(path);
            std::cerr << "invalid proxy CA file reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"https://proxy.example.test:8443\"\n";
    }
    try {
        const auto https_proxy = onedrive::config::Config::load(path);
        if (https_proxy.proxy.url !=
            std::optional<std::string>{"https://proxy.example.test:8443"}) {
            std::filesystem::remove(path);
            std::cerr << "HTTPS proxy URL was not parsed correctly\n";
            return EXIT_FAILURE;
        }
    } catch (const std::runtime_error& error) {
        std::filesystem::remove(path);
        std::cerr << "HTTPS proxy URL was rejected: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[proxy]\n"
               << "url = \"ftp://proxy.example.test:21\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unsupported proxy URL was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("proxy.url")) {
            std::filesystem::remove(path);
            std::cerr << "unsupported proxy URL reported wrong error\n";
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_proxy();
}
