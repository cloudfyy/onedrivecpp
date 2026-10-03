#pragma once

#include <string>

namespace onedrive {

enum class FileHashAlgorithm {
    quick_xor,
    sha256,
};

struct FileHash {
    FileHashAlgorithm algorithm{FileHashAlgorithm::quick_xor};
    std::string value;
};

}  // namespace onedrive
