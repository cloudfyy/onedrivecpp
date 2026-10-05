#include "onedrive/monitor/monitor.hpp"

#include "util/unique_file_descriptor.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace onedrive::monitor {
namespace {

constexpr std::uint32_t watch_mask =
    IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
    IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_IGNORED | IN_Q_OVERFLOW;

[[noreturn]] void throw_system_error(const std::string& operation) {
    throw std::system_error{
        errno,
        std::generic_category(),
        operation
    };
}

class WatchSet final {
public:
    explicit WatchSet(const std::filesystem::path& root)
        : descriptor_{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)} {
        if (descriptor_.get() < 0) {
            throw_system_error("inotify_init1 failed");
        }
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            if (error) {
                throw std::system_error{
                    error,
                    "cannot inspect monitor root '" + root.string() + "'"
                };
            }
            throw std::runtime_error(
                "monitor root is not a directory: " + root.string()
            );
        }
        add_tree(root);
    }

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_.get();
    }

    void rebuild(const std::filesystem::path& root) {
        for (const auto& [watch, path] : paths_) {
            static_cast<void>(path);
            if (::inotify_rm_watch(descriptor_.get(), watch) < 0 &&
                errno != EINVAL) {
                throw_system_error(
                    "cannot remove inotify watch while rebuilding"
                );
            }
        }
        paths_.clear();
        add_tree(root);
    }

    [[nodiscard]] bool drain(const std::filesystem::path& root) {
        alignas(inotify_event)
            std::array<char, std::size_t{64} * 1024U> buffer{};
        bool changed = false;
        bool overflow = false;
        while (true) {
            const auto bytes = ::read(
                descriptor_.get(),
                buffer.data(),
                buffer.size()
            );
            if (bytes < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw_system_error("cannot read inotify events");
            }
            if (bytes == 0) {
                throw std::runtime_error("inotify event stream closed");
            }

            std::size_t offset = 0;
            const auto size = static_cast<std::size_t>(bytes);
            while (offset < size) {
                if (size - offset < sizeof(inotify_event)) {
                    throw std::runtime_error("truncated inotify event");
                }
                const auto* event = reinterpret_cast<const inotify_event*>(
                    buffer.data() + offset
                );
                const std::size_t event_size =
                    sizeof(inotify_event) + event->len;
                if (event_size > size - offset) {
                    throw std::runtime_error("invalid inotify event length");
                }
                offset += event_size;

                if ((event->mask & IN_Q_OVERFLOW) != 0U) {
                    overflow = true;
                    changed = true;
                    continue;
                }
                const auto watched = paths_.find(event->wd);
                if ((event->mask & IN_IGNORED) != 0U) {
                    if (watched != paths_.end()) {
                        paths_.erase(watched);
                    }
                    continue;
                }
                if (watched == paths_.end()) {
                    continue;
                }

                std::filesystem::path path = watched->second;
                if (event->len != 0U && event->name[0] != '\0') {
                    path /= event->name;
                }
                if ((event->mask & IN_ISDIR) != 0U &&
                    (event->mask & (IN_CREATE | IN_MOVED_TO)) != 0U) {
                    add_tree(path);
                }
                if ((event->mask &
                     (IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE |
                      IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM |
                      IN_MOVED_TO)) != 0U ||
                    ((event->mask & IN_CREATE) != 0U &&
                     (event->mask & IN_ISDIR) != 0U)) {
                    changed = true;
                }
            }
        }
        if (overflow) {
            spdlog::warn(
                "inotify queue overflowed; rebuilding watches and scheduling "
                "a complete synchronization"
            );
            rebuild(root);
        }
        return changed;
    }

private:
    void add_watch(const std::filesystem::path& path) {
        const int watch = ::inotify_add_watch(
            descriptor_.get(),
            path.c_str(),
            watch_mask
        );
        if (watch < 0) {
            std::string message =
                "cannot monitor directory '" + path.string() + "'";
            if (errno == ENOSPC) {
                message +=
                    "; the inotify watch limit is exhausted "
                    "(increase fs.inotify.max_user_watches)";
            }
            throw std::system_error{
                errno,
                std::generic_category(),
                message
            };
        }
        paths_[watch] = path;
    }

    void add_tree(const std::filesystem::path& root) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(root, error);
        if (error) {
            if (error == std::errc::no_such_file_or_directory) {
                return;
            }
            throw std::system_error{
                error,
                "cannot inspect monitor directory '" + root.string() + "'"
            };
        }
        if (!std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            return;
        }
        add_watch(root);

        std::filesystem::recursive_directory_iterator iterator{
            root,
            std::filesystem::directory_options::none,
            error
        };
        if (error) {
            throw std::system_error{
                error,
                "cannot enumerate monitor directory '" + root.string() + "'"
            };
        }
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end) {
            const auto entry_status = iterator->symlink_status(error);
            if (error) {
                throw std::system_error{
                    error,
                    "cannot inspect monitor path '" +
                        iterator->path().string() + "'"
                };
            }
            if (!std::filesystem::is_symlink(entry_status) &&
                std::filesystem::is_directory(entry_status)) {
                add_watch(iterator->path());
            }
            iterator.increment(error);
            if (error) {
                throw std::system_error{
                    error,
                    "cannot continue monitor directory enumeration"
                };
            }
        }
    }

    onedrive::util::UniqueFileDescriptor descriptor_;
    std::unordered_map<int, std::filesystem::path> paths_;
};

class SignalMask final {
public:
    SignalMask() {
        ::sigemptyset(&signals_);
        ::sigaddset(&signals_, SIGINT);
        ::sigaddset(&signals_, SIGTERM);
        const int result = ::pthread_sigmask(SIG_BLOCK, &signals_, &previous_);
        if (result != 0) {
            throw std::system_error{
                result,
                std::generic_category(),
                "cannot block monitor termination signals"
            };
        }
        active_ = true;
    }

    SignalMask(const SignalMask&) = delete;
    SignalMask& operator=(const SignalMask&) = delete;
    SignalMask(SignalMask&&) = delete;
    SignalMask& operator=(SignalMask&&) = delete;

    ~SignalMask() {
        if (active_) {
            static_cast<void>(
                ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr)
            );
        }
    }

    [[nodiscard]] const sigset_t& signals() const noexcept {
        return signals_;
    }

private:
    sigset_t signals_{};
    sigset_t previous_{};
    bool active_{false};
};

int poll_timeout(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now
) {
    if (deadline <= now) {
        return 0;
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::min<std::int64_t>(
        remaining.count(),
        std::numeric_limits<int>::max()
    ));
}

}  // namespace

Monitor::Monitor(
    std::filesystem::path root,
    SyncCallback synchronize,
    std::chrono::milliseconds poll_interval,
    std::chrono::milliseconds settle_delay
)
    : root_{std::move(root)},
      synchronize_{std::move(synchronize)},
      poll_interval_{poll_interval},
      settle_delay_{settle_delay} {
    if (!synchronize_) {
        throw std::invalid_argument("monitor synchronization callback is empty");
    }
    if (poll_interval_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("monitor poll interval must be positive");
    }
    if (settle_delay_ < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("monitor settle delay must not be negative");
    }
}

int Monitor::run() const {
    SignalMask signal_mask;
    onedrive::util::UniqueFileDescriptor signal_descriptor{
        ::signalfd(
            -1,
            &signal_mask.signals(),
            SFD_NONBLOCK | SFD_CLOEXEC
        )
    };
    if (signal_descriptor.get() < 0) {
        throw_system_error("signalfd failed");
    }
    return run_loop({}, signal_descriptor.get());
}

int Monitor::run(const std::stop_token& stop_token) const {
    return run_loop(stop_token, -1);
}

int Monitor::run_loop(
    const std::stop_token& stop_token,
    int signal_descriptor
) const {
    if (stop_token.stop_requested()) {
        return 0;
    }

    spdlog::info("Running initial monitor synchronization");
    const int initial_result = synchronize_();
    if (initial_result != 0) {
        spdlog::warn(
            "Initial monitor synchronization completed with status {}",
            initial_result
        );
    }
    if (stop_token.stop_requested()) {
        return 0;
    }

    WatchSet watches{root_};
    onedrive::util::UniqueFileDescriptor stop_descriptor{
        ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
    };
    if (stop_descriptor.get() < 0) {
        throw_system_error("eventfd failed");
    }
    std::stop_callback stop_callback{
        stop_token,
        [descriptor = stop_descriptor.get()] {
            constexpr std::uint64_t value = 1;
            static_cast<void>(
                ::write(descriptor, &value, sizeof(value))
            );
        }
    };

    spdlog::info(
        "Monitoring '{}' with a {} ms local settle delay and {} ms Graph "
        "poll interval",
        root_.string(),
        settle_delay_.count(),
        poll_interval_.count()
    );

    bool local_change_pending = false;
    auto local_deadline = std::chrono::steady_clock::time_point::max();
    auto graph_deadline = std::chrono::steady_clock::now() + poll_interval_;
    while (true) {
        std::array<pollfd, 3> descriptors{{
            {
                .fd = watches.descriptor(),
                .events = POLLIN,
                .revents = 0,
            },
            {
                .fd = stop_descriptor.get(),
                .events = POLLIN,
                .revents = 0,
            },
            {
                .fd = signal_descriptor,
                .events = static_cast<short>(
                    signal_descriptor >= 0 ? POLLIN : 0
                ),
                .revents = 0,
            },
        }};
        const auto now = std::chrono::steady_clock::now();
        const auto deadline = local_change_pending ?
            local_deadline :
            graph_deadline;
        const int ready = ::poll(
            descriptors.data(),
            descriptors.size(),
            poll_timeout(deadline, now)
        );
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error("monitor poll failed");
        }
        if ((descriptors[2].revents & POLLIN) != 0) {
            signalfd_siginfo signal{};
            while (true) {
                const auto bytes = ::read(
                    signal_descriptor,
                    &signal,
                    sizeof(signal)
                );
                if (bytes == static_cast<ssize_t>(sizeof(signal))) {
                    break;
                }
                if (bytes < 0 && errno == EINTR) {
                    continue;
                }
                if (bytes < 0 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                if (bytes < 0) {
                    throw_system_error(
                        "cannot read monitor termination signal"
                    );
                }
                throw std::runtime_error(
                    "truncated monitor termination signal"
                );
            }
            spdlog::info("Monitor shutdown requested");
            return 0;
        }
        if ((descriptors[1].revents & POLLIN) != 0 ||
            stop_token.stop_requested()) {
            spdlog::info("Monitor shutdown requested");
            return 0;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("inotify descriptor became unavailable");
        }
        if ((descriptors[0].revents & POLLIN) != 0 &&
            watches.drain(root_)) {
            local_change_pending = true;
            local_deadline =
                std::chrono::steady_clock::now() + settle_delay_;
        }

        const auto after_events = std::chrono::steady_clock::now();
        const bool local_due =
            local_change_pending && after_events >= local_deadline;
        const bool graph_due =
            !local_change_pending && after_events >= graph_deadline;
        if (!local_due && !graph_due) {
            continue;
        }

        spdlog::info(
            "Monitor synchronization triggered by {}",
            local_due ? "local filesystem changes" :
                        "the Graph polling interval"
        );
        const int result = synchronize_();
        if (result != 0) {
            spdlog::warn(
                "Monitor synchronization completed with status {}",
                result
            );
        }
        watches.rebuild(root_);
        local_change_pending = false;
        local_deadline = std::chrono::steady_clock::time_point::max();
        graph_deadline =
            std::chrono::steady_clock::now() + poll_interval_;
    }
}

}  // namespace onedrive::monitor
