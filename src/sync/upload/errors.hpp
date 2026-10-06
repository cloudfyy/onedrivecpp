#pragma once

#include <stdexcept>

namespace onedrive::sync::detail {

class RemoteUploadConflictError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace onedrive::sync::detail
