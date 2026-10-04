#include "selective_sync.hpp"

#include "onedrive/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace onedrive::sync::detail {
namespace {

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string{value.substr(first, last - first + 1)};
}

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
    while (pattern_index < pattern.size() &&
           pattern[pattern_index] == '*') {
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
        if (first >= 0xc2 && first <= 0xdf &&
            index + 1 < value.size() &&
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
              continuation(
                  static_cast<unsigned char>(value[index + 1])
              )) ||
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
              continuation(
                  static_cast<unsigned char>(value[index + 1])
              )) ||
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
        if (match_segments(
                pattern,
                path,
                pattern_index + 1,
                path_index
            )) {
            return true;
        }
        return path_index < path.size() &&
               match_segments(
                   pattern,
                   path,
                   pattern_index,
                   path_index + 1
               );
    }
    return path_index < path.size() &&
           wildcard_match(pattern[pattern_index], path[path_index]) &&
           match_segments(
               pattern,
               path,
               pattern_index + 1,
               path_index + 1
           );
}

bool is_ancestor(std::string_view ancestor, std::string_view path) {
    return path.size() > ancestor.size() &&
           path.starts_with(ancestor) &&
           path[ancestor.size()] == '/';
}

}  // namespace

SyncList SyncList::load(
    const std::filesystem::path& path,
    bool include_root_files
) {
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
        line = trim(std::move(line));
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
        if (line.empty() || line.contains('\\') ||
            line.starts_with('/') || line.ends_with('/') ||
            line.contains("//")) {
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
    result.fingerprint_ = sha256_hex(canonical);
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
    const std::vector<std::string_view>& path_segments,
    bool directory
) const {
    return std::ranges::any_of(
        rules_,
        [&](const Rule& rule) {
            return rule.exclude && matches(rule, path_segments, directory);
        }
    );
}

bool SyncList::includes(
    std::string_view remote_path,
    bool directory
) const {
    const auto path_segments = split_path(remote_path);
    if (path_segments.empty() ||
        matches_exclusion(path_segments, directory)) {
        return false;
    }

    return (include_root_files_ && !directory &&
            path_segments.size() == 1) ||
           std::ranges::any_of(
               rules_,
               [&](const Rule& rule) {
                   return !rule.exclude &&
                          matches(rule, path_segments, directory);
               }
           );
}

bool SyncList::excludes(
    std::string_view remote_path,
    bool directory
) const {
    const auto path_segments = split_path(remote_path);
    return !path_segments.empty() &&
           matches_exclusion(path_segments, directory);
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
    bool replace_drive_items
) {
    std::vector<bool> selected(delta.changes.size(), false);
    std::vector<std::string_view> selected_paths;
    selected_paths.reserve(delta.changes.size());
    for (std::size_t index = 0; index < delta.changes.size(); ++index) {
        const auto& item = delta.changes[index];
        selected[index] =
            item.deleted || item.root ||
            sync_list.includes(item.remote_path, item.directory);
        if (selected[index] && !item.deleted && !item.root) {
            selected_paths.push_back(item.remote_path);
        }
    }

    for (std::size_t index = 0; index < delta.changes.size(); ++index) {
        const auto& item = delta.changes[index];
        if (selected[index] || !item.directory || item.deleted ||
            item.root) {
            continue;
        }
        selected[index] = std::ranges::any_of(
            selected_paths,
            [&](std::string_view path) {
                return is_ancestor(item.remote_path, path);
            }
        ) && !sync_list.excludes(item.remote_path, true);
    }

    FilteredDelta result{
        .delta = {
            .changes = {},
            .delta_link = std::move(delta.delta_link),
        },
    };
    result.delta.changes.reserve(delta.changes.size());
    for (std::size_t index = 0; index < delta.changes.size(); ++index) {
        auto& item = delta.changes[index];
        if (selected[index]) {
            result.delta.changes.push_back(std::move(item));
            continue;
        }
        ++result.excluded;
        if (!replace_drive_items && is_tracked(item.id)) {
            item.deleted = true;
            result.delta.changes.push_back(std::move(item));
        }
    }
    return result;
}

}  // namespace onedrive::sync::detail
