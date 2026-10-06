#pragma once

#include "onedrive/config/config.hpp"
#include "onedrive/graph/graph_client.hpp"

#include <filesystem>

namespace onedrive::sync::detail {

[[nodiscard]] bool xattr_error_is_unavailable(int error) noexcept;

class FilesystemMetadata {
public:
    [[nodiscard]] static FilesystemMetadata detect(
        config::FilesystemMetadataMode mode,
        const std::filesystem::path& root
    );
    [[nodiscard]] static FilesystemMetadata from_detected_support(
        config::FilesystemMetadataMode mode,
        bool xattrs_supported
    );

    [[nodiscard]] bool uses_xattrs() const noexcept;
    void write_remote_identity(
        const graph::RemoteItem& item,
        const std::filesystem::path& path
    ) const;

private:
    enum class MetadataStorage {
        database,
        database_with_xattrs,
    };

    explicit FilesystemMetadata(MetadataStorage storage);

    MetadataStorage storage_{MetadataStorage::database};
};

}  // namespace onedrive::sync::detail
