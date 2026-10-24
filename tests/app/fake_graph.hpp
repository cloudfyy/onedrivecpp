#pragma once

#include "onedrive/graph/graph_client.hpp"
#include "support/sync.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace onedrive::test::app {

class FakeGraphClient final {
public:
    using IdentityOverride =
        std::function<onedrive::account::DriveIdentity(std::stop_token)>;
    using DeltaOverride = std::function<onedrive::graph::DeltaResult(
        const std::optional<std::string>&, std::stop_token
    )>;

    explicit FakeGraphClient(
        std::string configured_drive_id,
        bool empty_shared = false,
        bool empty_sites = false,
        bool include_configured_drive = false,
        IdentityOverride identity_override = {},
        DeltaOverride delta_override = {}
    )
        : configured_drive_id_{std::move(configured_drive_id)},
          empty_shared_{empty_shared},
          empty_sites_{empty_sites},
          include_configured_drive_{include_configured_drive},
          identity_override_{std::move(identity_override)},
          delta_override_{std::move(delta_override)} {
    }

    [[nodiscard]] onedrive::account::DriveIdentity
    drive_identity(std::stop_token token = {}) const {
        if (identity_override_) {
            return identity_override_(token);
        }
        auto identity =
            onedrive::test::test_drive_identity(configured_drive_id_);
        identity.drive_type = "business";
        return identity;
    }

    [[nodiscard]] std::vector<onedrive::graph::DriveInfo> list_drives() const {
        std::vector<onedrive::graph::DriveInfo> drives{
            {
                .id = "shared-drive-id",
                .name = "Shared Drive",
                .type = "business",
                .web_url = "https://example.test/shared",
                .owner = "Example Team",
                .quota = std::nullopt,
            },
        };
        if (include_configured_drive_) {
            drives.push_back(drive_info());
        }
        return drives;
    }

    [[nodiscard]] onedrive::graph::DriveInfo drive_info() const {
        return {
            .id = configured_drive_id_ == "me" ? "drive-id"
                                               : configured_drive_id_,
            .name = "Test Drive",
            .type = "business",
            .web_url = "https://example.test/drive",
            .owner = "Test User",
            .quota = onedrive::graph::DriveQuota{
                .total = 2'048,
                .used = 400,
                .remaining = 600,
                .deleted = 25,
                .state = "normal",
            },
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::SharedResource>
    list_shared_resources() const {
        if (empty_shared_) {
            return {};
        }
        return {
            {
                .name = "Shared Plan",
                .target_name = "Plan",
                .drive_id = "team-drive-id",
                .item_id = "shared-item-id",
                .web_url = "https://example.test/shared-plan",
                .owner = "Example Team",
                .local_path = {},
                .directory = true,
                .source = onedrive::graph::SharedResourceSource::shared_with_me,
            },
            {
                .name = "Team Shortcut",
                .target_name = "Team Files",
                .drive_id = "team-drive-id",
                .item_id = "shortcut-target-id",
                .web_url = "https://example.test/team-files",
                .owner = "Example Team",
                .local_path = "Team Shortcut",
                .directory = true,
                .source = onedrive::graph::SharedResourceSource::shortcut,
            },
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::SiteInfo>
    search_sites(const std::string& query) const {
        if (empty_sites_ || query != "Engineering") {
            return {};
        }
        return {
            {
                .id = "example.sharepoint.test,site-id,web-id",
                .name = "engineering",
                .display_name = "Engineering",
                .web_url = "https://example.sharepoint.test/engineering",
                .drives = {
                    {
                        .id = "library-drive-id",
                        .name = "Documents",
                        .type = "documentLibrary",
                        .web_url =
                            "https://example.sharepoint.test/engineering/docs",
                        .owner = "Engineering",
                        .quota = std::nullopt,
                    },
                },
            },
        };
    }

    [[nodiscard]] std::vector<onedrive::graph::RemoteItem> list_root() const {
        return {};
    }

    [[nodiscard]] onedrive::graph::NotificationChannel
    notification_channel() const {
        return {
            .notification_url = "https://notification.example.test/token",
            .expires_at =
                std::chrono::system_clock::now() + std::chrono::hours{1},
        };
    }

    void refresh_access_token() const {
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    item_by_path(const std::string& remote_path) const {
        return {
            .id = "single-file-id",
            .name = std::filesystem::path{remote_path}.filename().string(),
            .etag = "single-file-etag",
            .parent_id = "root-id",
            .remote_path = remote_path,
            .last_modified = "2026-10-02T00:00:00Z",
            .size = 42,
            .directory = false,
        };
    }
    [[nodiscard]] onedrive::graph::RemoteItem item_by_path(
        const std::string& remote_path,
        std::stop_token stop_token,
        std::span<const onedrive::graph::RemoteItem> = {}
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated path lookup cancellation"
            };
        }
        return item_by_path(remote_path);
    }

    [[nodiscard]] onedrive::graph::DeltaResult list_delta(
        const std::optional<std::string>& cursor,
        const onedrive::graph::DeltaProgress& progress,
        std::stop_token token,
        std::span<const onedrive::graph::RemoteItem> = {}
    ) const {
        if (delta_override_) {
            auto result = delta_override_(cursor, token);
            if (progress) {
                progress(
                    1,
                    result.changes.size(),
                    onedrive::util::ProgressState::completed
                );
            }
            return result;
        }
        if (progress) {
            progress(1, 1, onedrive::util::ProgressState::completed);
        }
        return {
            .changes =
                {
                    {
                        .id = "file-id",
                        .name = "notes.txt",
                        .etag = "file-etag",
                        .parent_id = "root-id",
                        .remote_path = "notes.txt",
                        .last_modified = "2026-10-02T00:00:00Z",
                        .size = 42,
                        .directory = false,
                    },
                },
            .delta_link = "https://graph.example.test/delta-token",
        };
    }

    void download_file(
        const std::string&,
        const std::string&,
        std::uint64_t,
        const std::filesystem::path& destination,
        std::uint64_t,
        std::stop_token,
        const onedrive::graph::DownloadProgress&,
        const onedrive::graph::DownloadCheckpoint& checkpoint,
        const onedrive::graph::DownloadData& data
    ) const {
        const std::string contents(42, 'x');
        std::ofstream output{destination, std::ios::binary};
        output << contents;
        if (data) {
            data(0, std::as_bytes(std::span{contents}));
        }
        if (checkpoint) {
            checkpoint(42);
        }
    }

    [[nodiscard]] onedrive::graph::RemoteItem upload_file(
        const std::string&,
        const std::optional<std::string>&,
        const std::string&,
        const std::filesystem::path&,
        const std::optional<onedrive::graph::UploadSession>&,
        const onedrive::graph::UploadCheckpoint&,
        std::stop_token,
        const onedrive::graph::UploadProgress& = {}
    ) const {
        throw std::logic_error{"upload was not expected"};
    }

    [[nodiscard]] onedrive::graph::RemoteItem
    create_directory(const std::string&) const {
        throw std::logic_error{"directory creation was not expected"};
    }
    [[nodiscard]] onedrive::graph::RemoteItem create_directory(
        const std::string& remote_path, std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated directory creation cancellation"
            };
        }
        return create_directory(remote_path);
    }

    void delete_item(const std::string&, const std::string&) const {
    }
    void delete_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item deletion cancellation"
            };
        }
        delete_item(remote_id, expected_etag);
    }
    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string&, const std::string&, const std::string&
    ) const {
        return {};
    }
    [[nodiscard]] onedrive::graph::RemoteItem move_item(
        const std::string& remote_id,
        const std::string& expected_etag,
        const std::string& destination_path,
        std::stop_token stop_token
    ) const {
        if (stop_token.stop_requested()) {
            throw onedrive::graph::RequestCancelledError{
                "simulated item move cancellation"
            };
        }
        return move_item(remote_id, expected_etag, destination_path);
    }

private:
    std::string configured_drive_id_;
    bool empty_shared_;
    bool empty_sites_;
    bool include_configured_drive_;
    IdentityOverride identity_override_;
    DeltaOverride delta_override_;
};

} // namespace onedrive::test::app
