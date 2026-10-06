#include "onedrive/http/http_client.hpp"

#include "http/proxy.hpp"
#include "http/request.hpp"
#include "onedrive/util/path_security.hpp"
#include "onedrive/util/unique_file_descriptor.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace onedrive::http {

using detail::DownloadState;
using detail::join_proxy_bypass_list;
using detail::perform_request;
using detail::read_proxy_password;
using FileDescriptor = onedrive::util::UniqueFD;

CurlHttpClient::CurlHttpClient(ProxyOptions proxy)
    : proxy_{std::move(proxy)} {
    if (proxy_.password_file && !proxy_.username) {
        throw std::invalid_argument(
            "proxy password file requires a proxy username"
        );
    }
    if (proxy_.password_file) {
        proxy_password_ = read_proxy_password(*proxy_.password_file);
    }
    if (proxy_.no_proxy) {
        no_proxy_ = join_proxy_bypass_list(*proxy_.no_proxy);
    }
}

HttpResult CurlHttpClient::perform(const HttpRequest& request) const {
    return perform_request(
        request,
        proxy_,
        proxy_password_,
        no_proxy_,
        -1
    );
}

HttpResult CurlHttpClient::download(
    const HttpRequest& request,
    const std::filesystem::path& destination,
    const DownloadProgress& progress,
    const DownloadData& data,
    const DownloadCheckpoint& checkpoint,
    const DownloadResponseGate& response_gate
) const {
    if (request.download_offset >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected(HttpError{
            .message = "download offset exceeds the supported file size",
        });
    }
    const int flags = request.download_offset == 0 ?
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW :
                          O_WRONLY | O_CLOEXEC | O_NOFOLLOW;
    const mode_t mode = request.private_download_permissions ?
                            S_IRUSR | S_IWUSR :
                            S_IRUSR | S_IWUSR |
                                S_IRGRP | S_IWGRP |
                                S_IROTH | S_IWOTH;
    FileDescriptor descriptor;
    try {
        descriptor = onedrive::util::open_path_no_symlinks(
            destination,
            flags,
            request.download_offset == 0 ? mode : 0
        );
    } catch (const std::runtime_error& error) {
        return std::unexpected(HttpError{.message = error.what()});
    }

    DownloadState download_state;
    auto response = perform_request(
        request,
        proxy_,
        proxy_password_,
        no_proxy_,
        descriptor.get(),
        progress,
        data,
        checkpoint,
        response_gate,
        &download_state
    );
    std::string close_error;
    std::string truncate_error;
    const bool accepted = download_state.response_accepted;
    if ((!response || response->status_code < 200 ||
         response->status_code >= 300 || !accepted) &&
        download_state.durable_offset != 0 &&
        ::ftruncate(
            descriptor.get(),
            static_cast<off_t>(download_state.durable_offset)
        ) == -1) {
        truncate_error = std::strerror(errno);
    }
    if (const auto error = descriptor.close(); error) {
        close_error = error.message();
    }
    bool checkpoint_failed = false;
    if (response && response->status_code >= 200 &&
        response->status_code < 300 && accepted && close_error.empty() &&
        checkpoint &&
        download_state.file_offset > download_state.durable_offset) {
        try {
            checkpoint(download_state.file_offset);
            download_state.durable_offset = download_state.file_offset;
        } catch (const std::exception& error) {
            checkpoint_failed = true;
            response = std::unexpected(HttpError{
                .message = "download checkpoint callback failed: " +
                           std::string{error.what()},
            });
        } catch (...) {
            checkpoint_failed = true;
            response = std::unexpected(HttpError{
                .message =
                    "download checkpoint callback failed: unknown error",
            });
        }
    }
    if ((checkpoint_failed || !close_error.empty()) &&
        download_state.durable_offset != 0) {
        std::error_code error;
        std::filesystem::resize_file(
            destination,
            download_state.durable_offset,
            error
        );
        if (error) {
            truncate_error = error.message();
        }
    }
    if ((!response || response->status_code < 200 ||
         response->status_code >= 300 || !accepted ||
         !close_error.empty()) &&
        download_state.durable_offset == 0) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
    }
    if (!truncate_error.empty()) {
        return std::unexpected(HttpError{
            .message = "cannot roll back partial download file '" +
                       destination.string() + "': " + truncate_error,
        });
    }
    if (!close_error.empty()) {
        return std::unexpected(HttpError{
            .message = "cannot close download file '" + destination.string() +
                       "': " + close_error,
        });
    }
    return response;
}

}  // namespace onedrive::http
