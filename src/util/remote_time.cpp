#include "onedrive/util/remote_time.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace onedrive::util {
namespace {

[[noreturn]] void invalid_remote_modified_time(std::string_view timestamp) {
    throw std::runtime_error(
        "invalid Microsoft Graph modification time '" +
        std::string{timestamp} + "'"
    );
}

unsigned parse_decimal(
    std::string_view timestamp,
    std::size_t offset,
    std::size_t count
) {
    unsigned result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char character = timestamp[offset + index];
        if (character < '0' || character > '9') {
            invalid_remote_modified_time(timestamp);
        }
        result = result * 10U +
                 static_cast<unsigned>(character - '0');
    }
    return result;
}

}  // namespace

std::chrono::sys_time<std::chrono::nanoseconds>
parse_remote_modified_time(std::string_view timestamp) {
    if (timestamp.size() < 20 ||
        timestamp[4] != '-' ||
        timestamp[7] != '-' ||
        timestamp[10] != 'T' ||
        timestamp[13] != ':' ||
        timestamp[16] != ':') {
        invalid_remote_modified_time(timestamp);
    }

    const auto year = static_cast<int>(parse_decimal(timestamp, 0, 4));
    const auto month = parse_decimal(timestamp, 5, 2);
    const auto day = parse_decimal(timestamp, 8, 2);
    const auto hour = parse_decimal(timestamp, 11, 2);
    const auto minute = parse_decimal(timestamp, 14, 2);
    const auto second = parse_decimal(timestamp, 17, 2);
    std::size_t position = 19;
    std::chrono::nanoseconds fraction{};
    if (position < timestamp.size() && timestamp[position] == '.') {
        ++position;
        const auto fraction_begin = position;
        std::uint64_t nanoseconds = 0;
        while (position < timestamp.size() &&
               timestamp[position] >= '0' &&
               timestamp[position] <= '9') {
            if (position - fraction_begin >= 9) {
                invalid_remote_modified_time(timestamp);
            }
            nanoseconds = nanoseconds * 10U +
                          static_cast<unsigned>(timestamp[position] - '0');
            ++position;
        }
        const auto digits = position - fraction_begin;
        if (digits == 0) {
            invalid_remote_modified_time(timestamp);
        }
        for (std::size_t index = digits; index < 9; ++index) {
            nanoseconds *= 10U;
        }
        fraction = std::chrono::nanoseconds{nanoseconds};
    }
    if (position + 1 != timestamp.size() ||
        timestamp[position] != 'Z' ||
        hour > 23 || minute > 59 || second > 59) {
        invalid_remote_modified_time(timestamp);
    }

    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day},
    };
    if (!date.ok()) {
        invalid_remote_modified_time(timestamp);
    }
    return std::chrono::sys_days{date} +
           std::chrono::hours{hour} +
           std::chrono::minutes{minute} +
           std::chrono::seconds{second} +
           fraction;
}

}  // namespace onedrive::util
