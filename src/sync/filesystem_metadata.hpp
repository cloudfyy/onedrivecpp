#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"

#include <filesystem>

namespace onedrive::sync::detail {

class FilesystemMetadata {
public:
    [[nodiscard]] static FilesystemMetadata detect(
        config::FilesystemMetadataMode mode,
        const std::filesystem::path& root
    );

    [[nodiscard]] bool uses_xattrs() const noexcept;
    void write_remote_identity(
        const graph::RemoteItem& item,
        const std::filesystem::path& path
    ) const;

private:
    explicit FilesystemMetadata(bool use_xattrs);

    bool use_xattrs_{false};
};

}  // namespace onedrive::sync::detail
