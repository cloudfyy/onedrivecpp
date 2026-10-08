#include "cli/backend_factory.hpp"
#include "onedrive/cli/console.hpp"
#include "onedrive/util/system_error.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"
#include "support/common.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

using onedrive::test::wait_until;
using onedrive::util::throw_errno_error;
using onedrive::util::UniqueFD;

class TerminalPager {
public:
    explicit TerminalPager(
        int pages,
        unsigned short rows = 30,
        int redirected_descriptor = -1,
        onedrive::cli::TuiView view = onedrive::cli::TuiView::drives
    )
        : master_{::posix_openpt(O_RDWR | O_NOCTTY)},
          output_redirected_{redirected_descriptor == STDOUT_FILENO} {
        if (!master_ || ::grantpt(master_.get()) == -1 ||
            ::unlockpt(master_.get()) == -1) {
            throw_errno_error("cannot create paging test terminal");
        }
        const char* name = ::ptsname(master_.get());
        if (name == nullptr) {
            throw_errno_error("cannot locate paging test terminal");
        }
        slave_.reset(::open(name, O_RDWR | O_NOCTTY));
        if (!slave_ || ::tcgetattr(slave_.get(), &original_) == -1) {
            throw_errno_error("cannot inspect paging test terminal");
        }
        const winsize size{.ws_row = rows, .ws_col = 100};
        if (::ioctl(slave_.get(), TIOCSWINSZ, &size) == -1) {
            throw_errno_error("cannot resize paging test terminal");
        }
        child_ = ::fork();
        if (child_ == -1) {
            throw_errno_error("cannot fork paging test");
        }
        if (child_ == 0) {
            run_child(pages, redirected_descriptor, view);
        }
    }

    ~TerminalPager() {
        if (child_ > 0) {
            static_cast<void>(::kill(child_, SIGKILL));
            while (::waitpid(child_, nullptr, 0) == -1 && errno == EINTR) {
            }
        }
    }

    TerminalPager(const TerminalPager&) = delete;
    TerminalPager& operator=(const TerminalPager&) = delete;

    void expect_page(int page, int pages) {
        const auto counter =
            "Drive " + std::to_string(page) + "/" + std::to_string(pages);
        const auto name = "Test-drive-" + std::to_string(page);
        expect_result(counter, name);
    }

    void expect_result(
        std::string_view title = "Inspection result retained",
        std::string_view detail = {}
    ) {
        if (!wait_until(
                [&] {
                    drain();
                    return output_.contains(title) &&
                           output_.contains(detail) &&
                           output_.contains("Press Enter to exit (or q)");
                },
                std::chrono::seconds{5}
            )) {
            throw std::runtime_error(
                "inspection test did not display " + std::string{title} + "\n" +
                output_
            );
        }
    }

    void send(std::string_view keys) {
        output_.clear();
        if (::write(master_.get(), keys.data(), keys.size()) !=
            static_cast<ssize_t>(keys.size())) {
            throw std::runtime_error("cannot send paging test keys");
        }
    }

    void expect_exit(bool interrupted = false) {
        int status = 0;
        if (!wait_until(
                [&] {
                    drain();
                    const auto result = ::waitpid(child_, &status, WNOHANG);
                    if (result == -1) {
                        if (errno == EINTR) {
                            return false;
                        }
                        throw_errno_error("cannot wait for paging test");
                    }
                    return result == child_;
                },
                std::chrono::seconds{5}
            )) {
            throw std::runtime_error("paging test did not exit");
        }
        child_ = -1;
        drain();
        termios restored{};
        const bool expected_status =
            interrupted ? WIFSIGNALED(status) && WTERMSIG(status) == SIGINT
                        : WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!expected_status ||
            (!output_redirected_ && !output_.contains("\033[?1049l")) ||
            ::tcgetattr(slave_.get(), &restored) == -1 ||
            restored.c_lflag != original_.c_lflag ||
            restored.c_iflag != original_.c_iflag ||
            restored.c_oflag != original_.c_oflag) {
            throw std::runtime_error(
                "paging did not restore terminal state\n" + output_
            );
        }
    }

private:
    [[noreturn]] void run_child(
        int pages, int redirected_descriptor, onedrive::cli::TuiView view
    ) {
        for (const int descriptor :
             {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
            if (::dup2(slave_.get(), descriptor) == -1) {
                std::_Exit(EXIT_FAILURE);
            }
        }
        master_.reset();
        slave_.reset();
        if (::setenv("TERM", "xterm-256color", 1) == -1) {
            std::_Exit(EXIT_FAILURE);
        }
        int result = EXIT_SUCCESS;
        try {
            if (redirected_descriptor != -1) {
                const UniqueFD null{::open("/dev/null", O_RDWR)};
                if (!null || ::dup2(null.get(), redirected_descriptor) == -1) {
                    throw_errno_error("cannot redirect paging test stream");
                }
            }
            using namespace onedrive::cli;
            const Console console{detail::make_ftxui_console_backend(
                {.color = ColorMode::never, .ui = UiMode::tui, .view = view},
                std::cout,
                std::cerr,
                100,
                30
            )};
            if (pages == 0) {
                console.message(
                    MessageKind::success, "result", "Inspection result retained"
                );
            }
            for (int page = 1; page <= pages; ++page) {
                console.section(
                    "drive",
                    "OneDrive drive:",
                    {
                        {.label = "name:",
                         .key = "name",
                         .value = "Test-drive-" + std::to_string(page)},
                        {.label = "known files:",
                         .key = "known_files",
                         .value = std::to_string(page * 10)},
                    }
                );
            }
            console.finish();
            console.finish();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            result = EXIT_FAILURE;
        }
        std::cout << std::flush;
        std::cerr << std::flush;
        std::exit(result);
    }

    void drain() {
        pollfd descriptor{.fd = master_.get(), .events = POLLIN, .revents = 0};
        const int ready = ::poll(&descriptor, 1, 10);
        if (ready == -1) {
            if (errno == EINTR) {
                return;
            }
            throw_errno_error("cannot poll paging output");
        }
        if (ready == 0) {
            return;
        }
        std::array<char, 32768> buffer{};
        const auto size = ::read(master_.get(), buffer.data(), buffer.size());
        if (size == -1) {
            throw_errno_error("cannot read paging output");
        }
        output_.append(buffer.data(), static_cast<std::size_t>(size));
    }

    UniqueFD master_;
    UniqueFD slave_;
    pid_t child_{-1};
    termios original_{};
    std::string output_;
    bool output_redirected_;
};

} // namespace

int main() {
    try {
        using onedrive::cli::TuiView;
        for (const auto view : {
                 TuiView::health,
                 TuiView::status,
                 TuiView::drives,
                 TuiView::shared,
                 TuiView::sites,
                 TuiView::quota,
                 TuiView::storage,
                 TuiView::partials,
                 TuiView::files,
                 TuiView::verify,
                 TuiView::config,
             }) {
            for (const auto* key : {"\r", "q", "Q"}) {
                TerminalPager terminal{0, 30, -1, view};
                terminal.expect_result();
                terminal.send(key);
                terminal.expect_exit();
            }
        }
        {
            TerminalPager pager{3, 12};
            pager.expect_page(1, 3);
            pager.send("n");
            pager.expect_page(2, 3);
            pager.send("\033[C");
            pager.expect_page(3, 3);
            pager.send("p");
            pager.expect_page(2, 3);
            pager.send("\033[D");
            pager.expect_page(1, 3);
            pager.send("q");
            pager.expect_exit();
        }
        {
            TerminalPager pager{3};
            pager.expect_page(1, 3);
            pager.send("\r");
            pager.expect_exit();
        }
        {
            TerminalPager pager{1};
            pager.expect_page(1, 1);
            pager.send("q");
            pager.expect_exit();
        }
        {
            TerminalPager pager{3};
            pager.expect_page(1, 3);
            pager.send("\003");
            pager.expect_exit(true);
        }
        for (const auto descriptor : {STDIN_FILENO, STDOUT_FILENO}) {
            TerminalPager pager{3, 30, descriptor};
            if (descriptor == STDIN_FILENO) {
                pager.expect_page(1, 3);
            } else {
                pager.send("\n");
            }
            pager.expect_exit();
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
