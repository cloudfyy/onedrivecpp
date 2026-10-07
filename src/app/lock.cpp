#include "lock.hpp"

#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace onedrive::app::detail {
namespace {

constexpr mode_t private_file_mode = S_IRUSR | S_IWUSR;

}  // namespace

onedrive::util::UniqueFD acquire_runtime_lock(
    const std::filesystem::path& state_directory
) {
    const auto path = state_directory / "onedrive-cpp.lock";
    onedrive::util::UniqueFD descriptor{
        ::open(
            path.c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            private_file_mode
        )
    };
    if (!descriptor) {
        throw std::runtime_error(
            "cannot open runtime lock '" + path.string() + "': " +
            onedrive::util::system_error_message(errno)
        );
    }
    static_cast<void>(onedrive::util::secure_owned_regular_file(
        descriptor.get(), path, private_file_mode, "runtime lock"
    ));
    if (::flock(descriptor.get(), LOCK_EX | LOCK_NB) == -1) {
        const std::string message = errno == EWOULDBLOCK ?
            "another onedrive-cpp process is already using this state directory" :
            onedrive::util::system_error_message(errno);
        throw std::runtime_error(
            "cannot acquire runtime lock '" + path.string() + "': " + message
        );
    }

    const std::string process_id = std::to_string(::getpid()) + "\n";
    if (::ftruncate(descriptor.get(), 0) == -1 ||
        ::write(
            descriptor.get(),
            process_id.data(),
            process_id.size()
        ) !=
            static_cast<ssize_t>(process_id.size()) ||
        ::fsync(descriptor.get()) == -1) {
        const std::string message = onedrive::util::system_error_message(errno);
        throw std::runtime_error(
            "cannot update runtime lock '" + path.string() + "': " + message
        );
    }
    return descriptor;
}

}  // namespace onedrive::app::detail
