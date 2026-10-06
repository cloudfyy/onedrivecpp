#include "sync/filter/selective.hpp"

#include "util/ascii.hpp"
#include "onedrive/util/sha256.hpp"
#include "sync/filter/remote_path.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace onedrive::sync::detail {
namespace {

std::vector<std::string_view> split_path(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        segments.push_back(path.substr(begin, end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return segments;
}

bool wildcard_match(std::string_view pattern, std::string_view value) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t wildcard_index = std::string_view::npos;
    std::size_t wildcard_value_index = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            pattern[pattern_index] == value[value_index]) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == '*') {
            wildcard_index = pattern_index++;
            wildcard_value_index = value_index;
        } else if (wildcard_index != std::string_view::npos) {
            pattern_index = wildcard_index + 1;
            value_index = ++wildcard_value_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool valid_utf8(std::string_view value) {
    const auto continuation = [](unsigned char byte) {
        return byte >= 0x80 && byte <= 0xbf;
    };
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf && index + 1 < value.size() &&
            continuation(static_cast<unsigned char>(value[index + 1]))) {
            index += 2;
            continue;
        }
        if (index + 2 < value.size() &&
            ((first == 0xe0 &&
              static_cast<unsigned char>(value[index + 1]) >= 0xa0 &&
              static_cast<unsigned char>(value[index + 1]) <= 0xbf) ||
             (((first >= 0xe1 && first <= 0xec) ||
               (first >= 0xee && first <= 0xef)) &&
              continuation(static_cast<unsigned char>(value[index + 1]))) ||
             (first == 0xed &&
              static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
              static_cast<unsigned char>(value[index + 1]) <= 0x9f)) &&
            continuation(static_cast<unsigned char>(value[index + 2]))) {
            index += 3;
            continue;
        }
        if (index + 3 < value.size() &&
            ((first == 0xf0 &&
              static_cast<unsigned char>(value[index + 1]) >= 0x90 &&
              static_cast<unsigned char>(value[index + 1]) <= 0xbf) ||
             ((first >= 0xf1 && first <= 0xf3) &&
              continuation(static_cast<unsigned char>(value[index + 1]))) ||
             (first == 0xf4 &&
              static_cast<unsigned char>(value[index + 1]) >= 0x80 &&
              static_cast<unsigned char>(value[index + 1]) <= 0x8f)) &&
            continuation(static_cast<unsigned char>(value[index + 2])) &&
            continuation(static_cast<unsigned char>(value[index + 3]))) {
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool match_segments(
    const std::vector<std::string>& pattern,
    const std::vector<std::string_view>& path,
    std::size_t pattern_index,
    std::size_t path_index
) {
    if (pattern_index == pattern.size()) {
        return true;
    }
    if (pattern[pattern_index] == "**") {
        if (match_segments(pattern, path, pattern_index + 1, path_index)) {
            return true;
        }
        return path_index < path.size() &&
               match_segments(pattern, path, pattern_index, path_index + 1);
    }
    return path_index < path.size() &&
           wildcard_match(pattern[pattern_index], path[path_index]) &&
           match_segments(pattern, path, pattern_index + 1, path_index + 1);
}

bool dotfile_path(const std::vector<std::string_view>& segments) {
    return std::ranges::any_of(segments, [](std::string_view segment) {
        return segment.size() > 1 && segment.starts_with('.');
    });
}

bool nosync_marker(
    const std::filesystem::path& directory, std::string_view description
) {
    std::error_code error;
    const auto status =
        std::filesystem::symlink_status(directory / ".nosync", error);
    if (!error) {
        return std::filesystem::is_regular_file(status);
    }
    if (error == std::errc::no_such_file_or_directory) {
        return false;
    }
    throw std::runtime_error(
        "cannot inspect .nosync marker for '" + std::string{description} +
        "': " + error.message()
    );
}

std::vector<std::string>
discover_nosync_directories(const std::filesystem::path& root) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory) {
        return {};
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect synchronization root for .nosync markers: " +
            error.message()
        );
    }
    if (!std::filesystem::is_directory(root_status)) {
        return {};
    }
    if (nosync_marker(root, root.string())) {
        return {""};
    }

    std::vector<std::string> directories;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error
    };
    if (error) {
        throw std::runtime_error(
            "cannot scan synchronization root for .nosync markers: " +
            error.message()
        );
    }
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect .nosync candidate '" +
                iterator->path().string() + "': " + error.message()
            );
        }
        if (std::filesystem::is_directory(status)) {
            const auto relative =
                iterator->path().lexically_relative(root).generic_string();
            if (nosync_marker(iterator->path(), relative)) {
                directories.push_back(relative);
                iterator.disable_recursion_pending();
            }
        } else if (std::filesystem::is_symlink(status)) {
            iterator.disable_recursion_pending();
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error(
                "cannot continue .nosync marker scan: " + error.message()
            );
        }
    }
    std::ranges::sort(directories);
    return directories;
}

} // namespace

SyncList
SyncList::load(const std::filesystem::path& path, bool include_root_files) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error(
            "cannot read sync list '" + path.string() + "'"
        );
    }

    SyncList result;
    std::string canonical{"onedrive-cpp-sync-list-v1\n"};
    result.include_root_files_ = include_root_files;
    if (include_root_files) {
        canonical += "@sync-root-files\n";
    }
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = std::string{onedrive::util::trim_ascii_whitespace(line)};
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (!valid_utf8(line) || line.contains('\0')) {
            throw std::runtime_error(
                "invalid UTF-8 sync list rule at " + path.string() + ":" +
                std::to_string(line_number)
            );
        }

        Rule rule;
        if (line.starts_with('!') || line.starts_with('-')) {
            rule.exclude = true;
            line.erase(0, 1);
        }
        rule.anchored = line.starts_with('/');
        if (rule.anchored) {
            line.erase(0, 1);
        }
        rule.directory_only = line.ends_with('/');
        if (rule.directory_only) {
            line.pop_back();
        }
        if (line.empty() || line.contains('\\') || line.starts_with('/') ||
            line.ends_with('/') || line.contains("//")) {
            throw std::runtime_error(
                "invalid sync list rule at " + path.string() + ":" +
                std::to_string(line_number)
            );
        }

        for (const auto segment : split_path(line)) {
            if (segment.empty() || segment == "." || segment == ".." ||
                (segment.contains("**") && segment != "**")) {
                throw std::runtime_error(
                    "invalid sync list rule at " + path.string() + ":" +
                    std::to_string(line_number)
                );
            }
            rule.segments.emplace_back(segment);
        }
        canonical += rule.exclude ? "!" : "+";
        canonical += rule.anchored ? "/" : "";
        canonical += line;
        canonical += rule.directory_only ? "/" : "";
        canonical += "\n";
        result.rules_.push_back(std::move(rule));
    }
    if (input.bad()) {
        throw std::runtime_error(
            "cannot read sync list '" + path.string() + "'"
        );
    }
    result.canonical_ = std::move(canonical);
    result.fingerprint_ = util::sha256_hex(result.canonical_);
    return result;
}

SyncList SyncList::configured(
    const std::optional<std::filesystem::path>& rules_path,
    bool include_root_files,
    const std::filesystem::path& sync_root,
    bool nosync_enabled,
    bool exclude_dotfiles,
    std::uint64_t maximum_file_size_bytes
) {
    auto result =
        rules_path ? load(*rules_path, include_root_files) : SyncList{};
    const auto rules_fingerprint = result.fingerprint_;
    if (!rules_path) {
        result.select_all_ = true;
        result.canonical_ = "onedrive-cpp-sync-list-v1\n@all\n";
    }
    result.nosync_enabled_ = nosync_enabled;
    result.exclude_dotfiles_ = exclude_dotfiles;
    result.maximum_file_size_bytes_ = maximum_file_size_bytes;
    if (nosync_enabled) {
        result.nosync_directories_ = discover_nosync_directories(sync_root);
    }
    result.canonical_ +=
        std::string{"@nosync="} + (nosync_enabled ? "true\n" : "false\n");
    result.canonical_ += std::string{"@dotfiles="} +
                         (exclude_dotfiles ? "exclude\n" : "include\n");
    result.canonical_ +=
        "@maximum-file-size=" + std::to_string(maximum_file_size_bytes) + "\n";
    for (const auto& directory : result.nosync_directories_) {
        result.canonical_ += "@nosync-directory=" + directory + "\n";
    }
    const bool baseline_policy = result.nosync_directories_.empty() &&
                                 !exclude_dotfiles &&
                                 maximum_file_size_bytes == 0;
    result.fingerprint_ = baseline_policy ?
        rules_fingerprint :
        util::sha256_hex(result.canonical_);
    return result;
}

bool SyncList::matches(
    const Rule& rule,
    const std::vector<std::string_view>& path_segments,
    bool directory
) const {
    if (rule.directory_only && !directory &&
        rule.segments.size() >= path_segments.size()) {
        return false;
    }
    if (rule.anchored) {
        return match_segments(rule.segments, path_segments, 0, 0);
    }
    for (std::size_t start = 0; start < path_segments.size(); ++start) {
        if (match_segments(rule.segments, path_segments, 0, start)) {
            return true;
        }
    }
    return false;
}

bool SyncList::matches_exclusion(
    const std::vector<std::string_view>& path_segments, bool directory
) const {
    return std::ranges::any_of(rules_, [&](const Rule& rule) {
        return rule.exclude && matches(rule, path_segments, directory);
    });
}

bool SyncList::includes(
    std::string_view remote_path,
    bool directory,
    std::optional<std::uint64_t> size
) const {
    const auto path_segments = split_path(remote_path);
    if (path_segments.empty() ||
        policy_excludes(remote_path, path_segments, directory, size) ||
        matches_exclusion(path_segments, directory)) {
        return false;
    }

    return select_all_ ||
           (include_root_files_ && !directory && path_segments.size() == 1) ||
           std::ranges::any_of(rules_, [&](const Rule& rule) {
               return !rule.exclude && matches(rule, path_segments, directory);
           });
}

bool SyncList::excludes(
    std::string_view remote_path,
    bool directory,
    std::optional<std::uint64_t> size
) const {
    const auto path_segments = split_path(remote_path);
    return !path_segments.empty() &&
           (policy_excludes(remote_path, path_segments, directory, size) ||
            matches_exclusion(path_segments, directory));
}

bool SyncList::policy_excludes(
    std::string_view remote_path,
    const std::vector<std::string_view>& path_segments,
    bool directory,
    std::optional<std::uint64_t> size
) const {
    if (nosync_enabled_ &&
        (path_segments.back() == ".nosync" ||
         std::ranges::any_of(
             nosync_directories_,
             [remote_path](const std::string& excluded) {
                 return excluded.empty() || remote_path == excluded ||
                        remote_path_is_descendant(remote_path, excluded);
             }
         ))) {
        return true;
    }
    if (exclude_dotfiles_ && dotfile_path(path_segments)) {
        return true;
    }
    return !directory && maximum_file_size_bytes_ != 0 && size.has_value() &&
           *size > maximum_file_size_bytes_;
}

const std::string& SyncList::fingerprint() const noexcept {
    return fingerprint_;
}

std::size_t SyncList::rule_count() const noexcept {
    return rules_.size();
}

FilteredDelta filter_delta(
    graph::DeltaResult delta,
    const SyncList& sync_list,
    const std::function<bool(std::string_view)>& is_tracked,
    storage::DeltaApplyMode apply_mode
) {
    enum class SelectionState {
        excluded,
        selected,
    };
    std::vector<SelectionState> selection(
        delta.changes.size(), SelectionState::excluded
    );
    std::vector<std::string_view> selected_paths;
    selected_paths.reserve(delta.changes.size());
    for (std::size_t index = 0; index < delta.changes.size(); ++index) {
        const auto& item = delta.changes[index];
        const auto size =
            !item.directory && item.size >= 0
                ? std::optional{static_cast<std::uint64_t>(item.size)}
                : std::nullopt;
        if (item.deleted || item.root ||
            sync_list.includes(item.remote_path, item.directory, size)) {
            selection[index] = SelectionState::selected;
        }
        if (selection[index] == SelectionState::selected && !item.deleted &&
            !item.root) {
            selected_paths.push_back(item.remote_path);
        }
    }

    for (std::size_t index = 0; index < delta.changes.size(); ++index) {
        const auto& item = delta.changes[index];
        if (selection[index] == SelectionState::selected || !item.directory ||
            item.deleted || item.root) {
            continue;
        }
        if (std::ranges::any_of(
                selected_paths,
                [&](std::string_view path) {
                    return remote_path_is_descendant(path, item.remote_path);
                }
            ) &&
            !sync_list.excludes(item.remote_path, true)) {
            selection[index] = SelectionState::selected;
        }
    }

    FilteredDelta result{
        .delta =
            {
                .changes = {},
                .delta_link = std::move(delta.delta_link),
            },
        .snapshot_removals = {},
        .retained_remote_ids = {},
    };
    result.delta.changes.reserve(delta.changes.size());
    for (std::size_t index = 0; index < delta.changes.size(); ++index) {
        auto& item = delta.changes[index];
        if (selection[index] == SelectionState::selected) {
            result.delta.changes.push_back(std::move(item));
            continue;
        }
        ++result.excluded;
        if (is_tracked(item.id)) {
            result.retained_remote_ids.push_back(item.id);
            if (apply_mode == storage::DeltaApplyMode::merge) {
                result.snapshot_removals.push_back(item.id);
            }
        }
    }
    return result;
}

} // namespace onedrive::sync::detail
