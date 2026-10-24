#include "support/progress.hpp"
#include "app/checks.hpp"
#include "app/lock.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "support/common.hpp"
#include "app/support.hpp"
#include "onedrive/app/synchronization.hpp"
#include "onedrive/app/monitoring.hpp"
#include "onedrive/ui/cli/console.hpp"
#include "onedrive/logging/logging.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sstream>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace onedrive;
using test::fail;
using test::TemporaryDirectory;

class Observer final : public events::Observer {
public:
    explicit Observer(std::function<void(const events::Event&)> callback = {})
        : callback_{std::move(callback)} {
    }
    mutable bool reported{false};

private:
    void on_event(const events::Event& event) const override {
        if (const auto* section = std::get_if<events::SectionEvent>(&event)) {
            reported = section->event == "runtime_busy";
        }
        if (callback_) {
            callback_(event);
        }
    }
    std::function<void(const events::Event&)> callback_;
};

void send_byte(int fd, char value) {
    if (::write(fd, &value, 1) != 1) {
        throw std::runtime_error("cannot signal lock subprocess");
    }
}

char receive_byte(int fd) {
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, 5000) != 1) {
        throw std::runtime_error("lock subprocess did not respond");
    }
    char value{};
    if (::read(fd, &value, 1) != 1) {
        throw std::runtime_error("lock subprocess closed its pipe");
    }
    return value;
}

int wait_child(pid_t pid) {
    int status{};
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error("cannot wait for lock subprocess");
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

class Child {
public:
    Child(
        std::string mode,
        const std::filesystem::path& root,
        const std::filesystem::path& state,
        std::string operation
    ) {
        std::array<int, 2> ready{};
        std::array<int, 2> release{};
        if (::pipe(ready.data()) == -1) {
            throw std::runtime_error(
                "cannot create lock subprocess ready pipe"
            );
        }
        if (::pipe(release.data()) == -1) {
            ::close(ready[0]);
            ::close(ready[1]);
            throw std::runtime_error(
                "cannot create lock subprocess release pipe"
            );
        }
        const auto ready_arg = std::to_string(ready[1]);
        const auto release_arg = std::to_string(release[0]);
        const auto root_arg = root.string();
        const auto state_arg = state.string();
        pid_ = ::fork();
        if (pid_ == 0) {
            ::close(ready[0]);
            ::close(release[1]);
            ::execl(
                "/proc/self/exe",
                "sync_resource_lock_tests",
                mode.c_str(),
                root_arg.c_str(),
                state_arg.c_str(),
                operation.c_str(),
                ready_arg.c_str(),
                release_arg.c_str(),
                nullptr
            );
            ::_exit(127);
        }
        ::close(ready[1]);
        ::close(release[0]);
        ready_.reset(ready[0]);
        release_.reset(release[1]);
        if (pid_ == -1) {
            throw std::runtime_error("cannot fork lock subprocess");
        }
    }

    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) == -1 && errno == EINTR) {
            }
        }
    }

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    char ready() {
        return receive_byte(ready_.get());
    }
    void release() {
        send_byte(release_.get(), '1');
    }
    pid_t pid() const {
        return pid_;
    }
    int wait() {
        const auto result = wait_child(pid_);
        pid_ = -1;
        return result;
    }
    void crash() {
        if (::kill(pid_, SIGKILL) == -1) {
            throw std::runtime_error("cannot terminate lock subprocess");
        }
        static_cast<void>(wait());
    }

private:
    pid_t pid_{-1};
    util::UniqueFD ready_;
    util::UniqueFD release_;
};

int child_main(int argc, char* argv[]) {
    if (argc != 7) {
        return 4;
    }
    const std::string mode = argv[1];
    const std::filesystem::path root = argv[2];
    const std::filesystem::path state = argv[3];
    const std::string operation = argv[4];
    const int ready = std::stoi(argv[5]);
    const int release = std::stoi(argv[6]);
    bool announced = false;
    const Observer observer{[&](const events::Event& event) {
        const auto* phase = std::get_if<events::SyncPhaseEvent>(&event);
        if (mode == "service" && !announced && phase &&
            phase->phase == (operation == "monitor"
                                 ? events::SyncPhase::waiting
                                 : events::SyncPhase::preparing)) {
            announced = true;
            send_byte(ready, 'A');
            if (receive_byte(release) != '1') {
                throw std::runtime_error("service subprocess was not released");
            }
        }
    }};
    try {
        if (mode == "race") {
            send_byte(ready, 'R');
            if (receive_byte(release) != '1') {
                return 4;
            }
        }
        auto state_lock = app::detail::acquire_runtime_lock(state, operation);
        if (mode == "service") {
            auto config = config::Config::defaults();
            config.state_directory = state;
            config.sync_data_directory = root;
            config.dry_run = true;
            test::app::FakeRuntimeFactory implementation;
            const app::RuntimeFactory factory{
                util::borrowed_proxy, implementation
            };
            const int result =
                operation == "monitor"
                    ? app::monitor_account(config, factory, observer, false)
                    : app::synchronize_account(config, factory, observer);
            return result == exit_code::success && announced ? 0 : 4;
        }
        auto root_lock = app::detail::acquire_sync_directory_lock(
            root, state, operation, observer
        );
        send_byte(ready, 'A');
        if (receive_byte(release) != '1') {
            return 4;
        }
        if (mode == "exec") {
            ::execl(
                "/proc/self/exe",
                "sync_resource_lock_tests",
                "hold",
                argv[2],
                argv[3],
                argv[4],
                argv[5],
                argv[6],
                nullptr
            );
            return 127;
        }
        return 0;
    } catch (const app::detail::RuntimeBusyError& error) {
        if (error.resource != "state_directory" && !observer.reported) {
            return 4;
        }
        send_byte(ready, 'B');
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        send_byte(ready, 'E');
        return 3;
    }
}

int test_exclusion() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "data" / "root";
    const auto state1 = temporary.path() / "state1";
    const auto state2 = temporary.path() / "state2";
    std::filesystem::create_directories(root / "child");
    app::detail::secure_state_directory(state1);
    app::detail::secure_state_directory(state2);
    for (const auto& pair : {
             std::pair{"synchronize", "synchronize"},
             std::pair{"synchronize", "monitor"},
             std::pair{"monitor", "monitor"},
             std::pair{"monitor", "synchronize"},
         }) {
        Child holder{"hold", root, state1, pair.first};
        if (holder.ready() != 'A') {
            return fail("initial subprocess could not claim a sync root");
        }
        for (const auto& requested : {
                 root,
                 root / "child",
                 root.parent_path(),
                 root / "child" / "..",
             }) {
            Child contender{"hold", requested, state2, pair.second};
            if (contender.ready() != 'B' || contender.wait() != 2) {
                return fail(
                    "subprocess accepted an identical/overlapping root"
                );
            }
        }
        Child same_state{
            "hold", temporary.path() / "other", state1, pair.second
        };
        if (same_state.ready() != 'B' || same_state.wait() != 2) {
            return fail("subprocess accepted the same active state directory");
        }
        Child sibling{
            "hold", temporary.path() / "sibling", state2, pair.second
        };
        if (sibling.ready() != 'A') {
            return fail("independent synchronization roots were blocked");
        }
        sibling.release();
        if (sibling.wait() != 0) {
            return fail("independent lock subprocess failed to stop");
        }
        const Observer observer;
        try {
            const auto lock = app::detail::acquire_sync_directory_lock(
                root, state2, pair.second, observer
            );
            return fail("live subprocess was not treated as the lock owner");
        } catch (const app::detail::RuntimeBusyError& error) {
            if (error.owner.pid != holder.pid() ||
                error.owner.operation != pair.first ||
                error.owner.started_at.empty() ||
                error.owner.sync_directory != root ||
                error.owner.state_directory != state1 || !observer.reported) {
                return fail("busy error lost safe owner metadata");
            }
        }
        holder.release();
        if (holder.wait() != 0) {
            return fail("holder did not release synchronization locks");
        }
    }
    Child crashed{"hold", root, state1, "monitor"};
    if (crashed.ready() != 'A') {
        return fail("crash test could not acquire locks");
    }
    crashed.crash();
    Child replacement{"hold", root, state1, "synchronize"};
    if (replacement.ready() != 'A') {
        return fail("process crash did not release OS locks");
    }
    replacement.release();
    if (replacement.wait() != 0) {
        return fail("replacement failed");
    }
    Child executed{"exec", root, state1, "monitor"};
    if (executed.ready() != 'A') {
        return fail("exec test could not acquire locks");
    }
    executed.release();
    if (executed.ready() != 'A') {
        return fail("exec inherited synchronization lock descriptors");
    }
    executed.release();
    return executed.wait() == 0 ? EXIT_SUCCESS
                                : fail("exec replacement failed");
}

int test_races_and_missing_roots() {
    TemporaryDirectory temporary;
    const auto state1 = temporary.path() / "state1";
    const auto state2 = temporary.path() / "state2";
    app::detail::secure_state_directory(state1);
    app::detail::secure_state_directory(state2);
    for (int iteration = 0; iteration < 8; ++iteration) {
        const auto root =
            temporary.path() / ("missing-" + std::to_string(iteration));
        Child first{"race", root, state1, "synchronize"};
        Child second{"race", root / "child", state2, "monitor"};
        if (first.ready() != 'R' || second.ready() != 'R') {
            return fail("race subprocesses were not ready");
        }
        first.release();
        second.release();
        const char a = first.ready();
        const char b = second.ready();
        if (!((a == 'A' && b == 'B') || (a == 'B' && b == 'A')) ||
            std::filesystem::exists(root)) {
            return fail(
                "atomic registration race or dry-run root creation failed"
            );
        }
        if (a == 'A') {
            first.release();
        } else {
            second.release();
        }
        const int first_status = first.wait();
        const int second_status = second.wait();
        if ((a == 'A' && (first_status != 0 || second_status != 2)) ||
            (b == 'A' && (second_status != 0 || first_status != 2))) {
            return fail("registration race did not retain exactly one owner");
        }
    }
    const auto missing_root = temporary.path() / "missing-alias";
    Child missing{"hold", missing_root, state1, "monitor"};
    if (missing.ready() != 'A') {
        return fail("missing-root alias test could not acquire a root");
    }
    Child missing_alias{
        "hold", missing_root / "child" / "..", state2, "synchronize"
    };
    if (missing_alias.ready() != 'B' || missing_alias.wait() != 2 ||
        std::filesystem::exists(missing_root)) {
        return fail("missing directory alias bypassed lexical root ownership");
    }
    missing.release();
    if (missing.wait() != 0) {
        return fail("missing-root holder failed to release its lease");
    }
    const auto root = temporary.path() / "root";
    std::filesystem::create_directory(root);
    const auto link = temporary.path() / "alias";
    std::filesystem::create_directory_symlink(root, link);
    const Observer observer;
    if (!test::throws_with<std::runtime_error>(
            [&] {
                const auto lock = app::detail::acquire_sync_directory_lock(
                    link, state1, "synchronize", observer
                );
            },
            "symbolic link"
        )) {
        return fail("symbolic-link synchronization root bypassed validation");
    }
    const auto state_link = temporary.path() / "state-alias";
    std::filesystem::create_directory_symlink(state1, state_link);
    if (!test::throws_with<std::runtime_error>(
            [&] {
                const auto lock = app::detail::acquire_runtime_lock(
                    state_link, "synchronize"
                );
            },
            "cannot safely open path"
        )) {
        return fail("runtime lock opening followed a symbolic-link parent");
    }
    Child holder{"hold", root, state1, "monitor"};
    if (holder.ready() != 'A') {
        return fail("filesystem-identity test could not acquire a root");
    }
    const auto renamed = temporary.path() / "renamed";
    std::filesystem::rename(root, renamed);
    Child alias{"hold", renamed, state2, "synchronize"};
    if (alias.ready() != 'B' || alias.wait() != 2) {
        return fail("filesystem identity alias bypassed an active root lock");
    }
    holder.release();
    if (holder.wait() != 0) {
        return fail("filesystem-identity holder failed to release its root");
    }
    return EXIT_SUCCESS;
}

int test_shared_services_and_json() {
    TemporaryDirectory temporary;
    const auto base = temporary.path() / "data";
    const auto state1 = temporary.path() / "state1";
    const auto state2 = temporary.path() / "state2";
    app::detail::secure_state_directory(state1);
    app::detail::secure_state_directory(state2);
    std::filesystem::create_directory(base);
    const auto identity = test::test_drive_identity("me");
    static_cast<void>(
        account::AccountState::activate(state2, identity, "test-refresh-token")
    );
    const auto config_file = temporary.path() / "config.toml";
    test::write_file(
        config_file,
        "config_version = 2\n[sync]\ndry_run = true\ndata_directory = \"" +
            base.string() + "\"\n[state]\ndirectory = \"" + state2.string() +
            "\"\n[auth]\napplication_id = \"test-client\"\n"
    );
    const auto actual_root =
        account::AccountState::drive_data_directory(base, identity);
    for (const auto operation : {"synchronize", "monitor"}) {
        Child service{"service", base, state1, operation};
        if (service.ready() != 'A') {
            return fail(
                "shared synchronization service did not reach its held phase"
            );
        }
        Child contender{"hold", actual_root, state2, "synchronize"};
        if (contender.ready() != 'B' || contender.wait() != 2) {
            return fail(
                "service released the actual Drive-root lock while "
                "active/waiting"
            );
        }
        test::app::FakeRuntimeFactory implementation;
        const app::RuntimeFactory factory{util::borrowed_proxy, implementation};
        auto configuration = config::Config::defaults();
        configuration.state_directory = state2;
        configuration.sync_data_directory = base;
        configuration.dry_run = true;
        std::ostringstream output;
        std::ostringstream errors;
        const cli::Console console{
            {.color = cli::ColorMode::never, .output = cli::OutputMode::json},
            output,
            errors
        };
        try {
            static_cast<void>(
                app::synchronize_account(configuration, factory, console)
            );
            return fail(
                "shared service accepted an externally occupied Drive root"
            );
        } catch (const app::detail::RuntimeBusyError&) {
            if (implementation.item_store_count != 0 ||
                implementation.metrics_count != 0 ||
                std::filesystem::exists(actual_root) ||
                !output.str().contains("\"event\":\"runtime_busy\"") ||
                !output.str().contains("\"resource\":\"sync_directory\"") ||
                !output.str().contains(
                    "\"pid\":\"" + std::to_string(service.pid()) + "\""
                ) ||
                !errors.str().contains("\"event\":\"runtime_busy\"")) {
                return fail(
                    "busy service mutated the root or lost structured JSON "
                    "diagnostics"
                );
            }
        }
        for (const std::vector<std::string>& command : {
                 std::vector<std::string>{"transfer", "sync"},
                 std::vector<std::string>{"transfer", "watch"},
                 std::vector<std::string>{
                     "transfer", "download", "Documents/file.txt"
                 },
                 std::vector<std::string>{"state", "cleanup"},
             }) {
            test::app::FakeRuntimeFactory cli_implementation;
            std::vector<std::string> arguments{"onedrive-cpp"};
            arguments.insert(arguments.end(), command.begin(), command.end());
            arguments.insert(
                arguments.end(), {"--config", config_file.string()}
            );
            arguments.insert(arguments.end(), {"--output", "json"});
            const auto result = test::app::run_application(
                cli_implementation, std::move(arguments)
            );
            const logging::Session restore_logger{configuration.logging};
            if (result.exit_code != exit_code::failure ||
                !result.standard_output.contains(
                    "\"event\":\"runtime_busy\""
                ) ||
                !result.standard_output.contains(
                    "\"resource\":\"sync_directory\""
                ) ||
                !result.standard_error.contains("owner PID") ||
                cli_implementation.item_store_count != 0 ||
                cli_implementation.monitor_count != 0 ||
                std::filesystem::exists(actual_root)) {
                return fail(
                    "CLI " + command[0] + " " + command[1] +
                    " bypassed shared root ownership; output: " +
                    result.standard_output + "; error: " + result.standard_error
                );
            }
        }
        service.release();
        if (service.wait() != 0) {
            return fail("shared service failed after releasing its round");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char* argv[]) {
    onedrive::test::ScenarioProgress progress{3};
    if (argc > 1) {
        return child_main(argc, argv);
    }
    try {
        if (progress.run("exclusion", test_exclusion) != EXIT_SUCCESS ||
            progress.run(
                "races_and_missing_roots", test_races_and_missing_roots
            ) != EXIT_SUCCESS ||
            progress.run(
                "shared_services_and_json", test_shared_services_and_json
            ) != EXIT_SUCCESS) {
            return EXIT_FAILURE;
        }
    } catch (const std::exception& error) {
        return fail(error.what());
    }
    return progress.finish();
}
