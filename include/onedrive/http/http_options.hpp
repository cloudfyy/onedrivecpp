#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace onedrive::http {

enum class HttpVersion {
    automatic,
    http_1_1,
    http_2,
};

enum class IpVersion {
    automatic,
    ipv4,
    ipv6,
};

enum class ProxyAuth {
    automatic,
    basic,
    digest,
    ntlm,
    negotiate,
};

struct ProxyOptions {
    std::optional<std::string> url;
    std::optional<std::vector<std::string>> no_proxy;
    std::optional<std::string> username;
    std::optional<std::filesystem::path> password_file;
    ProxyAuth auth{ProxyAuth::automatic};
    std::optional<std::filesystem::path> ca_file;

    bool operator==(const ProxyOptions&) const = default;
};

struct TransferTransportOptions {
    std::chrono::seconds connect_timeout{30};
    std::chrono::seconds operation_timeout{std::chrono::hours{1}};
    std::chrono::seconds low_speed_timeout{60};
    std::uint64_t low_speed_limit_bytes_per_second{1};
    HttpVersion http_version{HttpVersion::automatic};
    IpVersion ip_version{IpVersion::automatic};

    bool operator==(const TransferTransportOptions&) const = default;
};

struct DownloadTransportOptions {
    TransferTransportOptions transfer;
    std::uint64_t maximum_receive_speed_bytes_per_second{0};
    std::uint64_t maximum_total_receive_speed_bytes_per_second{0};

    bool operator==(const DownloadTransportOptions&) const = default;
};

}  // namespace onedrive::http
