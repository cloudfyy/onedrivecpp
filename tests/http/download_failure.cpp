#include "onedrive/http/http_client.hpp"
#include "support/common.hpp"
#include "support/network.hpp"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

enum class Fault { none, close, truncate };
Fault fault = Fault::none;
std::filesystem::path destination;
unsigned injected = 0;

bool is_download(int descriptor) {
    struct stat actual{};
    struct stat expected{};
    return ::fstat(descriptor, &actual) == 0 &&
           ::stat(destination.c_str(), &expected) == 0 &&
           actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
}

} // namespace

extern "C" {
int __real_close(int);
int __real_ftruncate(int, off_t);

int __wrap_close(int descriptor) {
    const bool selected = fault == Fault::close && is_download(descriptor);
    const int result = __real_close(descriptor);
    if (selected) {
        ++injected;
        errno = EIO;
        return -1;
    }
    return result;
}

int __wrap_ftruncate(int descriptor, off_t size) {
    if (fault == Fault::truncate && is_download(descriptor)) {
        ++injected;
        errno = EIO;
        return -1;
    }
    return __real_ftruncate(descriptor, size);
}
}

int main() {
    using onedrive::test::fail;
    const onedrive::test::TemporaryDirectory temporary;
    destination = temporary.path() / "download";
    const onedrive::http::CurlHttpClient client;
    enum class Callback {
        none,
        standard,
        unknown,
        streaming,
        rollback_failure
    };
    struct Scenario {
        Fault fault;
        Callback callback;
        bool resumed;
        std::string_view message;
    };
    for (const auto& scenario : {
             Scenario{
                 Fault::none,
                 Callback::standard,
                 false,
                 "checkpoint callback failed: checkpoint rejected"
             },
             Scenario{
                 Fault::none,
                 Callback::unknown,
                 false,
                 "checkpoint callback failed: unknown error"
             },
             Scenario{
                 Fault::none,
                 Callback::standard,
                 true,
                 "checkpoint callback failed: checkpoint rejected"
             },
             Scenario{
                 Fault::none,
                 Callback::unknown,
                 true,
                 "checkpoint callback failed: unknown error"
             },
             Scenario{
                 Fault::none, Callback::streaming, true, "checkpoint rejected"
             },
             Scenario{
                 Fault::close,
                 Callback::none,
                 false,
                 "cannot close download file"
             },
             Scenario{
                 Fault::close,
                 Callback::none,
                 true,
                 "cannot close download file"
             },
             Scenario{
                 Fault::truncate,
                 Callback::streaming,
                 true,
                 "cannot roll back partial download file"
             },
             Scenario{
                 Fault::none,
                 Callback::rollback_failure,
                 true,
                 "cannot roll back partial download file"
             },
         }) {
        if (scenario.resumed) {
            onedrive::test::write_file(destination, "prefix");
        }
        auto listener = onedrive::test::create_loopback_listener();
        if (!listener.socket) {
            return fail("cannot create rollback test listener");
        }
        std::string server_error;
        std::string request_headers;
        fault = scenario.fault;
        injected = 0;
        std::jthread server{[&] {
            if (!onedrive::test::wait_for_connection(listener.socket.get())) {
                server_error = "timed out waiting for rollback request";
                return;
            }
            onedrive::test::Socket connection{
                ::accept4(listener.socket.get(), nullptr, nullptr, SOCK_CLOEXEC)
            };
            if (!connection || !onedrive::test::receive_http_headers(
                                   connection.get(), request_headers
                               )) {
                server_error = "cannot read rollback request";
                return;
            }
            const std::string response =
                scenario.resumed
                    ? "HTTP/1.1 206 Partial Content\r\n"
                      "Content-Range: bytes 6-9/10\r\n"
                      "Content-Length: 4\r\nConnection: close\r\n\r\ntail"
                    : "HTTP/1.1 200 OK\r\n"
                      "Content-Length: 4\r\nConnection: close\r\n\r\ntail";
            if (!onedrive::test::send_all(
                    connection.get(), std::as_bytes(std::span{response})
                )) {
                server_error = "cannot send rollback response";
            }
        }};
        std::vector<std::uint64_t> checkpoints;
        const auto result = client.download(
            {
                .url = "http://127.0.0.1:" + std::to_string(listener.port) +
                       "/rollback",
                .headers = scenario.resumed
                               ? std::vector<std::string>{"Range: bytes=6-9"}
                               : std::vector<std::string>{},
                .connect_timeout = std::chrono::seconds{2},
                .operation_timeout = std::chrono::seconds{5},
                .download_offset = scenario.resumed ? 6U : 0U,
                .download_checkpoint_interval_bytes =
                    scenario.callback == Callback::streaming ? 1U : 0U,
            },
            destination,
            {},
            {},
            [&](std::uint64_t completed) {
                checkpoints.push_back(completed);
                if (scenario.callback == Callback::unknown) {
                    throw 42;
                }
                if (scenario.callback == Callback::rollback_failure) {
                    std::filesystem::rename(
                        destination, temporary.path() / "saved"
                    );
                    std::filesystem::create_directory(destination);
                }
                throw std::runtime_error{"checkpoint rejected"};
            }
        );
        server.join();
        fault = Fault::none;
        if (!server_error.empty()) {
            return fail(server_error);
        }
        if (result || !result.error().message.contains(scenario.message) ||
            result.error().code != onedrive::http::HttpErrorCode::transport) {
            return fail(
                "download failure did not report its expected transport error"
            );
        }
        if (scenario.fault != Fault::none && injected != 1) {
            return fail(
                "download syscall failure was not exercised exactly once"
            );
        }
        const auto expected_checkpoints =
            scenario.callback == Callback::none
                ? std::vector<std::uint64_t>{}
                : std::vector<std::uint64_t>{scenario.resumed ? 10U : 4U};
        if (checkpoints != expected_checkpoints ||
            (scenario.resumed &&
             !request_headers.contains("Range: bytes=6-9"))) {
            return fail(
                "download failure checkpoint offsets or resume headers were "
                "incorrect"
            );
        }
        if (!scenario.resumed) {
            if (std::filesystem::exists(destination)) {
                return fail("failed new download retained uncheckpointed data");
            }
        } else if (scenario.callback == Callback::rollback_failure) {
            if (!std::filesystem::is_directory(destination) ||
                onedrive::test::read_file(temporary.path() / "saved") !=
                    "prefixtail") {
                return fail(
                    "rollback failure did not preserve the substituted path"
                );
            }
            std::filesystem::remove(temporary.path() / "saved");
        } else if (onedrive::test::read_file(destination) !=
                   (scenario.fault == Fault::truncate ? "prefixtail"
                                                      : "prefix")) {
            return fail(
                "failed resumed download did not respect its durable prefix"
            );
        }
        std::filesystem::remove_all(destination);
    }
    return EXIT_SUCCESS;
}
