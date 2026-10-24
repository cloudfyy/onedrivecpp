#pragma once

#include "onedrive/app/factory.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace onedrive::app::detail {

struct InspectionStore {
    account::DriveIdentity identity;
    std::filesystem::path sync_root;
    std::filesystem::path database_path;
    std::unique_ptr<storage::ItemStore> items;
};

InspectionStore open_inspection_store(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    account::DriveIdentity identity
);
InspectionStore open_inspection_store(
    const config::Config& config, const RuntimeFactory& runtime_factory
);

bool path_is_within(
    const std::filesystem::path& root, const std::filesystem::path& path
);
std::uint64_t checked_add(std::uint64_t total, std::uint64_t value);

enum class LocalFileStatus {
    ok,
    missing,
    modified,
    type_changed,
    outside_root,
};

struct InspectedFile {
    const storage::ItemState* item;
    LocalFileStatus status;
    std::optional<std::uint64_t> actual_size;
};

std::string_view status_name(LocalFileStatus status);
InspectedFile
inspect_file(const std::filesystem::path& root, const storage::ItemState& item);

std::string normalized_inspection_path(std::string_view path);
bool path_matches_filter(std::string_view path, std::string_view filter);

enum class PartialStatus {
    resumable,
    missing,
    type_changed,
    outside_root,
    path_mismatch,
    invalid_checkpoint,
};

struct InspectedPartial {
    const storage::PartialDownload* partial;
    PartialStatus status;
    std::optional<std::uint64_t> actual_size;
};

std::string_view status_name(PartialStatus status);
InspectedPartial inspect_partial(
    const std::filesystem::path& root, const storage::PartialDownload& partial
);

} // namespace onedrive::app::detail
