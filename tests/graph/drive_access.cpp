#include "support.hpp"

#include <ranges>
#include <stop_token>
#include "drive_cases.hpp"

namespace onedrive::test::graph::drive_cases {

using namespace onedrive::test::graph;

int test_list_root_with_refresh_and_pagination() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"rotated-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"value":[{"id":"folder-id","name":"Documents",)"
                    R"("eTag":"folder-etag","folder":{"childCount":2}}],)"
                    R"("@odata.nextLink":)"
                    R"("https://graph.example.test/v1.0/next?page=2"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","malware":{},"file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA="}}}]})json",
        },
    });
    auto* transport_pointer = transport.get();
    auto token_store =
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"});
    auto* token_store_pointer = token_store.get();

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::move(token_store)),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0/",
        },
    };
    const auto items = client.list_root();

    if (items.size() != 2 || !items[0].directory || items[1].directory ||
        items[0].name != "Documents" || items[1].etag != "file-etag" ||
        !items[1].malware || !items[1].content_hash ||
        items[1].content_hash->algorithm !=
            onedrive::util::FileHashAlgorithm::quick_xor ||
        items[1].content_hash->value != "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
        return fail("Graph drive items were not parsed across pages");
    }

    if (token_store_pointer->saved_tokens !=
        std::vector<std::string>{"rotated-refresh"}) {
        return fail("rotated refresh token was not persisted");
    }
    if (transport_pointer->queued.requests.size() != 3 ||
        transport_pointer->queued.requests[0].method !=
            onedrive::http::HttpMethod::post ||
        !transport_pointer->queued.requests[0].body.contains(
            "refresh_token=existing-refresh"
        ) ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/root/children" ||
        transport_pointer->queued.requests[2].url !=
            "https://graph.example.test/v1.0/next?page=2" ||
        !has_header(
            transport_pointer->queued.requests[1],
            "Authorization: Bearer access-secret"
        )) {
        return fail("Graph authentication or pagination request was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_access_token_is_reused_across_graph_operations() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response("cached-access", "rotated-refresh"),
            graph_page("[]"),
            graph_page("[]"),
            graph_page(
                "[]",
                std::nullopt,
                "https://graph.example.test/v1.0/delta?token=next"
            ),
        });
    auto* transport_pointer = transport.get();
    auto token_store =
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"});
    auto* token_store_pointer = token_store.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::move(token_store)),
        auth_options(),
        {
            .drive_id = "drive/id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    static_cast<void>(client.list_root());
    static_cast<void>(client.list_root());
    static_cast<void>(client.list_delta(std::nullopt));

    const auto& requests = transport_pointer->queued.requests;
    if (requests.size() != 4 ||
        requests.front().method != onedrive::http::HttpMethod::post ||
        !requests.front().body.contains("refresh_token=existing-refresh") ||
        token_store_pointer->saved_tokens !=
            std::vector<std::string>{"rotated-refresh"}) {
        return fail("Graph operations refreshed a still-valid access token");
    }
    for (std::size_t index = 1; index < requests.size(); ++index) {
        if (requests[index].method != onedrive::http::HttpMethod::get ||
            !has_header(requests[index], "Authorization: ******")) {
            return fail(
                "Graph operations did not reuse the cached access token"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_access_token_is_refreshed_before_expiry_margin() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":30,"access_token":"short-lived",)"
                        R"("refresh_token":"rotated-once"})",
            },
            graph_page("[]"),
            token_response("long-lived", "rotated-twice"),
            graph_page("[]"),
            graph_page("[]"),
        });
    auto* transport_pointer = transport.get();
    auto token_store =
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"});
    auto* token_store_pointer = token_store.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::move(token_store)),
        auth_options(),
        {
            .drive_id = "drive/id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    static_cast<void>(client.list_root());
    static_cast<void>(client.list_root());
    static_cast<void>(client.list_root());

    const auto& requests = transport_pointer->queued.requests;
    if (requests.size() != 5 ||
        requests[0].method != onedrive::http::HttpMethod::post ||
        requests[1].method != onedrive::http::HttpMethod::get ||
        requests[2].method != onedrive::http::HttpMethod::post ||
        requests[3].method != onedrive::http::HttpMethod::get ||
        requests[4].method != onedrive::http::HttpMethod::get ||
        token_store_pointer->saved_tokens !=
            std::vector<std::string>{"rotated-once", "rotated-twice"}) {
        return fail(
            "Graph did not refresh a token inside the expiry safety margin"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::graph::drive_cases
