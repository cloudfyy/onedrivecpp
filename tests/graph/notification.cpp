#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

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
    return EXIT_SUCCESS;
}

int test_unauthorized_channel_invalidates_token() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"first-access",)"
                        R"("refresh_token":"existing-refresh"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 401,
                .body = R"({"error":{"code":"InvalidAuthenticationToken"}})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"second-access",)"
                        R"("refresh_token":"existing-refresh"})",
            },
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
    client.refresh_access_token();
    if (transport_pointer->queued.requests.size() != 3) {
        return fail("invalidated notification token was not refreshed");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_notification_channel();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_unauthorized_channel_invalidates_token();
}
