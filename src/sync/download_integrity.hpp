#pragma once

#include "onedrive/graph/graph_client.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace onedrive::sync::detail {

class DownloadIntegrityError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string quick_xor_hash(
    const std::filesystem::path& path
);
void verify_download_integrity(
    const std::filesystem::path& path,
    const graph::RemoteItem& item,
    const std::string& sha256_fingerprint
);

}  // namespace onedrive::sync::detail
