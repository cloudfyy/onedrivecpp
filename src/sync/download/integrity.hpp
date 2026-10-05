#pragma once

#include "onedrive/graph/graph_client.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace onedrive::sync::detail {

class DownloadIntegrityError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DownloadHashes {
    std::string sha256;
    std::string quick_xor;
};

class StreamingDownloadHasher final {
public:
    StreamingDownloadHasher();
    ~StreamingDownloadHasher();
    StreamingDownloadHasher(const StreamingDownloadHasher&) = delete;
    StreamingDownloadHasher& operator=(const StreamingDownloadHasher&) =
        delete;
    StreamingDownloadHasher(StreamingDownloadHasher&&) noexcept;
    StreamingDownloadHasher& operator=(StreamingDownloadHasher&&) noexcept;

    void update(
        std::uint64_t offset,
        std::span<const std::byte> data
    );
    [[nodiscard]] std::optional<DownloadHashes> finish(
        std::uint64_t expected_size
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string quick_xor_hash(
    const std::filesystem::path& path
);
void verify_download_integrity(
    const graph::RemoteItem& item,
    const DownloadHashes& hashes
);

}  // namespace onedrive::sync::detail
