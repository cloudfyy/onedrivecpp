#include "onedrive/auth/auth_code.hpp"
#include "support/common.hpp"

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>

namespace {

using namespace onedrive;

enum class Fault {
    none,
    url_allocate,
    url_set,
    url_get,
    first_random,
    second_random
};
Fault fault{Fault::none};
int random_calls{0};
int secret_cleanses{0};
const void* failed_random_buffer{nullptr};
bool failed_random_cleaned{false};

auth::DeviceAuthOptions options() {
    return {
        .application_id = "client",
        .auth_endpoint = "https://login.example.test",
    };
}

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

void reset(Fault next) {
    fault = next;
    random_calls = 0;
    secret_cleanses = 0;
    failed_random_buffer = nullptr;
    failed_random_cleaned = false;
}

void test_begin_failures() {
    for (const auto next : {
             Fault::url_allocate,
             Fault::url_set,
             Fault::url_get,
             Fault::first_random,
             Fault::second_random,
         }) {
        auth::AuthCodeSession session{options()};
        reset(next);
        require(
            test::throws_with<std::runtime_error>([&] {
                static_cast<void>(session.begin("http://localhost/"));
            }),
            "authorization initialization failure was swallowed"
        );
        reset(Fault::none);
        const auto result = session.check();
        require(
            session.state() == auth::AuthCodeState::failed && !result &&
                result.error().code == auth::AuthErrorCode::server,
            "authorization initialization exception was not terminal"
        );
    }
}

void test_random_cleanup() {
    for (const auto next : {Fault::first_random, Fault::second_random}) {
        auth::AuthCodeSession session{options()};
        reset(next);
        require(
            test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(session.begin("http://localhost/")); },
                "randomness"
            ) && failed_random_cleaned,
            "failed randomness did not cleanse its partial output"
        );
        if (next == Fault::second_random)
            require(
                secret_cleanses >= 2,
                "CSRF randomness failure retained the generated verifier"
            );
        reset(Fault::none);
    }
}

void test_callback_parser_failures() {
    for (const auto next :
         {Fault::url_allocate, Fault::url_set, Fault::url_get}) {
        auth::AuthCodeSession session{options()};
        reset(Fault::none);
        require(
            session.begin("http://localhost/").has_value(),
            "callback parser test could not begin"
        );
        reset(next);
        require(
            test::throws_with<std::runtime_error>([&] {
                static_cast<void>(
                    session.accept_callback("http://localhost/?code=a")
                );
            }) &&
                secret_cleanses >= 2,
            "callback parser exception retained owned verifier/CSRF bytes"
        );
        reset(Fault::none);
        require(
            session.state() == auth::AuthCodeState::failed && !session.check(),
            "callback parser exception did not preserve terminal failure"
        );
    }
}

} // namespace

extern "C" CURLU* __real_curl_url();
extern "C" CURLU* __wrap_curl_url() {
    return fault == Fault::url_allocate ? nullptr : __real_curl_url();
}

extern "C" CURLUcode
__real_curl_url_set(CURLU*, CURLUPart, const char*, unsigned int);
extern "C" CURLUcode __wrap_curl_url_set(
    CURLU* url, CURLUPart part, const char* text, unsigned int flags
) {
    return fault == Fault::url_set
               ? CURLUE_OUT_OF_MEMORY
               : __real_curl_url_set(url, part, text, flags);
}

extern "C" CURLUcode
__real_curl_url_get(CURLU*, CURLUPart, char**, unsigned int);
extern "C" CURLUcode __wrap_curl_url_get(
    CURLU* url, CURLUPart part, char** text, unsigned int flags
) {
    if (fault == Fault::url_get) {
        *text = nullptr;
        return CURLUE_OUT_OF_MEMORY;
    }
    return __real_curl_url_get(url, part, text, flags);
}

extern "C" int __real_RAND_bytes(unsigned char*, int);
extern "C" int __wrap_RAND_bytes(unsigned char* buffer, int size) {
    ++random_calls;
    if ((fault == Fault::first_random && random_calls == 1) ||
        (fault == Fault::second_random && random_calls == 2)) {
        std::fill_n(buffer, size, 0x33);
        failed_random_buffer = buffer;
        return 0;
    }
    return __real_RAND_bytes(buffer, size);
}

extern "C" void __real_OPENSSL_cleanse(void*, std::size_t);
extern "C" void __wrap_OPENSSL_cleanse(void* buffer, std::size_t size) {
    if (size == 43)
        ++secret_cleanses;
    __real_OPENSSL_cleanse(buffer, size);
    if (buffer == failed_random_buffer) {
        const auto* bytes = static_cast<const unsigned char*>(buffer);
        failed_random_cleaned = std::all_of(bytes, bytes + size, [](auto byte) {
            return byte == 0;
        });
    }
}

int main() {
    try {
        test_begin_failures();
        test_random_cleanup();
        test_callback_parser_failures();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        reset(Fault::none);
        return test::fail(error.what());
    }
}
