#pragma once

#include <chrono>
#include <cstdint>

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

struct DownloadTransportOptions {
    std::chrono::seconds connect_timeout{30};
    std::chrono::seconds operation_timeout{std::chrono::hours{1}};
    std::chrono::seconds low_speed_timeout{60};
    std::uint64_t low_speed_limit_bytes_per_second{1};
    std::uint64_t maximum_receive_speed_bytes_per_second{0};
    HttpVersion http_version{HttpVersion::automatic};
    IpVersion ip_version{IpVersion::automatic};

    bool operator==(const DownloadTransportOptions&) const = default;
};

}  // namespace onedrive::http
