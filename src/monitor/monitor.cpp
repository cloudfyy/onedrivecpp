#include "onedrive/monitor/monitor.hpp"

#include "monitor/signal.hpp"
#include "monitor/input.hpp"
#include "monitor/notify.hpp"
#include "monitor/socket.hpp"
#include "monitor/state.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace onedrive::monitor {

detail::TerminationSignalMask::TerminationSignalMask() {
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

detail::TerminationSignalMask::~TerminationSignalMask() {
    if (!active_) {
        return;
    }
    const int result =
        ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    if (result != 0) {
        try {
            spdlog::error(
                "Cannot restore monitor termination signal mask: {}",
                onedrive::util::system_error_message(result)
            );
        } catch (...) {
            std::fprintf(
                stderr,
                "Cannot restore monitor termination signal mask: error %d\n",
                result
            );
        }
    }
}

const sigset_t& detail::TerminationSignalMask::signals() const noexcept {
    return signals_;
}

namespace {

constexpr std::uint32_t watch_mask =
    IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
    IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_IGNORED | IN_Q_OVERFLOW;

class WatchSet final {
public:
    explicit WatchSet(const std::filesystem::path& root)
        : descriptor_{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)} {
        if (descriptor_.get() < 0) {
            util::throw_errno_error("inotify_init1 failed");
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
                util::throw_errno_error(
                    "cannot remove inotify watch while rebuilding"
                );
            }
        }
        paths_.clear();
        add_tree(root);
    }

    [[nodiscard]] std::optional<detail::LocalChangeKind> drain(
        const std::filesystem::path& root
    ) {
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
                util::throw_errno_error("cannot read inotify events");
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
        if (overflow) {
            return detail::LocalChangeKind::watch_overflow;
        }
        if (changed) {
            return detail::LocalChangeKind::filesystem;
        }
        return std::nullopt;
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

    onedrive::util::UniqueFD descriptor_;
    std::unordered_map<int, std::filesystem::path> paths_;
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
    std::chrono::milliseconds settle_delay,
    NotificationCallbacks notifications
)
    : root_{std::move(root)},
      synchronize_{std::move(synchronize)},
      poll_interval_{poll_interval},
      settle_delay_{settle_delay},
      notifications_{std::move(notifications)} {
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
    return run(false);
}

int Monitor::run(bool keyboard_exit) const {
    return run(keyboard_exit, {});
}

int Monitor::run(
    bool keyboard_exit, const std::stop_token& stop_token
) const {
    detail::TerminationSignalMask signal_mask;
    onedrive::util::UniqueFD signal_descriptor{
        ::signalfd(
            -1,
            &signal_mask.signals(),
            SFD_NONBLOCK | SFD_CLOEXEC
        )
    };
    if (signal_descriptor.get() < 0) {
        util::throw_errno_error("signalfd failed");
    }
    return run_loop(stop_token, signal_descriptor.get(), keyboard_exit);
}

int Monitor::run(const std::stop_token& stop_token) const {
    return run_loop(stop_token, -1, false);
}

int Monitor::run_loop(
    const std::stop_token& stop_token,
    int signal_descriptor,
    bool keyboard_exit
) const {
    const detail::MonitorTiming timing{
        .poll_interval = poll_interval_,
        .settle_delay = settle_delay_,
    };
    detail::KeyboardInput keyboard{
        keyboard_exit ? STDIN_FILENO : -1,
        keyboard_exit
    };
    detail::MonitorState state = detail::StartingState{};
    if (stop_token.stop_requested()) {
        state = detail::transition_monitor(
            std::move(state),
            detail::StopRequestedEvent{},
            timing
        ).state;
        return 0;
    }

    auto transition = detail::transition_monitor(
        std::move(state),
        detail::StartEvent{},
        timing
    );
    state = std::move(transition.state);
    const auto initial_reason =
        std::get<detail::SynchronizingState>(state).reason;
    spdlog::info(
        "Running synchronization triggered by {}",
        detail::synchronization_reason_name(initial_reason)
    );
    const int initial_result = synchronize_(stop_token);
    if (initial_result != 0) {
        spdlog::warn(
            "Initial monitor synchronization completed with status {}",
            initial_result
        );
    }
    if (stop_token.stop_requested()) {
        state = detail::transition_monitor(
            std::move(state),
            detail::StopRequestedEvent{},
            timing
        ).state;
        return 0;
    }
    WatchSet watches{root_};
    onedrive::util::UniqueFD stop_descriptor{
        ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
    };
    if (stop_descriptor.get() < 0) {
        util::throw_errno_error("eventfd failed");
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
    state = detail::transition_monitor(
        std::move(state),
        detail::SynchronizationCompletedEvent{
            .status = initial_result,
            .completed_at = std::chrono::steady_clock::now(),
        },
        timing
    ).state;

    std::unique_ptr<detail::SocketIoTransport> notification_socket;
    detail::NotificationState notification_state =
        detail::NotificationStoppedState{};
    if (notifications_) {
        notification_socket =
            std::make_unique<detail::SocketIoTransport>(
                notifications_.proxy,
                notifications_.connect_timeout
            );
        notification_state = detail::NotificationDormantState{};
    }
    const detail::NotificationTiming notification_timing{
        .request_timeout = notifications_.request_timeout,
        .connect_timeout = notifications_.connect_timeout,
        .initial_backoff = notifications_.initial_backoff,
        .maximum_backoff = notifications_.maximum_backoff,
    };
    bool notification_sync_requested = false;
    const auto advance_notification =
        [&](const detail::NotificationEvent& event) {
            auto notification_transition = detail::transition_notification(
                std::move(notification_state),
                event,
                notification_timing
            );
            while (true) {
                notification_state =
                    std::move(notification_transition.state);
                notification_sync_requested =
                    notification_sync_requested ||
                    notification_transition.synchronize;
                const auto command = notification_transition.command;
                if (command == detail::NotificationCommand::none) {
                    return;
                }
                if (command ==
                    detail::NotificationCommand::disconnect_socket) {
                    notification_socket->disconnect();
                    return;
                }
                if (command == detail::NotificationCommand::connect_socket) {
                    notification_socket->connect(
                        std::get<detail::NotificationConnectingState>(
                            notification_state
                        ).notification_url
                    );
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                if (command == detail::NotificationCommand::refresh_token) {
                    notification_transition =
                        detail::transition_notification(
                            std::move(notification_state),
                            notifications_.refresh_token() ?
                                detail::NotificationEvent{
                                    detail::NotificationTokenRefreshedEvent{
                                        now
                                    }
                                } :
                                detail::NotificationEvent{
                                    detail::
                                        NotificationTokenRefreshFailedEvent{
                                            now
                                        }
                                },
                            notification_timing
                        );
                    continue;
                }
                const auto channel = notifications_.acquire_channel();
                if (channel) {
                    notification_transition =
                        detail::transition_notification(
                            std::move(notification_state),
                            detail::NotificationChannelAcquiredEvent{
                                .acquired_at = now,
                                .renew_at = channel->renew_at,
                                .notification_url = channel->url,
                            },
                            notification_timing
                        );
                } else {
                    notification_transition =
                        detail::transition_notification(
                            std::move(notification_state),
                            detail::NotificationChannelFailedEvent{
                                .failed_at = now,
                                .failure =
                                    channel.error() ?
                                        detail::NotificationFailure::
                                            unauthorized :
                                        detail::NotificationFailure::transient,
                            },
                            notification_timing
                        );
                }
            }
        };
    if (notification_socket) {
        advance_notification(detail::NotificationStartEvent{
            std::chrono::steady_clock::now()
        });
    }
    const auto shutdown = [&] {
        spdlog::info("Monitor shutdown requested");
        state = detail::transition_monitor(
            std::move(state),
            detail::StopRequestedEvent{},
            timing
        ).state;
        if (notification_socket) {
            advance_notification(detail::NotificationStopEvent{});
        }
        return 0;
    };

    while (true) {
        std::array<pollfd, 5> descriptors{{
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
            {
                .fd = notification_socket ?
                          notification_socket->descriptor() :
                          -1,
                .events = static_cast<short>(
                    notification_socket ? POLLIN : 0
                ),
                .revents = 0,
            },
            {
                .fd = keyboard.descriptor(),
                .events = static_cast<short>(
                    keyboard.descriptor() >= 0 ? POLLIN : 0
                ),
                .revents = 0,
            },
        }};
        const auto now = std::chrono::steady_clock::now();
        auto deadline = detail::next_deadline(state);
        if (!deadline) {
            throw std::logic_error(
                "monitor entered a non-waiting state before poll"
            );
        }
        if (const auto notification_deadline =
                detail::next_notification_deadline(notification_state);
            notification_deadline &&
            *notification_deadline < *deadline) {
            deadline = notification_deadline;
        }
        const int ready = ::poll(
            descriptors.data(),
            descriptors.size(),
            poll_timeout(*deadline, now)
        );
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            util::throw_errno_error("monitor poll failed");
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
                    util::throw_errno_error(
                        "cannot read monitor termination signal"
                    );
                }
                throw std::runtime_error(
                    "truncated monitor termination signal"
                );
            }
            return shutdown();
        }
        if ((descriptors[1].revents & POLLIN) != 0 ||
            stop_token.stop_requested()) {
            return shutdown();
        }
        if ((descriptors[4].revents &
             (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return shutdown();
        }
        if ((descriptors[4].revents & POLLIN) != 0) {
            if (keyboard.exit_requested()) {
                return shutdown();
            }
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("inotify descriptor became unavailable");
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            if (const auto changed = watches.drain(root_)) {
                state = detail::transition_monitor(
                    std::move(state),
                    detail::LocalChangeEvent{
                        .kind = *changed,
                        .observed_at =
                            std::chrono::steady_clock::now(),
                    },
                    timing
                ).state;
            }
        }

        if (notification_socket &&
            (descriptors[3].revents & POLLIN) != 0) {
            for (const auto event : notification_socket->drain()) {
                switch (event) {
                case detail::SocketEvent::connected:
                    advance_notification(
                        detail::NotificationSocketConnectedEvent{}
                    );
                    break;
                case detail::SocketEvent::notification:
                    advance_notification(
                        detail::RemoteNotificationReceivedEvent{}
                    );
                    break;
                case detail::SocketEvent::disconnected:
                    advance_notification(
                        detail::NotificationSocketDisconnectedEvent{
                            std::chrono::steady_clock::now()
                        }
                    );
                    break;
                }
            }
        }
        if (notification_socket) {
            advance_notification(detail::NotificationDeadlineReachedEvent{
                std::chrono::steady_clock::now()
            });
        }
        if (notification_sync_requested) {
            transition = detail::transition_monitor(
                std::move(state),
                detail::RemoteChangeEvent{},
                timing
            );
            notification_sync_requested = false;
        } else {
            const auto after_events = std::chrono::steady_clock::now();
            transition = detail::transition_monitor(
                std::move(state),
                detail::DeadlineReachedEvent{after_events},
                timing
            );
        }
        state = std::move(transition.state);
        if (transition.effect != detail::MonitorEffect::synchronize) {
            continue;
        }

        const auto reason =
            std::get<detail::SynchronizingState>(state).reason;
        spdlog::info(
            "Monitor synchronization triggered by {}",
            detail::synchronization_reason_name(reason)
        );
        const int result = synchronize_(stop_token);
        if (result != 0) {
            spdlog::warn(
                "Monitor synchronization completed with status {}",
                result
            );
        }
        watches.rebuild(root_);
        state = detail::transition_monitor(
            std::move(state),
            detail::SynchronizationCompletedEvent{
                .status = result,
                .completed_at = std::chrono::steady_clock::now(),
            },
            timing
        ).state;
    }
}

}  // namespace onedrive::monitor
