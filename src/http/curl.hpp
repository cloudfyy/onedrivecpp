#pragma once

#include "onedrive/http/http_options.hpp"

#include <curl/curl.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace onedrive::http::detail {

class CurlRuntime final {
public:
    CurlRuntime()
        : result_{::curl_global_init(CURL_GLOBAL_DEFAULT)} {
    }

    ~CurlRuntime() {
        if (result_ == CURLE_OK) {
            ::curl_global_cleanup();
        }
    }

    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
    CurlRuntime(CurlRuntime&&) = delete;
    CurlRuntime& operator=(CurlRuntime&&) = delete;

    [[nodiscard]] CURLcode result() const noexcept {
        return result_;
    }

private:
    CURLcode result_;
};

[[nodiscard]] inline CURLcode initialize_curl() {
    static const CurlRuntime runtime;
    return runtime.result();
}

inline void throw_if_curl_error(CURLcode result, std::string_view operation) {
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string{operation} + ": " + ::curl_easy_strerror(result)
        );
    }
}

[[nodiscard]] inline long curl_proxy_auth(ProxyAuth auth) {
    switch (auth) {
    case ProxyAuth::automatic:
        return static_cast<long>(CURLAUTH_ANY);
    case ProxyAuth::basic:
        return static_cast<long>(CURLAUTH_BASIC);
    case ProxyAuth::digest:
        return static_cast<long>(CURLAUTH_DIGEST);
    case ProxyAuth::ntlm:
        return static_cast<long>(CURLAUTH_NTLM);
    case ProxyAuth::negotiate:
        return static_cast<long>(CURLAUTH_NEGOTIATE);
    }
    throw std::invalid_argument("unsupported proxy authentication mode");
}

struct CurlHandleDeleter {
    void operator()(CURL* handle) const noexcept {
        ::curl_easy_cleanup(handle);
    }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

class ThreadCurlHandlePool final {
public:
    [[nodiscard]] CurlHandle acquire() {
        auto handle = std::move(available_);
        if (!handle) {
            handle.reset(::curl_easy_init());
        }
        if (handle) {
            ::curl_easy_reset(handle.get());
        }
        return handle;
    }

    void release(CurlHandle handle) noexcept {
        if (!available_) {
            available_ = std::move(handle);
        }
    }

private:
    CurlHandle available_;
};

[[nodiscard]] inline ThreadCurlHandlePool& thread_curl_handle_pool() {
    thread_local ThreadCurlHandlePool pool;
    return pool;
}

class CurlHandleLease final {
public:
    CurlHandleLease()
        : pool_{thread_curl_handle_pool()},
          handle_{pool_.acquire()} {
    }

    ~CurlHandleLease() {
        pool_.release(std::move(handle_));
    }

    CurlHandleLease(const CurlHandleLease&) = delete;
    CurlHandleLease& operator=(const CurlHandleLease&) = delete;
    CurlHandleLease(CurlHandleLease&&) = delete;
    CurlHandleLease& operator=(CurlHandleLease&&) = delete;

    [[nodiscard]] CURL* get() const noexcept {
        return handle_.get();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle_);
    }

private:
    ThreadCurlHandlePool& pool_;
    CurlHandle handle_;
};

} // namespace onedrive::http::detail
