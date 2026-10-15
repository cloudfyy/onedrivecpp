#include "onedrive/monitor/control.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "onedrive/logging/logging.hpp"
#include "onedrive/util/path_security.hpp"
#include "app/checks.hpp"
#include "app/lock.hpp"
#include "app/support.hpp"
#include "sync/filesystem/metadata.hpp"
#include "support/common.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace onedrive;
using namespace std::chrono_literals;
using Json = nlohmann::json;

template <typename Predicate> void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "Monitor control subprocess did not reach the expected state"
            );
        }
        std::this_thread::sleep_for(5ms);
    }
}

class IgnoreEvents final : public events::Observer {
    void on_event(const events::Event&) const override {
    }
};

class Process {
public:
    Process(
        const std::filesystem::path& state, const std::filesystem::path& root
    ) {
        const auto state_string = state.string();
        const auto root_string = root.string();
        pid_ = ::fork();
        if (pid_ == 0) {
            ::execl(
                "/proc/self/exe",
                "monitor_control_tests",
                "--child",
                state_string.c_str(),
                root_string.c_str(),
                nullptr
            );
            ::_exit(127);
        }
        if (pid_ < 0) {
            throw std::runtime_error("cannot start Monitor control subprocess");
        }
    }
    ~Process() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) == -1 && errno == EINTR) {
            }
        }
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    pid_t pid() const {
        return pid_;
    }
    void finish(bool crash = false) {
        if (crash) {
            ::kill(pid_, SIGKILL);
        }
        int status{};
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                break;
            }
            if (result == -1 && errno != EINTR) {
                throw std::runtime_error("cannot wait for Monitor process");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("Monitor shutdown did not finish");
            }
            std::this_thread::sleep_for(5ms);
        }
        pid_ = -1;
        if (!crash && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
            throw std::runtime_error("Monitor child failed");
        }
    }

private:
    pid_t pid_{-1};
};

int child(
    const std::filesystem::path& state, const std::filesystem::path& root
) {
    app::detail::secure_state_directory(state);
    const auto state_lock = app::detail::acquire_runtime_lock(state, "monitor");
    const IgnoreEvents observer;
    const auto root_lock = app::detail::acquire_sync_directory_lock(
        root, state, "monitor", observer
    );
    int runs = 0;
    monitor::Monitor instance{
        root,
        [&](const std::stop_token& token) {
            ++runs;
            test::write_file(state / "round", std::to_string(runs));
            while (!std::filesystem::exists(
                state / ("release-" + std::to_string(runs))
            )) {
                if (token.stop_requested()) {
                    std::this_thread::sleep_for(250ms);
                    return exit_code::cancelled;
                }
                std::this_thread::sleep_for(5ms);
            }
            return runs == 1 ? exit_code::blocked : exit_code::success;
        },
        30s,
        20ms,
        {},
        state
    };
    return instance.run(std::stop_token{});
}

Json raw_request(const std::filesystem::path& state, std::string_view packet) {
    util::UniqueFD socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (state / "monitor-control.sock").string();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) == -1 ||
        ::send(socket.get(), packet.data(), packet.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(packet.size())) {
        throw std::runtime_error("cannot send raw Monitor control request");
    }
    pollfd descriptor{socket.get(), POLLIN, 0};
    if (::poll(&descriptor, 1, 3000) != 1) {
        throw std::runtime_error(
            "invalid control request did not receive a response"
        );
    }
    std::array<char, 4096> response{};
    const auto size = ::recv(socket.get(), response.data(), response.size(), 0);
    if (size <= 0) {
        throw std::runtime_error("invalid control response");
    }
    return Json::parse(response.data(), response.data() + size);
}

monitor::ControlReply control_status(const std::filesystem::path& state) {
    return monitor::request_control(state, monitor::ControlCommand::status);
}

void test_process_control() {
    test::TemporaryDirectory temporary;
    const auto state = temporary.path() / "state";
    const auto root = temporary.path() / "root";
    std::filesystem::create_directory(root);
    Process process{state, root};
    wait_until([&] { return std::filesystem::exists(state / "round"); });
    const auto initial = control_status(state);
    if (!test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(monitor::request_control(
                    state,
                    monitor::ControlCommand::trigger,
                    monitor::ControlTarget{initial.pid + 1, initial.instance_id}
                ));
            },
            "owner changed"
        ) ||
        control_status(state).pending) {
        throw std::runtime_error(
            "owner identity mismatch was allowed to control a replacement "
            "Monitor"
        );
    }
    if (initial.pid != process.pid() || initial.state != "synchronizing" ||
        initial.completed_rounds != 0 || initial.last_result ||
        initial.sync_directory != root) {
        throw std::runtime_error(
            "status did not report active initial round facts"
        );
    }
    std::string boundary_packet{R"({"version":2,"command":"status"})"};
    boundary_packet.resize(4096, ' ');
    if (raw_request(state, boundary_packet).at("ok") != true) {
        throw std::runtime_error(
            "the maximum-sized valid control packet was rejected"
        );
    }
    boundary_packet.push_back(' ');
    const auto oversized = raw_request(state, boundary_packet);
    if (oversized.at("ok") != false ||
        !oversized.at("error").get<std::string>().contains("oversized")) {
        throw std::runtime_error(
            "an oversized valid control packet bypassed the size limit"
        );
    }
    for (const auto& packet : {
             std::string{R"({"version":9,"command":"trigger"})"},
             std::string{R"({"version":1,"command":"trigger"})"},
             std::string{R"({"version":"2","command":"trigger"})"},
             std::string{R"({"version":2})"},
             std::string{R"({"version":2,"command":1})"},
             std::string{
                 R"({"version":2,"command":"trigger","instance_id":1})"
             },
             std::string{"[]"},
             std::string{R"({"version":2,"command":"unknown"})"},
             std::string{
                 R"({"version":2,"command":"trigger","config":"other"})"
             },
             std::string{
                 R"({"version":2,"command":"trigger","instance_id":"obsolete"})"
             },
             std::string{"not JSON"},
             std::string{},
             std::string(4097, 'x'),
         }) {
        const auto reply = raw_request(state, packet);
        if (reply.at("ok") != false || !reply.contains("error") ||
            control_status(state).pending) {
            throw std::runtime_error(
                "invalid protocol request was accepted or hid its error"
            );
        }
    }
    if (monitor::request_control(state, monitor::ControlCommand::trigger)
                .acknowledgement != "accepted" ||
        monitor::request_control(state, monitor::ControlCommand::trigger)
                .acknowledgement != "merged") {
        throw std::runtime_error(
            "active initial-round requests were not coalesced"
        );
    }
    test::write_file(root / "initial-change", "retained");
    test::write_file(state / "release-1", "");
    wait_until([&] { return control_status(state).completed_rounds == 1; });
    wait_until([&] {
        std::ifstream input{state / "round"};
        int count{};
        input >> count;
        return count == 2;
    });
    if (control_status(state).last_result != exit_code::blocked) {
        throw std::runtime_error(
            "blocked round was hidden or stopped monitoring"
        );
    }
    test::write_file(root / "during-round", "new change");
    for (int index = 0; index < 5; ++index) {
        const auto reply =
            monitor::request_control(state, monitor::ControlCommand::trigger);
        if (reply.acknowledgement != (index == 0 ? "accepted" : "merged")) {
            throw std::runtime_error(
                "repeated active-round triggers did not merge"
            );
        }
    }
    test::write_file(state / "release-2", "");
    wait_until([&] {
        std::ifstream input{state / "round"};
        int count{};
        input >> count;
        return count == 3;
    });
    test::write_file(state / "release-3", "");
    wait_until([&] { return control_status(state).state == "waiting"; });
    std::this_thread::sleep_for(100ms);
    if (control_status(state).completed_rounds != 3 ||
        control_status(state).pending) {
        throw std::runtime_error(
            "trigger burst created duplicate follow-up rounds"
        );
    }
    // Local changes alone must survive a running synchronization, without a
    // manual trigger.
    static_cast<void>(
        monitor::request_control(state, monitor::ControlCommand::trigger)
    );
    wait_until([&] {
        std::ifstream input{state / "round"};
        int count{};
        input >> count;
        return count == 4;
    });
    test::write_file(root / "local-only", "new local work");
    test::write_file(state / "release-4", "");
    wait_until([&] {
        std::ifstream input{state / "round"};
        int count{};
        input >> count;
        return count == 5;
    });
    const auto stopped =
        monitor::request_control(state, monitor::ControlCommand::stop);
    if (monitor::request_control(state, monitor::ControlCommand::stop)
            .acknowledgement != "merged") {
        throw std::runtime_error("repeated stop did not merge during cleanup");
    }
    if (stopped.acknowledgement != "accepted" || stopped.state != "stopping" ||
        stopped.completed_rounds != 4 ||
        !test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(monitor::request_control(
                    state, monitor::ControlCommand::trigger
                ));
            },
            "stopping"
        )) {
        throw std::runtime_error(
            "stop acknowledged completion prematurely or accepted a late "
            "trigger"
        );
    }
    if (!test::throws_with<app::detail::RuntimeBusyError>(
            [&] {
                static_cast<void>(
                    app::detail::acquire_runtime_lock(state, "synchronize")
                );
            },
            "already using"
        )) {
        throw std::runtime_error(
            "stop acknowledgement released the state lock before cleanup"
        );
    }
    const IgnoreEvents observer;
    if (!test::throws_with<app::detail::RuntimeBusyError>(
            [&] {
                static_cast<void>(app::detail::acquire_sync_directory_lock(
                    root,
                    temporary.path() / "other-state",
                    "synchronize",
                    observer
                ));
            },
            "already using"
        )) {
        throw std::runtime_error(
            "stop acknowledgement released the synchronization-root lease"
        );
    }
    process.finish();
    static_cast<void>(app::detail::acquire_runtime_lock(state, "synchronize"));
    static_cast<void>(app::detail::acquire_sync_directory_lock(
        root, state, "synchronize", observer
    ));
    if (std::filesystem::exists(state / "monitor-control.sock") ||
        !test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(control_status(state)); }, "unavailable"
        )) {
        throw std::runtime_error(
            "Monitor did not remove its endpoint after cleanup"
        );
    }
}

void test_cli_and_restart() {
    test::TemporaryDirectory temporary;
    const auto state = temporary.path() / "state";
    const auto root = temporary.path() / "root";
    std::filesystem::create_directory(root);
    const auto config_file = temporary.path() / "config.toml";
    test::write_file(
        config_file,
        "config_version = 2\n[state]\ndirectory = \"" + state.string() + "\"\n"
    );
    {
        Process process{state, root};
        wait_until([&] { return std::filesystem::exists(state / "round"); });
        test::app::FakeRuntimeFactory text_factory;
        const auto text = test::app::run_application(
            text_factory,
            {"onedrive-cpp",
             "monitor",
             "status",
             "--config",
             config_file.string()}
        );
        const logging::Session text_restore{config::Config::defaults().logging};
        if (text.exit_code != exit_code::success ||
            !text.standard_output.contains("Local Monitor control:") ||
            !text.standard_output.contains("synchronizing")) {
            throw std::runtime_error(
                "Monitor text status omitted its snapshot"
            );
        }
        for (const auto action : {"status", "trigger", "trigger", "stop"}) {
            test::app::FakeRuntimeFactory factory;
            const auto result = test::app::run_application(
                factory,
                {"onedrive-cpp",
                 "monitor",
                 action,
                 "--config",
                 config_file.string(),
                 "--output",
                 "json"}
            );
            const logging::Session restore{config::Config::defaults().logging};
            if (result.exit_code != exit_code::success ||
                !result.standard_output.contains(
                    "\"event\":\"monitor_control\""
                ) ||
                !result.standard_output.contains("\"instance_id\":") ||
                factory.graph_client_count != 0 ||
                factory.token_store_count != 0 ||
                factory.item_store_count != 0 ||
                (std::string_view{action} != "status" &&
                 !result.standard_output.contains(
                     "not synchronization or shutdown completion"
                 ))) {
                throw std::runtime_error(
                    "CLI Monitor control required authentication or lost "
                    "acknowledgement semantics"
                );
            }
        }
        process.finish();
    }
    {
        test::app::FakeRuntimeFactory factory;
        const auto missing = test::app::run_application(
            factory,
            {"onedrive-cpp",
             "monitor",
             "status",
             "--config",
             config_file.string(),
             "--quiet"}
        );
        const logging::Session restore{config::Config::defaults().logging};
        if (missing.exit_code != exit_code::failure ||
            missing.standard_output.contains("Local Monitor control:") ||
            !missing.standard_error.contains("unavailable") ||
            factory.graph_client_count || factory.item_store_count) {
            throw std::runtime_error(
                "missing Monitor was treated as a successful quiet query"
            );
        }
    }
    std::filesystem::remove(state / "round");
    {
        Process crashed{state, root};
        wait_until([&] { return std::filesystem::exists(state / "round"); });
        crashed.finish(true);
    }
    if (!test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(control_status(state)); }, "stale"
        )) {
        throw std::runtime_error(
            "crashed Monitor endpoint returned success-shaped status"
        );
    }
    std::filesystem::remove(state / "round");
    Process restarted{state, root};
    wait_until([&] { return std::filesystem::exists(state / "round"); });
    static_cast<void>(
        monitor::request_control(state, monitor::ControlCommand::stop)
    );
    restarted.finish();
    test::write_file(state / "monitor-control.sock", "not a socket");
    if (!test::throws_with<std::runtime_error>(
            [&] { monitor::ControlServer invalid{state, root}; },
            "not an owned socket"
        )) {
        throw std::runtime_error(
            "control startup replaced an unrelated endpoint file"
        );
    }
}

void test_initial_changes_and_transient_probes() {
    for (const bool existing : {true, false}) {
        test::TemporaryDirectory temporary;
        const auto root = temporary.path() / "root";
        if (existing) {
            std::filesystem::create_directory(root);
        }
        std::atomic_int runs{0};
        std::atomic_bool entered{false};
        std::atomic_bool release{false};
        std::exception_ptr failure;
        monitor::Monitor instance{
            root,
            [&](const std::stop_token& token) {
                const auto count = ++runs;
                if (count == 1) {
                    std::filesystem::create_directories(root);
                    entered = true;
                    while (!release && !token.stop_requested()) {
                        std::this_thread::sleep_for(5ms);
                    }
                }
                static_cast<void>(sync::detail::FilesystemMetadata::detect(
                    config::Config::defaults().filesystem_metadata, root
                ));
                return exit_code::success;
            },
            30s,
            10ms
        };
        std::jthread worker{[&](std::stop_token token) {
            try {
                static_cast<void>(instance.run(token));
            } catch (...) {
                failure = std::current_exception();
            }
        }};
        wait_until([&] { return entered.load(); });
        test::write_file(root / "initial-edit", "changed during initial round");
        release = true;
        wait_until([&] { return runs.load() >= 2; });
        std::this_thread::sleep_for(100ms);
        worker.request_stop();
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (runs != 2) {
            throw std::runtime_error(
                "initial changes were lost or transient metadata probes caused "
                "a sync loop"
            );
        }
    }
}

void test_endpoint_validation_and_cleanup() {
    test::TemporaryDirectory temporary;
    const auto state = temporary.path() / "state";
    const auto root = temporary.path() / "root";
    app::detail::secure_state_directory(state);
    const auto path = state / "monitor-control.sock";
    if (monitor::find_control(state) ||
        app::detail::inspect_runtime_owner(state) ||
        std::filesystem::exists(state / "onedrive-cpp.lock")) {
        throw std::runtime_error(
            "offline Monitor discovery created a runtime lock"
        );
    }
    {
        monitor::ControlServer server{state, root};
        struct stat permissions{};
        if (::lstat(path.c_str(), &permissions) == -1 ||
            (permissions.st_mode & 07777) != 0600) {
            throw std::runtime_error(
                "Monitor control socket did not restrict access"
            );
        }
        if (!test::throws_with<std::runtime_error>(
                [&] { monitor::ControlServer duplicate{state, root}; }, "active"
            )) {
            throw std::runtime_error(
                "second server replaced an active endpoint"
            );
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
        if (!test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(control_status(state)); }, "0600"
            )) {
            throw std::runtime_error(
                "client accepted unsafe socket permissions"
            );
        }
        std::filesystem::remove(path);
        test::write_file(path, "replacement");
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(
            "Monitor cleanup deleted a replacement endpoint"
        );
    }
    std::filesystem::remove(path);
    std::filesystem::create_symlink(temporary.path() / "unrelated", path);
    if (!test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(control_status(state)); }, "owned socket"
        )) {
        throw std::runtime_error("client followed a symlink endpoint");
    }
    std::filesystem::remove(path);
    std::filesystem::permissions(
        state,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read
    );
    if (!test::throws_with<std::runtime_error>(
            [&] { monitor::ControlServer unsafe{state, root}; }, "0700"
        )) {
        throw std::runtime_error(
            "server accepted unsafe state-directory permissions"
        );
    }
    app::detail::secure_state_directory(state);
    const auto long_directory = state / std::string(110, 'x');
    app::detail::secure_state_directory(long_directory);
    if (!test::throws_with<std::runtime_error>(
            [&] { monitor::ControlServer oversized{long_directory, root}; },
            "too long"
        )) {
        throw std::runtime_error("server truncated its Unix endpoint path");
    }
}

void test_client_protocol_and_timeout() {
    test::TemporaryDirectory temporary;
    const auto state = temporary.path() / "state";
    app::detail::secure_state_directory(state);
    const auto path = state / "monitor-control.sock";
    const Json valid{
        {"version", 2},
        {"ok", true},
        {"acknowledgement", "status"},
        {"state", "waiting"},
        {"pid", ::getpid()},
        {"instance_id", "test-instance"},
        {"sync_directory", "/sync"},
        {"pending", false},
        {"completed_rounds", 0},
        {"last_result", nullptr},
    };
    for (const std::string field :
         {"version",
          "state",
          "pid",
          "instance_id",
          "empty_instance_id",
          "sync_directory",
          "completed_rounds",
          "last_result",
          "acknowledgement",
          "pending",
          "unexpected_field",
          "oversized",
          "not_json",
          "closed",
          "timeout"}) {
        util::UniqueFD listener{
            ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)
        };
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = path.string();
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        if (::bind(
                listener.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)
            ) == -1 ||
            ::chmod(path.c_str(), 0600) == -1 ||
            ::listen(listener.get(), 1) == -1) {
            throw std::runtime_error("cannot prepare protocol test listener");
        }
        std::exception_ptr failure;
        std::jthread worker{[&] {
            try {
                pollfd ready{listener.get(), POLLIN, 0};
                if (::poll(&ready, 1, 4000) != 1) {
                    throw std::runtime_error(
                        "client did not connect to protocol test listener"
                    );
                }
                util::UniqueFD client{
                    ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC)
                };
                std::array<char, 4096> request{};
                if (!client ||
                    ::recv(client.get(), request.data(), request.size(), 0) <=
                        0) {
                    throw std::runtime_error(
                        "client did not send its protocol request"
                    );
                }
                if (field == "timeout") {
                    std::this_thread::sleep_for(2200ms);
                    return;
                }
                if (field == "closed") {
                    return;
                }
                Json reply = valid;
                if (field == "instance_id" || field == "empty_instance_id") {
                    reply["instance_id"] =
                        field == "instance_id" ? Json(1) : Json("");
                } else {
                    reply[field] =
                        field == "last_result" ? Json(std::uint64_t{2147483648})
                        : field == "pid" || field == "completed_rounds"
                            ? Json(-1)
                            : Json("invalid");
                }
                const auto content =
                    field == "oversized"  ? std::string(4097, 'x')
                    : field == "not_json" ? std::string{"not JSON"}
                                          : reply.dump();
                if (::send(
                        client.get(),
                        content.data(),
                        content.size(),
                        MSG_NOSIGNAL
                    ) != static_cast<ssize_t>(content.size())) {
                    throw std::runtime_error("cannot send test protocol reply");
                }
            } catch (...) {
                failure = std::current_exception();
            }
        }};
        bool rejected = false;
        try {
            static_cast<void>(control_status(state));
        } catch (const std::exception& error) {
            const std::string_view detail{error.what()};
            rejected = field == "timeout"     ? detail.contains("timed out")
                       : field == "oversized" ? detail.contains("oversized")
                       : field == "not_json"  ? detail.contains("parse_error")
                       : field == "closed" ? detail.contains("disconnected") ||
                                                 detail.contains("empty")
                                           : true;
        }
        worker.join();
        std::filesystem::remove(path);
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!rejected) {
            throw std::runtime_error(
                "client accepted an invalid Monitor reply or hid a timeout"
            );
        }
    }
}

void test_same_process_instance_replacement() {
    test::TemporaryDirectory temporary;
    const auto state = temporary.path() / "state";
    const auto root = temporary.path() / "root";
    app::detail::secure_state_directory(state);
    const auto lock = app::detail::acquire_runtime_lock(state, "monitor");
    monitor::ControlTarget previous;
    {
        monitor::ControlServer server{state, root};
        previous = control_status(state).target();
    }
    monitor::ControlServer replacement{state, root};
    const auto current = control_status(state);
    if (current.pid != previous.pid ||
        current.instance_id == previous.instance_id) {
        throw std::runtime_error(
            "Monitor replacement reused an instance identity"
        );
    }
    if (!test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(monitor::request_control(
                    state,
                    monitor::ControlCommand::trigger,
                    monitor::ControlTarget{current.pid, ""}
                ));
            },
            "owner changed"
        ) ||
        control_status(state).pending) {
        throw std::runtime_error(
            "an empty target identity controlled a Monitor"
        );
    }
    for (const auto command :
         {monitor::ControlCommand::stop, monitor::ControlCommand::trigger}) {
        if (!test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        monitor::request_control(state, command, previous)
                    );
                },
                "owner changed"
            ) ||
            control_status(state).state != "starting" ||
            control_status(state).pending) {
            throw std::runtime_error(
                "a confirmed target controlled a newer Monitor in the same "
                "process"
            );
        }
    }
    if (monitor::request_control(
            state, monitor::ControlCommand::trigger, current.target()
        )
            .acknowledgement != "accepted") {
        throw std::runtime_error(
            "a confirmed target controlled a newer Monitor in the same process"
        );
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 4 && std::string_view{argv[1]} == "--child") {
            return child(argv[2], argv[3]);
        }
        test_process_control();
        test_cli_and_restart();
        const logging::Session restore{config::Config::defaults().logging};
        test_initial_changes_and_transient_probes();
        test_endpoint_validation_and_cleanup();
        test_client_protocol_and_timeout();
        test_same_process_instance_replacement();
    } catch (const std::exception& error) {
        return test::fail(error.what());
    }
    return EXIT_SUCCESS;
}
