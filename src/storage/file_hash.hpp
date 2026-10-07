#pragma once

#include "onedrive/util/file_hash.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace onedrive::storage::item_database_detail {

inline std::pair<std::string, std::string> serialize_file_hash(
    const std::optional<util::FileHash>& hash
) {
    if (!hash) {
        return {};
    }
    return {
        hash->algorithm == util::FileHashAlgorithm::sha256 ? "sha256"
                                                           : "quick_xor",
        hash->value,
    };
}

inline std::optional<util::FileHash> parse_file_hash(
    std::string algorithm, std::string value, std::string_view context
) {
    if (algorithm.empty() && value.empty()) {
        return std::nullopt;
    }
    if (value.empty() || (algorithm != "sha256" && algorithm != "quick_xor")) {
        throw std::runtime_error(
            std::string{context} + " contains invalid content hash metadata"
        );
    }
    return util::FileHash{
        .algorithm = algorithm == "sha256" ?
            util::FileHashAlgorithm::sha256 :
            util::FileHashAlgorithm::quick_xor,
        .value = std::move(value),
    };
}

} // namespace onedrive::storage::item_database_detail
