#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace onedrive::sync::detail {

enum class TreeWalkAction {
    continue_walk,
    skip_subtree,
};

struct TreeWalkErrors {
    std::string_view open;
    std::string_view inspect;
    std::string_view advance;
};

template <typename Visitor>
void walk_directory_tree(
    const std::filesystem::path& root, TreeWalkErrors errors, Visitor&& visitor
) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error
    };
    if (error) {
        throw std::runtime_error(
            std::string{errors.open} + ": " + error.message()
        );
    }

    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw std::runtime_error(
                std::string{errors.inspect} + " '" + iterator->path().string() +
                "': " + error.message()
            );
        }
        const auto action = std::invoke(visitor, *iterator, status);
        if (action == TreeWalkAction::skip_subtree &&
            std::filesystem::is_directory(status)) {
            iterator.disable_recursion_pending();
        }
        iterator.increment(error);
        if (error) {
            throw std::runtime_error(
                std::string{errors.advance} + ": " + error.message()
            );
        }
    }
}

} // namespace onedrive::sync::detail
