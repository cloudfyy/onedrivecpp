#pragma once

#include <string>
#include <stdexcept>
#include <string_view>

namespace onedrive::util {

enum class FileHashAlgorithm {
    quick_xor,
    sha256,
    sha1,
};

[[nodiscard]] inline std::string_view
file_hash_algorithm_name(FileHashAlgorithm algorithm) {
    switch (algorithm) {
    case FileHashAlgorithm::quick_xor:
        return "quick_xor";
    case FileHashAlgorithm::sha256:
        return "sha256";
    case FileHashAlgorithm::sha1:
        return "sha1";
    }
    throw std::invalid_argument("unknown file hash algorithm");
}

struct FileHash {
    FileHashAlgorithm algorithm{FileHashAlgorithm::quick_xor};
    std::string value;

    bool operator==(const FileHash&) const = default;
};

}  // namespace onedrive::util
