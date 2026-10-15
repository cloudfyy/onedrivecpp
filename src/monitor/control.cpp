#include "onedrive/monitor/control.hpp"

#include "onedrive/util/path_security.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <system_error>
#include <thread>
#include <utility>
#include <unistd.h>

namespace onedrive::monitor {
namespace {

using Json = nlohmann::json;
constexpr int protocol_version = 1;
constexpr std::size_t maximum_packet_size = 4096;
constexpr auto timeout = std::chrono::seconds{2};

std::filesystem::path endpoint(const std::filesystem::path& state_directory) {
    const auto directory = util::normalized_absolute(state_directory);
    const auto descriptor =
        util::open_path_no_symlinks(directory, O_RDONLY | O_DIRECTORY);
    const auto status = util::inspect_owned_directory(
        descriptor.get(), directory, "Monitor control directory"
    );
    if ((status.st_mode & 07777) != 0700) {
        throw std::runtime_error(
            "Monitor control directory must have permissions 0700"
        );
    }
    return directory / "monitor-control.sock";
}

sockaddr_un address(const std::filesystem::path& path) {
    sockaddr_un result{};
    result.sun_family = AF_UNIX;
    const auto name = path.string();
    if (name.size() >= sizeof(result.sun_path)) {
        throw std::runtime_error(
            "Monitor control socket path is too long: " + name
        );
    }
    std::memcpy(result.sun_path, name.c_str(), name.size() + 1);
    return result;
}

util::UniqueFD socket_descriptor() {
    util::UniqueFD result{
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
    };
    if (!result) {
        util::throw_errno_error("cannot create Monitor control socket");
    }
    return result;
}

void wait_ready(
    int descriptor, short events, std::chrono::steady_clock::time_point deadline
) {
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            )
                .count();
        if (remaining <= 0) {
            throw std::runtime_error("Monitor control request timed out");
        }
        pollfd poll_descriptor{descriptor, events, 0};
        const int result =
            ::poll(&poll_descriptor, 1, static_cast<int>(remaining));
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result == -1) {
            util::throw_errno_error("cannot poll Monitor control socket");
        }
        if (result == 0) {
            throw std::runtime_error("Monitor control request timed out");
        }
        if ((poll_descriptor.revents & events) != 0) {
            return;
        }
        throw std::runtime_error("Monitor control socket disconnected");
    }
}

ucred verify_peer(int descriptor) {
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(
            descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size
        ) == -1) {
        util::throw_errno_error("cannot verify Monitor control peer");
    }
    if (size != sizeof(credentials) || credentials.uid != ::geteuid()) {
        throw std::runtime_error(
            "Monitor control peer belongs to another user"
        );
    }
    return credentials;
}

void connect_socket(int descriptor, const sockaddr_un& socket_address) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (::connect(
                descriptor,
                reinterpret_cast<const sockaddr*>(&socket_address),
                sizeof(socket_address)
            ) == 0) {
            return;
        }
        if (errno == EINPROGRESS) {
            wait_ready(descriptor, POLLOUT, deadline);
            int error{};
            socklen_t size = sizeof(error);
            if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size) ==
                -1) {
                util::throw_errno_error(
                    "cannot inspect Monitor control connection"
                );
            }
            if (error == 0) {
                return;
            }
            throw std::system_error(
                error,
                std::generic_category(),
                "cannot connect to Monitor control endpoint"
            );
        }
        if (errno != EINTR && errno != EAGAIN) {
            util::throw_errno_error(
                "cannot connect to Monitor control endpoint (not running or "
                "stale)"
            );
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Monitor control request timed out");
        }
        if (errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
}

Json receive_packet(int descriptor) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, maximum_packet_size> buffer{};
    for (;;) {
        wait_ready(descriptor, POLLIN, deadline);
        const auto bytes =
            ::recv(descriptor, buffer.data(), buffer.size(), MSG_TRUNC);
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            util::throw_errno_error("cannot receive Monitor control packet");
        }
        if (bytes == 0 || static_cast<std::size_t>(bytes) > buffer.size()) {
            throw std::runtime_error(
                "Monitor control packet is empty or oversized"
            );
        }
        return Json::parse(buffer.data(), buffer.data() + bytes);
    }
}

void send_packet(int descriptor, const Json& packet) {
    const auto content = packet.dump();
    if (content.size() > maximum_packet_size) {
        throw std::runtime_error(
            "Monitor control response exceeds the size limit"
        );
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        wait_ready(descriptor, POLLOUT, deadline);
        const auto bytes =
            ::send(descriptor, content.data(), content.size(), MSG_NOSIGNAL);
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            util::throw_errno_error("cannot send Monitor control packet");
        }
        if (static_cast<std::size_t>(bytes) != content.size()) {
            throw std::runtime_error("Monitor control packet was truncated");
        }
        return;
    }
}

void wake(int descriptor) {
    constexpr std::uint64_t value = 1;
    while (::write(descriptor, &value, sizeof(value)) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return;
        }
        util::throw_errno_error("cannot wake Monitor control scheduler");
    }
}

const char* command_name(ControlCommand command) {
    switch (command) {
    case ControlCommand::status:
        return "status";
    case ControlCommand::trigger:
        return "trigger";
    case ControlCommand::stop:
        return "stop";
    }
    std::unreachable();
}

} // namespace

struct ControlServer::Impl {
    std::filesystem::path path;
    std::filesystem::path root;
    util::UniqueFD listener;
    util::UniqueFD notification;
    std::stop_source stop;
    mutable std::mutex mutex;
    std::string state{"starting"};
    bool pending{false};
    std::uint64_t completed{0};
    std::optional<int> last_result;
    std::exception_ptr error;
    std::jthread worker;
    std::optional<util::FilesystemIdentity> identity;

    Impl(
        const std::filesystem::path& directory,
        const std::filesystem::path& sync_root
    )
        : path{endpoint(directory)},
          root{util::canonical_directory(sync_root)},
          listener{socket_descriptor()},
          notification{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)} {
        if (!notification) {
            util::throw_errno_error(
                "cannot create Monitor control event descriptor"
            );
        }
        struct stat status{};
        if (::lstat(path.c_str(), &status) == 0) {
            if (!S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid()) {
                throw std::runtime_error(
                    "Monitor control endpoint is not an owned socket"
                );
            }
            auto probe = socket_descriptor();
            const auto socket_address = address(path);
            if (::connect(
                    probe.get(),
                    reinterpret_cast<const sockaddr*>(&socket_address),
                    sizeof(socket_address)
                ) == 0 ||
                (errno != ECONNREFUSED && errno != ENOENT)) {
                throw std::runtime_error(
                    "Monitor control endpoint is already active"
                );
            }
            if (::unlink(path.c_str()) == -1) {
                util::throw_errno_error(
                    "cannot remove stale Monitor control socket"
                );
            }
        } else if (errno != ENOENT) {
            util::throw_errno_error("cannot inspect Monitor control endpoint");
        }
        const auto socket_address = address(path);
        if (::bind(
                listener.get(),
                reinterpret_cast<const sockaddr*>(&socket_address),
                sizeof(socket_address)
            ) == -1) {
            util::throw_errno_error("cannot bind Monitor control socket");
        }
        try {
            if (::lstat(path.c_str(), &status) == -1) {
                util::throw_errno_error(
                    "cannot inspect bound Monitor control socket"
                );
            }
            identity = util::filesystem_identity(status);
            if (::chmod(path.c_str(), 0600) == -1 ||
                ::listen(listener.get(), 16) == -1) {
                util::throw_errno_error(
                    "cannot prepare Monitor control socket"
                );
            }
            worker = std::jthread{[this](std::stop_token token) {
                try {
                    serve(token);
                } catch (...) {
                    {
                        const std::scoped_lock lock{mutex};
                        error = std::current_exception();
                        state = "stopping";
                    }
                    stop.request_stop();
                    try {
                        wake(notification.get());
                    } catch (const std::exception& failure) {
                        std::fprintf(
                            stderr,
                            "Cannot wake failed Monitor control scheduler: "
                            "%s\n",
                            failure.what()
                        );
                    }
                }
            }};
        } catch (...) {
            remove_endpoint();
            throw;
        }
    }

    ~Impl() {
        worker.request_stop();
        if (worker.joinable()) {
            worker.join();
        }
        remove_endpoint();
    }

    void remove_endpoint() noexcept {
        struct stat status{};
        if (::lstat(path.c_str(), &status) == -1) {
            if (errno != ENOENT) {
                std::fprintf(
                    stderr,
                    "Cannot inspect Monitor control endpoint during cleanup: "
                    "%s\n",
                    util::system_error_message(errno).c_str()
                );
            }
            return;
        }
        if (!identity || util::filesystem_identity(status) != *identity) {
            std::fprintf(
                stderr,
                "Monitor control endpoint was replaced; refusing to remove it\n"
            );
            return;
        }
        if (::unlink(path.c_str()) == -1 && errno != ENOENT) {
            std::fprintf(
                stderr,
                "Cannot remove Monitor control endpoint: %s\n",
                util::system_error_message(errno).c_str()
            );
        }
    }

    Json dispatch(const Json& request) {
        if (!request.is_object() || request.size() != 2 ||
            !request.at("version").is_number_integer() ||
            request.at("version") != protocol_version ||
            !request.at("command").is_string()) {
            throw std::runtime_error(
                "invalid Monitor control request or protocol version"
            );
        }
        const auto command = request.at("command").get<std::string>();
        bool stopping = false;
        Json reply;
        {
            const std::scoped_lock lock{mutex};
            std::string acknowledgement{"status"};
            if (command == "trigger") {
                if (state == "stopping") {
                    throw std::runtime_error(
                        "Monitor is stopping; trigger rejected"
                    );
                }
                acknowledgement = pending ? "merged" : "accepted";
                pending = true;
                wake(notification.get());
            } else if (command == "stop") {
                acknowledgement = state == "stopping" ? "merged" : "accepted";
                state = "stopping";
                pending = false;
                stopping = true;
            } else if (command != "status") {
                throw std::runtime_error("unknown Monitor control command");
            }
            reply = {
                {"version", protocol_version},
                {"ok", true},
                {"acknowledgement", acknowledgement},
                {"state", state},
                {"pid", ::getpid()},
                {"sync_directory", root.string()},
                {"pending", pending},
                {"completed_rounds", completed},
                {"last_result",
                 last_result ? Json(*last_result) : Json(nullptr)},
            };
        }
        if (stopping) {
            stop.request_stop();
            wake(notification.get());
        }
        return reply;
    }

    void serve(std::stop_token token) {
        while (!token.stop_requested()) {
            pollfd descriptor{listener.get(), POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 100);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result == -1) {
                util::throw_errno_error("Monitor control accept poll failed");
            }
            if (result == 0) {
                continue;
            }
            if ((descriptor.revents & POLLIN) == 0) {
                throw std::runtime_error(
                    "Monitor control listener became unavailable"
                );
            }
            util::UniqueFD client{::accept4(
                listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC
            )};
            if (!client) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                util::throw_errno_error(
                    "cannot accept Monitor control connection"
                );
            }
            try {
                verify_peer(client.get());
                Json reply;
                try {
                    reply = dispatch(receive_packet(client.get()));
                } catch (const std::exception& failure) {
                    reply = {
                        {"version", protocol_version},
                        {"ok", false},
                        {"error", failure.what()}
                    };
                }
                send_packet(client.get(), reply);
            } catch (const std::exception& failure) {
                std::fprintf(
                    stderr,
                    "Monitor control connection failed: %s\n",
                    failure.what()
                );
            }
        }
    }
};

ControlServer::ControlServer(
    const std::filesystem::path& directory, const std::filesystem::path& root
)
    : implementation_{std::make_unique<Impl>(directory, root)} {
}
ControlServer::~ControlServer() = default;
int ControlServer::descriptor() const noexcept {
    return implementation_->notification.get();
}
std::stop_token ControlServer::stop_token() const noexcept {
    return implementation_->stop.get_token();
}
void ControlServer::request_stop() {
    {
        const std::scoped_lock lock{implementation_->mutex};
        implementation_->state = "stopping";
        implementation_->pending = false;
    }
    implementation_->stop.request_stop();
    wake(descriptor());
}
bool ControlServer::take_trigger() {
    std::uint64_t value{};
    while (::read(descriptor(), &value, sizeof(value)) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            break;
        }
        util::throw_errno_error("cannot drain Monitor control scheduler");
    }
    const std::scoped_lock lock{implementation_->mutex};
    return std::exchange(implementation_->pending, false);
}
void ControlServer::begin_round() {
    const std::scoped_lock lock{implementation_->mutex};
    if (implementation_->state != "stopping") {
        implementation_->state = "synchronizing";
    }
}
void ControlServer::finish_round(int result) {
    const std::scoped_lock lock{implementation_->mutex};
    if (implementation_->completed ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Monitor completed-round counter overflowed");
    }
    ++implementation_->completed;
    implementation_->last_result = result;
    if (implementation_->state != "stopping") {
        implementation_->state = "waiting";
    }
}
void ControlServer::check_error() const {
    const std::scoped_lock lock{implementation_->mutex};
    if (implementation_->error) {
        std::rethrow_exception(implementation_->error);
    }
}

ControlReply request_control(
    const std::filesystem::path& directory, ControlCommand command
) {
    const auto path = endpoint(directory);
    struct stat status{};
    if (::lstat(path.c_str(), &status) == -1) {
        util::throw_errno_error(
            "Monitor control endpoint is unavailable; start 'transfer watch'"
        );
    }
    if (!S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != 0600) {
        throw std::runtime_error(
            "Monitor control endpoint must be an owned socket with permissions "
            "0600"
        );
    }
    auto client = socket_descriptor();
    const auto socket_address = address(path);
    connect_socket(client.get(), socket_address);
    const auto peer = verify_peer(client.get());
    send_packet(
        client.get(),
        {{"version", protocol_version}, {"command", command_name(command)}}
    );
    const auto reply = receive_packet(client.get());
    if (!reply.is_object() || !reply.at("version").is_number_integer() ||
        reply.at("version") != protocol_version) {
        throw std::runtime_error(
            "unsupported Monitor control response version"
        );
    }
    if (!reply.at("ok").get<bool>()) {
        throw std::runtime_error(reply.at("error").get<std::string>());
    }
    if (reply.size() != 9 || !reply.at("pid").is_number_integer() ||
        reply.at("pid") != peer.pid ||
        !reply.at("completed_rounds").is_number_unsigned() ||
        (!reply.at("last_result").is_null() &&
         (!reply.at("last_result").is_number_integer() ||
          reply.at("last_result") < std::numeric_limits<int>::min() ||
          reply.at("last_result") > std::numeric_limits<int>::max()))) {
        throw std::runtime_error("invalid Monitor control response fields");
    }
    ControlReply result{
        .acknowledgement = reply.at("acknowledgement").get<std::string>(),
        .state = reply.at("state").get<std::string>(),
        .pid = reply.at("pid").get<std::int64_t>(),
        .sync_directory = reply.at("sync_directory").get<std::string>(),
        .pending = reply.at("pending").get<bool>(),
        .completed_rounds = reply.at("completed_rounds").get<std::uint64_t>(),
        .last_result = std::nullopt,
    };
    if (!reply.at("last_result").is_null()) {
        result.last_result = reply.at("last_result").get<int>();
    }
    if ((result.state != "starting" && result.state != "synchronizing" &&
         result.state != "waiting" && result.state != "stopping") ||
        !result.sync_directory.is_absolute() ||
        (command == ControlCommand::status
             ? result.acknowledgement != "status"
             : result.acknowledgement != "accepted" &&
                   result.acknowledgement != "merged")) {
        throw std::runtime_error(
            "invalid Monitor control response state or acknowledgement"
        );
    }
    return result;
}

} // namespace onedrive::monitor
