#include "support/progress.hpp"
#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

std::unique_ptr<FakeTransport>
channel_transport(onedrive::http::HttpResponse response) {
    return std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        json_response(
            200,
            R"({"expires_in":3600,"access_token":"access","refresh_token":"refresh"})"
        ),
        std::move(response),
    });
}

int test_documented_response_without_expiration() {
    auto transport = channel_transport(json_response(
        200,
        R"({"id":"opaque-id","notificationUrl":"https://notify.example.test/tenant/callback?token=secret"})"
    ));
    auto* requests = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
        auth_options(),
    };
    const auto channel = client.notification_channel();
    if (channel.expires_at ||
        channel.notification_url !=
            "https://notify.example.test/tenant/callback?token=secret" ||
        requests->queued.requests.size() != 2 ||
        !has_header(
            requests->queued.requests.back(), "Content-Type: application/json"
        )) {
        return fail(
            "documented Socket.IO response required an invented expiration"
        );
    }
    return EXIT_SUCCESS;
}

int test_notification_failure_classification() {
    using Kind = onedrive::graph::NotificationFailureKind;
    for (const auto& [status, expected] :
         {std::pair{401L, Kind::unauthorized},
          std::pair{403L, Kind::unavailable},
          std::pair{404L, Kind::unavailable},
          std::pair{405L, Kind::unavailable},
          std::pair{410L, Kind::unavailable},
          std::pair{501L, Kind::unavailable},
          std::pair{429L, Kind::transient},
          std::pair{503L, Kind::transient}}) {
        auto transport = channel_transport({
            .status_code = status,
            .body = "non-json-response-secret",
        });
        auto* requests = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
        };
        try {
            static_cast<void>(client.notification_channel());
            return fail("failed notification response was accepted");
        } catch (const onedrive::graph::NotificationChannelError& error) {
            if (error.kind() != expected ||
                std::string_view{error.what()}.contains("response-secret") ||
                requests->queued.requests.size() != 2) {
                return fail(
                    "notification status lost its classification or leaked its "
                    "response"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_invalid_notification_response() {
    for (const auto* body : {
             R"({"notificationUrl":"response-secret",broken)",
             "[]",
             "null",
             "{}",
             R"({"notificationUrl":12})",
             R"({"notificationUrl":""})",
             R"({"notificationUrl":"response-secret","expirationDateTime":null})",
             R"({"notificationUrl":"response-secret","expirationDateTime":42})",
             R"({"notificationUrl":"response-secret","expirationDateTime":"expiration-secret"})"
         }) {
        auto transport = channel_transport(json_response(200, body));
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
        };
        try {
            static_cast<void>(client.notification_channel());
            return fail("invalid notification response was accepted");
        } catch (const onedrive::graph::NotificationChannelError& error) {
            const std::string_view message{error.what()};
            if (error.kind() !=
                    onedrive::graph::NotificationFailureKind::transient ||
                message.contains("response-secret") ||
                message.contains("expiration-secret")) {
                return fail(
                    "invalid notification data lost its typed error or "
                    "disclosed secrets"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_notification_channel() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"existing-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"id":"subscription-id",)"
                R"("notificationUrl":"https://notify.example.test/notifications?token=secret",)"
                R"("expirationDateTime":"2026-10-06T09:00:00Z"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"notificationUrl":"https://notify.example.test/tenant/callback?token=renewed",)"
                R"("expirationDateTime":"2026-10-06T10:00:00Z"})",
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
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    const auto channel = client.notification_channel();
    if (channel.notification_url !=
            "https://notify.example.test/notifications?token=secret" ||
        channel.expires_at !=
            std::chrono::sys_days{
                std::chrono::year{2026} / std::chrono::October / 6
            } + std::chrono::hours{9}) {
        return fail("Graph notification channel was not parsed");
    }
    if (transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/root/"
            "subscriptions/socketIo" ||
        !has_header(
            transport_pointer->queued.requests[1], "Authorization: ******"
        )) {
        return fail("Graph notification channel request was incorrect");
    }
    const auto renewed = client.notification_channel();
    if (renewed.notification_url !=
            "https://notify.example.test/tenant/callback?token=renewed" ||
        renewed.expires_at != *channel.expires_at + std::chrono::hours{1}) {
        return fail(
            "notification renewal did not replace its endpoint and expiration"
        );
    }
    if (transport_pointer->queued.requests.size() != 3 ||
        transport_pointer->queued.requests[2].url !=
            transport_pointer->queued.requests[1].url ||
        transport_pointer->queued.requests[2].method !=
            onedrive::http::HttpMethod::get) {
        return fail(
            "notification renewal changed the Drive or unnecessarily refreshed "
            "authentication"
        );
    }
    return EXIT_SUCCESS;
}

int test_unauthorized_channel_invalidates_token() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"first-access",)"
                    R"("refresh_token":"existing-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 401,
            .body = "non-json-authentication-response",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"second-access",)"
                    R"("refresh_token":"existing-refresh"})",
        },
        json_response(
            200, R"({"notificationUrl":"https://notify.example.test/callback"})"
        ),
        json_response(
            200,
            R"({"expires_in":3600,"access_token":"third-access","refresh_token":"existing-refresh"})"
        ),
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
        static_cast<void>(client.notification_channel());
        return fail("unauthorized notification channel was accepted");
    } catch (const onedrive::graph::NotificationChannelError& error) {
        if (!error.unauthorized()) {
            return fail("unauthorized channel was classified as transient");
        }
    }
    static_cast<void>(client.notification_channel());
    if (transport_pointer->queued.requests.size() != 4 ||
        transport_pointer->queued.requests[2].method !=
            onedrive::http::HttpMethod::post) {
        return fail("invalidated notification token was not refreshed");
    }
    client.refresh_access_token();
    if (transport_pointer->queued.requests.size() != 5) {
        return fail(
            "explicit notification token refresh reused its cached access token"
        );
    }
    return EXIT_SUCCESS;
}

int test_transport_failure_is_transient() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret",)"
                        R"("refresh_token":"existing-refresh"})",
            },
            std::unexpected(
                onedrive::http::HttpError{
                    .message = "simulated notification transport failure",
                }
            ),
        });
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
    };

    try {
        static_cast<void>(client.notification_channel());
        return fail("failed notification transport was accepted");
    } catch (const onedrive::graph::NotificationChannelError& error) {
        if (error.unauthorized()) {
            return fail("transport failure was classified as unauthorized");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{6};
    if (progress.run(
            "documented_response_without_expiration",
            test_documented_response_without_expiration
        ) != EXIT_SUCCESS ||
        progress.run(
            "notification_failure_classification",
            test_notification_failure_classification
        ) != EXIT_SUCCESS ||
        progress.run(
            "invalid_notification_response", test_invalid_notification_response
        ) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result =
            progress.run("notification_channel", test_notification_channel);
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "unauthorized_channel_invalidates_token",
            test_unauthorized_channel_invalidates_token
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run(
        "transport_failure_is_transient", test_transport_failure_is_transient
    ));
}
