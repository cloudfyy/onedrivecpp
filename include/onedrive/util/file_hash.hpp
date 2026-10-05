#pragma once

#include <string>

namespace onedrive::util {

enum class FileHashAlgorithm {
    quick_xor,
    sha256,
};

struct FileHash {
    FileHashAlgorithm algorithm{FileHashAlgorithm::quick_xor};
    std::string value;
};

}  // namespace onedrive::util
