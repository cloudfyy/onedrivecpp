#include "support.hpp"

#include <ranges>
#include <stop_token>
#include "drive_cases.hpp"

namespace onedrive::test::graph::drive_cases {

using namespace onedrive::test::graph;

namespace {

int expect_invalid_drive_info(
    std::string body, std::string_view failure_message
) {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = std::move(body),
            },
        });
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
        static_cast<void>(client.drive_info());
        return fail(failure_message);
    } catch (const std::runtime_error&) {
        return EXIT_SUCCESS;
    }
}

} // namespace

int test_drive_information_and_quota() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"drive-1","name":"OneDrive","driveType":"personal","webUrl":"https://example.test/one","owner":{"user":{"displayName":"Alice"}},"quota":{"total":1000,"used":400,"remaining":600,"deleted":25,"state":"normal"}}],"@odata.nextLink":"https://graph.example.test/v1.0/drives-next"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"drive-2","name":"Team","driveType":"business","owner":{"group":{"displayName":"Team Owner"}}}]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"drive-1","name":"OneDrive","driveType":"personal","webUrl":"https://example.test/one","owner":{"user":{"displayName":"Alice"}},"quota":{"total":1000,"used":400,"remaining":600,"deleted":25,"state":"normal"}})json",
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
    const auto drives = client.list_drives();
    const auto current = client.drive_info();
    const auto& requests = transport_pointer->queued.requests;
    if (drives.size() != 2 || drives[0].owner != "Alice" || !drives[0].quota ||
        drives[0].quota->remaining != 600 || drives[1].owner != "Team Owner" ||
        drives[1].quota || current.id != "drive-1" || !current.quota ||
        current.quota->deleted != 25 || requests.size() != 4 ||
        requests[1].url !=
            "https://graph.example.test/v1.0/me/drives?$select=id,name,"
            "driveType,webUrl,owner,quota" ||
        requests[2].url != "https://graph.example.test/v1.0/drives-next" ||
        requests[3].url !=
            "https://graph.example.test/v1.0/me/drive?$select=id,name,"
            "driveType,webUrl,owner,quota") {
        return fail("Graph drive information or quota was not parsed");
    }
    return EXIT_SUCCESS;
}

int test_rejects_invalid_drive_metadata() {
    {
        auto transport = std::make_unique<
            FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"json({"value":[],"@odata.nextLink":"https://attacker.example/drives"})json",
            },
        });
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };
        try {
            static_cast<void>(client.list_drives());
            return fail("an external drives pagination URL was accepted");
        } catch (const std::runtime_error&) {
        }
    }
    {
        auto transport = std::make_unique<
            FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body =
                    R"json({"id":"drive-1","name":"OneDrive","quota":{"total":1000,"used":400,"remaining":-1,"deleted":25}})json",
            },
        });
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };
        try {
            static_cast<void>(client.drive_info());
            return fail("a negative drive quota was accepted");
        } catch (const std::runtime_error&) {
        }
    }
    if (const int result = expect_invalid_drive_info(
            R"json({"id":"drive-1","name":"OneDrive","quota":[]})json",
            "a non-object drive quota was accepted"
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = expect_invalid_drive_info(
            R"json({"id":"drive-1","name":"OneDrive","quota":{"total":1000}})json",
            "an incomplete drive quota was accepted"
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = expect_invalid_drive_info(
            R"json({"id":"","name":"OneDrive"})json",
            "incomplete drive metadata was accepted"
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}

int test_drive_identity_and_profile_photo() {
    FakeTransport transport{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"user-id","displayName":"Alice Example"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"id":"canonical-drive-id","name":"Alice Drive","driveType":"personal"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .headers =
                {
                    {.name = "Content-Type", .value = "image/png"},
                },
            .body = "photo-bytes",
        },
    }};
    onedrive::http::HttpTransport transport_proxy{
        onedrive::util::borrowed_proxy, transport
    };
    const auto identity = onedrive::graph::fetch_drive_identity(
        transport_proxy,
        "access-token",
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        }
    );
    if (identity.user_id != "user-id" ||
        identity.user_display_name != "Alice Example" ||
        identity.configured_drive_id != "me" ||
        identity.drive_id != "canonical-drive-id" ||
        identity.drive_name != "Alice Drive" ||
        identity.drive_type != "personal" || !identity.photo ||
        identity.photo->content_type != "image/png" ||
        std::string{
            identity.photo->bytes.begin(), identity.photo->bytes.end()
        } != "photo-bytes" ||
        transport.queued.requests.size() != 3 ||
        transport.queued.requests[0].url !=
            "https://graph.example.test/v1.0/me?$select=id,displayName" ||
        transport.queued.requests[1].url !=
            "https://graph.example.test/v1.0/me/"
            "drive?$select=id,name,driveType" ||
        transport.queued.requests[2].url !=
            "https://graph.example.test/v1.0/me/photo/$value" ||
        !has_header(transport.queued.requests[2], "Authorization: ******")) {
        return fail("Graph account and drive identity were not loaded");
    }
    FakeTransport without_photo{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"user-id","displayName":"Alice Example"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"canonical-drive-id","name":"Alice Drive"})",
        },
        onedrive::http::HttpResponse{.status_code = 404},
    }};
    onedrive::http::HttpTransport without_photo_proxy{
        onedrive::util::borrowed_proxy, without_photo
    };
    if (onedrive::graph::fetch_drive_identity(
            without_photo_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        )
            .photo) {
        return fail("missing Graph profile photo was not treated as optional");
    }
    FakeTransport invalid_identity{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = "{",
        },
    }};
    onedrive::http::HttpTransport invalid_identity_proxy{
        onedrive::util::borrowed_proxy, invalid_identity
    };
    try {
        static_cast<void>(onedrive::graph::fetch_drive_identity(
            invalid_identity_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        ));
        return fail("invalid Graph identity JSON was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("invalid user identity JSON")) {
            return fail("invalid Graph JSON lost its request context");
        }
    }
    FakeTransport denied_identity{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 403,
            .body =
                R"json({"error":{"code":"accessDenied","message":"denied"}})json",
        },
    }};
    onedrive::http::HttpTransport denied_identity_proxy{
        onedrive::util::borrowed_proxy, denied_identity
    };
    try {
        static_cast<void>(onedrive::graph::fetch_drive_identity(
            denied_identity_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        ));
        return fail("denied Graph identity query was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "Microsoft Graph user identity query failed: denied"
            )) {
            return fail("Graph identity error lost its request context");
        }
    }
    return EXIT_SUCCESS;
}

int test_drive_identity_types() {
    using Json = nlohmann::json;
    for (const std::string type :
         {"personal", "business", "documentLibrary", "futureType", ""}) {
        Json drive{{"id", "canonical-drive"}, {"name", "Drive"}};
        if (!type.empty()) {
            drive["driveType"] = type;
        }
        FakeTransport transport{std::deque<onedrive::http::HttpResult>{
            json_response(200, R"({"id":"user","displayName":"User"})"),
            json_response(200, drive.dump()),
            json_response(404, ""),
        }};
        onedrive::http::HttpTransport proxy{
            onedrive::util::borrowed_proxy, transport
        };
        const auto identity = onedrive::graph::fetch_drive_identity(
            proxy,
            "access",
            {.drive_id = "selected drive",
             .endpoint = "https://graph.example.test/v1.0"}
        );
        if (identity.drive_type != type ||
            identity.drive_id != "canonical-drive" ||
            identity.configured_drive_id != "selected drive" ||
            transport.queued.requests[1].url !=
                "https://graph.example.test/v1.0/drives/selected%20drive"
                "?$select=id,name,driveType") {
            return fail(
                "Graph Drive type was guessed or queried on another Drive"
            );
        }
    }
    for (const auto& type : {Json(42), Json(nullptr)}) {
        FakeTransport transport{std::deque<onedrive::http::HttpResult>{
            json_response(200, R"({"id":"user","displayName":"User"})"),
            json_response(
                200,
                Json{
                    {"id", "drive"}, {"name", "Drive"}, {"driveType", type}
                }.dump()
            ),
        }};
        onedrive::http::HttpTransport proxy{
            onedrive::util::borrowed_proxy, transport
        };
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(onedrive::graph::fetch_drive_identity(
                        proxy,
                        "access",
                        {.endpoint = "https://graph.example.test/v1.0"}
                    ));
                },
                "identity response is missing required data"
            )) {
            return fail("malformed Graph Drive type appeared as unknown");
        }
    }
    return EXIT_SUCCESS;
}

int test_drive_identity_cancellation() {
    const auto cancellation_is_observed = [](std::size_t cancel_on_request) {
        std::stop_source cancellation;
        FakeTransport transport{std::deque<onedrive::http::HttpResult>{
            json_response(
                200, R"({"id":"user-id","displayName":"Alice Example"})"
            ),
            json_response(200, R"({"id":"drive-id","name":"Alice Drive"})"),
            json_response(200, "photo-bytes", {{"Content-Type", "image/png"}}),
        }};
        std::size_t perform_count = 0;
        transport.on_perform = [&](const auto&) {
            ++perform_count;
            if (perform_count == cancel_on_request) {
                cancellation.request_stop();
            }
        };
        onedrive::http::HttpTransport transport_proxy{
            onedrive::util::borrowed_proxy, transport
        };

        if (cancel_on_request == 0) {
            cancellation.request_stop();
        }
        try {
            static_cast<void>(onedrive::graph::fetch_drive_identity(
                transport_proxy,
                "access-token",
                {
                    .drive_id = "me",
                    .endpoint = "https://graph.example.test/v1.0",
                },
                cancellation.get_token()
            ));
            return false;
        } catch (const std::runtime_error& error) {
            if (!std::string{error.what()}.contains("cancelled")) {
                return false;
            }
        }

        const auto expected_requests = cancel_on_request;
        return transport.queued.requests.size() == expected_requests &&
               std::ranges::all_of(
                   transport.queued.requests, [&](const auto& request) {
                       return request.stop_token == cancellation.get_token() &&
                              request.stop_token.stop_requested();
                   }
               );
    };

    if (!cancellation_is_observed(0) || !cancellation_is_observed(1) ||
        !cancellation_is_observed(2)) {
        return fail(
            "Graph identity query did not observe cancellation at each stage"
        );
    }
    return EXIT_SUCCESS;
}

int test_profile_photo_rejects_invalid_responses() {
    const auto rejected_photo_response = [](onedrive::http::HttpResult photo,
                                            std::string_view expected_error) {
        FakeTransport transport{std::deque<onedrive::http::HttpResult>{
            json_response(
                200, R"({"id":"user-id","displayName":"Alice Example"})"
            ),
            json_response(200, R"({"id":"drive-id","name":"Alice Drive"})"),
            std::move(photo),
        }};
        onedrive::http::HttpTransport transport_proxy{
            onedrive::util::borrowed_proxy, transport
        };
        try {
            static_cast<void>(onedrive::graph::fetch_drive_identity(
                transport_proxy,
                "access-token",
                {
                    .drive_id = "me",
                    .endpoint = "https://graph.example.test/v1.0",
                }
            ));
            return false;
        } catch (const std::runtime_error& error) {
            return std::string{error.what()}.contains(expected_error) &&
                   transport.queued.requests.size() == 3 &&
                   transport.queued.requests.back().url ==
                       "https://graph.example.test/v1.0/me/photo/$value";
        }
    };

    if (!rejected_photo_response(
            json_response(200, "not a photo", {{"Content-Type", "text/plain"}}),
            "invalid content type"
        ) ||
        !rejected_photo_response(
            json_response(500, R"({"error":"internalError"})"),
            "failed with HTTP 500"
        ) ||
        !rejected_photo_response(
            std::unexpected(
                onedrive::http::HttpError{
                    .message = "simulated profile photo transport failure",
                }
            ),
            "simulated profile photo transport failure"
        )) {
        return fail("Graph profile photo error response was mishandled");
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::graph::drive_cases
