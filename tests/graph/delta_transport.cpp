#include "support.hpp"
#include "graph/delta_changes.hpp"
#include "graph/delta_hierarchy.hpp"
#include "graph/support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/storage/item_database.hpp"
#include "sync/support.hpp"

#include "delta_cases.hpp"

namespace onedrive::test::graph::delta_cases {

using namespace onedrive::test::graph;

int test_shared_delta_changes() {
    namespace graph = onedrive::graph;
    graph::RemoteItem first{
        .id = "first",
        .name = "old.txt",
        .etag = "old",
        .remote_path = "old.txt",
        .malware = true,
        .content_hash =
            onedrive::util::FileHash{
                .algorithm = onedrive::util::FileHashAlgorithm::sha256,
                .value = std::string(64, '0'),
            },
        .validate_content = false,
        .unsupported_format = "onenote",
    };
    graph::RemoteItem second{.id = "second", .deleted = true};
    auto latest = first;
    latest.name = latest.remote_path = "new.txt";
    latest.etag = "new";
    latest.malware = false;
    latest.validate_content = true;
    latest.content_hash.reset();
    latest.unsupported_format.clear();
    graph::client_detail::DeltaChanges changes{
        std::vector<graph::RemoteItem>{first, second, latest}
    };
    if (changes.size() != 2 || changes.find("missing") ||
        !changes.find("first") || changes.find("first")->etag != "new") {
        return fail(
            "seeded delta merge did not preserve unique latest records"
        );
    }
    changes.upsert({.id = "third", .root = true});
    changes.upsert(
        {.id = "second", .name = "restored.txt", .remote_path = "restored.txt"}
    );
    const auto result = std::move(changes).release();
    if (result.size() != 3 || result[0].id != "first" ||
        result[0].name != "new.txt" || result[0].remote_path != "new.txt" ||
        result[0].malware || result[0].content_hash ||
        !result[0].validate_content || !result[0].unsupported_format.empty() ||
        result[1].id != "second" || result[1].deleted ||
        result[1].name != "restored.txt" || result[2].id != "third" ||
        !result[2].root) {
        return fail(
            "shared delta merge retained old flags or changed first-seen order"
        );
    }
    return EXIT_SUCCESS;
}

int test_delta_transport_and_shape_failures() {
    namespace od = onedrive;
    const auto rejected = [](od::http::HttpResult response,
                             std::string_view message,
                             bool cancelled = false) {
        auto transport =
            std::make_unique<FakeTransport>(std::deque<od::http::HttpResult>{
                token_response(), std::move(response)
            });
        auto* recorded = transport.get();
        od::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        try {
            static_cast<void>(client.list_delta(std::nullopt));
        } catch (const od::graph::RequestCancelledError&) {
            return cancelled && recorded->queued.requests.size() == 2;
        } catch (const std::runtime_error& error) {
            return !cancelled &&
                   std::string_view{error.what()}.contains(message) &&
                   recorded->queued.requests.size() == 2;
        }
        return false;
    };
    if (!rejected(
            std::unexpected{od::http::HttpError{
                .code = od::http::HttpErrorCode::transport,
                .message = "offline transport failed",
            }},
            "offline transport failed"
        ) ||
        !rejected(
            std::unexpected{od::http::HttpError{
                .code = od::http::HttpErrorCode::cancelled,
                .message = "cancelled transport",
            }},
            {},
            true
        )) {
        return fail(
            "delta transport error/cancellation was hidden or misclassified"
        );
    }
    const std::vector<std::pair<std::string, std::string>> cases{
        {R"({"value":[{"id":"","name":"Drive","root":{},"folder":{}}]})",
         "empty ID"},
        {R"({"value":[{"id":"root","name":"Drive","eTag":42,"root":{},"folder":{}}]})",
         "invalid eTag"},
        {R"({"value":[{"id":"root","name":"","root":{},"folder":{}}]})",
         "invalid metadata"},
        {R"({"value":[{"id":"root","name":"Drive","size":-1,"root":{},"folder":{}}]})",
         "invalid metadata"},
        {R"({"value":[{"name":"missing ID"}]})",
         "missing required drive item data"},
        {R"({"value":[]})", "final delta link"},
        {R"({"value":[],"@odata.deltaLink":42})", "final delta link"},
    };
    for (const auto& [body, message] : cases) {
        if (!rejected(json_response(200, body), message)) {
            return fail(
                "invalid delta metadata/page shape was accepted: " + message
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_delta_cancelled_before_request_or_between_pages() {
    namespace od = onedrive;
    for (const bool before_request : {false, true}) {
        auto transport =
            std::make_unique<FakeTransport>(std::deque<od::http::HttpResult>{
                token_response(),
                graph_page(
                    "[]", "https://graph.example.test/v1.0/delta?page=2"
                ),
            });
        auto* recorded = transport.get();
        od::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        std::stop_source stop;
        if (before_request) {
            stop.request_stop();
        }
        bool progress_seen = false;
        try {
            static_cast<void>(client.list_delta(
                std::nullopt,
                [&](auto, auto, auto state) {
                    progress_seen = state == od::util::ProgressState::ongoing;
                    stop.request_stop();
                },
                stop.get_token()
            ));
            return fail("cancelled delta returned a partial successful result");
        } catch (const od::graph::RequestCancelledError&) {
            if (recorded->queued.requests.size() !=
                    (before_request ? 0U : 2U) ||
                progress_seen == before_request) {
                return fail(
                    "delta cancellation issued an unexpected request/page"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_untrusted_pagination_url() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page("[]", "https://attacker.example/collect"),
        });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
    };

    try {
        static_cast<void>(client.list_root());
        return fail("untrusted Graph pagination URL was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("outside its endpoint")) {
            return fail("untrusted pagination URL error was not reported");
        }
    }
    if (transport_pointer->queued.requests.size() != 2) {
        return fail(
            "authorization token was sent to an untrusted pagination URL"
        );
    }
    return EXIT_SUCCESS;
}

int test_invalid_root_page_shapes() {
    const auto rejected = [](std::string body,
                             std::string_view expected_message) {
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"expires_in":3600,"access_token":"access-secret"})",
                },
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body = std::move(body),
                },
            }
        );
        auto* transport_pointer = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
        };
        try {
            static_cast<void>(client.list_root());
        } catch (const std::runtime_error& error) {
            return std::string_view{error.what()}.contains(expected_message) &&
                   transport_pointer->queued.requests.size() == 2;
        }
        return false;
    };

    if (!rejected(
            R"json({"value":[],"@odata.nextLink":"https://graph.microsoft.com/v1.0/me/drive/root/children"})json",
            "repeated pagination URL"
        ) ||
        !rejected(
            R"json({"value":[],"@odata.nextLink":42})json",
            "invalid pagination URL"
        ) ||
        !rejected(R"json({})json", "missing field 'value'") ||
        !rejected(R"json({"value":{}})json", "field 'value' is not an array")) {
        return fail("invalid Graph root page shape was accepted");
    }
    return EXIT_SUCCESS;
}

int test_invalid_delta_cursor_error() {
    constexpr std::string_view saved_delta_link{
        "https://graph.example.test/v1.0/delta?token=expired"
    };
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 410,
                .body = R"({"error":{"code":"resyncRequired",)"
                        R"("message":"The delta token is no longer valid."}})",
            },
        });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    try {
        static_cast<void>(client.list_delta(std::string{saved_delta_link}));
        return fail("invalid saved Graph delta cursor was accepted");
    } catch (const onedrive::graph::DeltaCursorInvalidError& error) {
        if (!std::string{error.what()}.contains(
                "delta token is no longer valid"
            )) {
            return fail(
                "invalid Graph delta cursor error omitted the server detail"
            );
        }
    }
    if (transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[1].url != saved_delta_link) {
        return fail("invalid Graph delta cursor request was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_delta_request_cancellation() {
    const auto run_cancelled_request = [](std::size_t cancel_on_call) {
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                graph_page(
                    "[]", std::nullopt, "https://graph.example.test/v1.0/delta"
                ),
            }
        );
        auto* transport_pointer = transport.get();
        std::stop_source stop;
        std::size_t calls = 0;
        transport_pointer->on_perform =
            [&](const onedrive::http::HttpRequest&) {
                if (++calls == cancel_on_call) {
                    stop.request_stop();
                }
            };
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "drive id",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };

        try {
            static_cast<void>(
                client.list_delta(std::nullopt, {}, stop.get_token())
            );
            return false;
        } catch (const onedrive::graph::RequestCancelledError&) {
            return transport_pointer->queued.requests.size() ==
                       cancel_on_call &&
                   transport_pointer->queued.requests.back()
                       .stop_token.stop_possible();
        }
    };

    if (!run_cancelled_request(1) || !run_cancelled_request(2)) {
        return fail(
            "Graph delta did not propagate cancellation through token refresh "
            "and page requests"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::graph::delta_cases
