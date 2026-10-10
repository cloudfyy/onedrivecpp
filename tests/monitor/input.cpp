#include "monitor/input.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "support/common.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <thread>
#include <system_error>
#include <string_view>
#include <termios.h>
#include <unistd.h>

namespace {

using onedrive::test::fail;

int interrupted_descriptor = -1;
unsigned interruptions = 0;

bool exits_for(std::string_view keys, bool interrupt = false) {
    std::array<int, 2> pipe_descriptors{};
    if (::pipe(pipe_descriptors.data()) == -1) {
        return false;
    }
    const auto written = ::write(
        pipe_descriptors[1], keys.data(), keys.size()
    );
    static_cast<void>(::close(pipe_descriptors[1]));
    if (written != static_cast<ssize_t>(keys.size())) {
        static_cast<void>(::close(pipe_descriptors[0]));
        return false;
    }
    const onedrive::monitor::detail::KeyboardInput input{
        pipe_descriptors[0], false
    };
    if (interrupt) {
        interrupted_descriptor = pipe_descriptors[0];
    }
    const bool requested = input.exit_requested();
    static_cast<void>(::close(pipe_descriptors[0]));
    return requested;
}

int test_terminal_mode() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master == -1 ||
        ::grantpt(master) == -1 ||
        ::unlockpt(master) == -1) {
        if (master != -1) {
            static_cast<void>(::close(master));
        }
        return fail("could not create monitor keyboard pseudo-terminal");
    }
    const char* slave_name = ::ptsname(master);
    const int slave =
        slave_name == nullptr ? -1 : ::open(slave_name, O_RDWR | O_NOCTTY);
    if (slave == -1) {
        static_cast<void>(::close(master));
        return fail("could not open monitor keyboard pseudo-terminal");
    }

    termios original{};
    termios active{};
    termios restored{};
    if (::tcgetattr(slave, &original) == -1) {
        static_cast<void>(::close(slave));
        static_cast<void>(::close(master));
        return fail("could not inspect pseudo-terminal mode");
    }
    bool requested = false;
    bool raw_mode_ok = false;
    {
        const onedrive::monitor::detail::KeyboardInput input{slave, true};
        raw_mode_ok =
            ::tcgetattr(slave, &active) == 0 &&
            (active.c_lflag & (ICANON | ECHO)) == 0 &&
            ::write(master, "q", 1) == 1;
        if (raw_mode_ok) {
            requested = input.exit_requested();
        }
    }
    const bool restored_ok =
        ::tcgetattr(slave, &restored) == 0 &&
        restored.c_lflag == original.c_lflag;
    static_cast<void>(::close(slave));
    static_cast<void>(::close(master));
    if (!raw_mode_ok || !requested || !restored_ok) {
        return fail("monitor keyboard did not restore terminal mode");
    }
    return EXIT_SUCCESS;
}

int test_input_errors() {
    const onedrive::monitor::detail::KeyboardInput disabled{-1, true};
    if (disabled.descriptor() != -1) {
        return fail("disabled monitor keyboard changed its descriptor");
    }

    const int non_terminal = ::open("/dev/null", O_RDONLY);
    bool terminal_error = false;
    try {
        const onedrive::monitor::detail::KeyboardInput input{
            non_terminal,
            true
        };
    } catch (const std::system_error&) {
        terminal_error = true;
    }
    static_cast<void>(::close(non_terminal));

    std::array<int, 2> descriptors{};
    if (::pipe(descriptors.data()) == -1 ||
        ::fcntl(descriptors[0], F_SETFL, O_NONBLOCK) == -1) {
        return fail("could not create nonblocking monitor input");
    }
    const onedrive::monitor::detail::KeyboardInput empty{
        descriptors[0],
        false
    };
    const bool empty_requested = empty.exit_requested();
    static_cast<void>(::close(descriptors[0]));
    bool read_error = false;
    try {
        static_cast<void>(empty.exit_requested());
    } catch (const std::system_error&) {
        read_error = true;
    }
    static_cast<void>(::close(descriptors[1]));

    if (!terminal_error || empty_requested || !read_error) {
        return fail("monitor keyboard input errors were not surfaced");
    }
    return EXIT_SUCCESS;
}

int test_monitor_keyboard_exit(char key) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master == -1 ||
        ::grantpt(master) == -1 ||
        ::unlockpt(master) == -1) {
        if (master != -1) {
            static_cast<void>(::close(master));
        }
        return fail("could not create monitor loop pseudo-terminal");
    }
    const char* slave_name = ::ptsname(master);
    const int slave =
        slave_name == nullptr ? -1 : ::open(slave_name, O_RDWR | O_NOCTTY);
    const int saved_input = ::dup(STDIN_FILENO);
    if (slave == -1 || saved_input == -1 ||
        ::dup2(slave, STDIN_FILENO) == -1) {
        if (slave != -1) {
            static_cast<void>(::close(slave));
        }
        if (saved_input != -1) {
            static_cast<void>(::close(saved_input));
        }
        static_cast<void>(::close(master));
        return fail("could not redirect monitor loop input");
    }
    static_cast<void>(::close(slave));

    onedrive::test::TemporaryDirectory root;
    int synchronization_count = 0;
    const onedrive::monitor::Monitor monitor{
        root.path(),
        [&synchronization_count](const std::stop_token&) {
            ++synchronization_count;
            return 0;
        },
        std::chrono::hours{1},
        std::chrono::milliseconds{10}
    };
    std::jthread writer{[master, key] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        static_cast<void>(::write(master, &key, 1));
    }};
    const int result = monitor.run(true);
    writer.join();

    const bool restored =
        ::dup2(saved_input, STDIN_FILENO) != -1;
    static_cast<void>(::close(saved_input));
    static_cast<void>(::close(master));
    if (result != 0 || synchronization_count != 1 || !restored) {
        return fail("monitor did not stop cleanly from a keyboard exit");
    }
    return EXIT_SUCCESS;
}

int test_keyboard_exit_during_initial_sync() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master == -1 ||
        ::grantpt(master) == -1 ||
        ::unlockpt(master) == -1) {
        if (master != -1) {
            static_cast<void>(::close(master));
        }
        return fail("could not create initial-sync keyboard pseudo-terminal");
    }
    const char* slave_name = ::ptsname(master);
    const int slave =
        slave_name == nullptr ? -1 : ::open(slave_name, O_RDWR | O_NOCTTY);
    const int saved_input = ::dup(STDIN_FILENO);
    if (slave == -1 || saved_input == -1 ||
        ::dup2(slave, STDIN_FILENO) == -1) {
        if (slave != -1) {
            static_cast<void>(::close(slave));
        }
        if (saved_input != -1) {
            static_cast<void>(::close(saved_input));
        }
        static_cast<void>(::close(master));
        return fail("could not redirect initial-sync keyboard input");
    }
    static_cast<void>(::close(slave));

    onedrive::test::TemporaryDirectory root;
    bool raw_during_sync = false;
    const onedrive::monitor::Monitor monitor{
        root.path(),
        [&raw_during_sync](const std::stop_token&) {
            termios active{};
            raw_during_sync =
                ::tcgetattr(STDIN_FILENO, &active) == 0 &&
                (active.c_lflag & ICANON) == 0;
            std::this_thread::sleep_for(std::chrono::milliseconds{150});
            return 0;
        },
        std::chrono::hours{1},
        std::chrono::milliseconds{10}
    };
    std::jthread writer{[master] {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        static_cast<void>(::write(master, "q", 1));
        std::this_thread::sleep_for(std::chrono::milliseconds{475});
        static_cast<void>(::write(master, "\n", 1));
    }};
    const auto started = std::chrono::steady_clock::now();
    const int result = monitor.run(true);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const bool restored = ::dup2(saved_input, STDIN_FILENO) != -1;
    static_cast<void>(::close(saved_input));
    writer.join();
    static_cast<void>(::close(master));
    if (result != 0 || !restored || !raw_during_sync ||
        elapsed >= std::chrono::milliseconds{400}) {
        return fail("monitor lost keyboard exit during initial sync");
    }
    return EXIT_SUCCESS;
}

}  // namespace

extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" ssize_t __wrap_read(int descriptor, void* buffer, std::size_t size) {
    if (descriptor == interrupted_descriptor && descriptor >= 0) {
        interrupted_descriptor = -1;
        ++interruptions;
        errno = EINTR;
        return -1;
    }
    return __real_read(descriptor, buffer, size);
}

int main() {
    if (!exits_for("q") || !exits_for("Q") || !exits_for("\033") ||
        !exits_for("xq") || exits_for("x") || exits_for("") ||
        !exits_for("q", true) || interruptions != 1) {
        return fail("monitor keyboard exit keys were handled incorrectly");
    }
    if (const int result = test_terminal_mode();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_input_errors();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_monitor_keyboard_exit('q');
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_monitor_keyboard_exit('\033');
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_keyboard_exit_during_initial_sync();
}
