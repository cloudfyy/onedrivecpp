#include "onedrive/http/http_client.hpp"
#include "support/common.hpp"

#include <curl/curl.h>

#include <array>
#include <cstring>

namespace {

constexpr std::array headers{"X-First: one", "X-Second: two", "X-Third: three"};
std::size_t fail_append = 0;
std::size_t append_calls = 0;
std::size_t perform_calls = 0;
std::size_t freed_lists = 0;
std::size_t freed_nodes = 0;
bool ordered_headers = true;

} // namespace

extern "C" curl_slist* __real_curl_slist_append(curl_slist*, const char*);
extern "C" void __real_curl_slist_free_all(curl_slist*);

extern "C" curl_slist*
__wrap_curl_slist_append(curl_slist* list, const char* header) {
    if (++append_calls == fail_append) {
        return nullptr;
    }
    return __real_curl_slist_append(list, header);
}

extern "C" void __wrap_curl_slist_free_all(curl_slist* list) {
    ++freed_lists;
    for (auto* node = list; node != nullptr; node = node->next) {
        if (freed_nodes >= headers.size() ||
            std::strcmp(node->data, headers[freed_nodes]) != 0) {
            ordered_headers = false;
        }
        ++freed_nodes;
    }
    __real_curl_slist_free_all(list);
}

extern "C" CURLcode __wrap_curl_easy_perform(CURL*) {
    ++perform_calls;
    return CURLE_COULDNT_CONNECT;
}

int main() {
    const onedrive::http::CurlHttpClient client;
    for (std::size_t failure = 0; failure <= headers.size(); ++failure) {
        fail_append = failure;
        append_calls = 0;
        perform_calls = 0;
        freed_lists = 0;
        freed_nodes = 0;
        ordered_headers = true;
        const auto result = client.perform({
            .url = "https://headers.example.test/",
            .headers = {headers.begin(), headers.end()},
        });
        const auto expected_nodes = failure == 0 ? headers.size() : failure - 1;
        if (result || freed_nodes != expected_nodes ||
            freed_lists != (expected_nodes == 0 ? 0U : 1U) ||
            !ordered_headers ||
            append_calls != (failure == 0 ? headers.size() : failure) ||
            perform_calls != (failure == 0 ? 1U : 0U) ||
            (failure != 0 &&
             result.error().message != "cannot allocate HTTP headers")) {
            return onedrive::test::fail(
                "HTTP header append did not preserve and release its owned list"
            );
        }
    }
    return EXIT_SUCCESS;
}
