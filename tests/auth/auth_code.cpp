#include "onedrive/auth/auth_code.hpp"
#include "support/common.hpp"
#include "support/http.hpp"

#include <openssl/evp.h>

#include <array>

namespace {

using namespace onedrive;

auth::DeviceAuthOptions options() {
    return {
        .application_id = "client id",
        .tenant_id = "common",
        .auth_endpoint = "https://login.example.test",
        .scope = "User.Read Files.ReadWrite offline_access"
    };
}

std::string parameter(const std::string& url, const std::string& name) {
    const auto position = url.find(name + "=");
    if (position == std::string::npos)
        throw std::logic_error{"missing test parameter"};
    const auto start = position + name.size() + 1;
    return url.substr(start, url.find('&', start) - start);
}

int test_success() {
    auth::AuthCodeSession session{options()};
    if (session.state() != auth::AuthCodeState::created) {
        return test::fail("browser session did not start in created state");
    }
    const auto url = session.begin("http://localhost:54321/callback");
    if (!url || session.state() != auth::AuthCodeState::awaiting_callback ||
        !url->starts_with(
            "https://login.example.test/common/oauth2/v2.0/authorize?"
        ) ||
        parameter(*url, "client_id") != "client%20id" ||
        parameter(*url, "redirect_uri") !=
            "http%3A%2F%2Flocalhost%3A54321%2Fcallback" ||
        parameter(*url, "response_type") != "code" ||
        parameter(*url, "code_challenge_method") != "S256" ||
        parameter(*url, "state").size() != 43 ||
        url->contains("client_secret")) {
        return test::fail(
            "core generated an invalid browser authorization URL"
        );
    }
    const auto state = parameter(*url, "state");
    auto wrong_state = state;
    wrong_state[0] = state[0] == 'a' ? 'b' : 'a';
    const std::string callback =
        "http://localhost:54321/callback?state=" + state;
    for (const auto& invalid : {
             callback + "&code=a&code=b",
             callback + "&%63ode=a&code=b",
             callback + "&state=" + state + "&code=a",
             callback + "&code=a&error=access_denied",
             callback + "&code=",
             callback,
             callback + "&code=%ZZ",
             callback + "&code=%00",
             callback + "&code=%",
             callback + "&code=%A",
             callback + "&code=%7F",
             callback + "&%ZZ=value&code=a",
             callback + "&error=",
             callback + "&error=a&error=b",
             std::string{"http://localhost:54321/callback?state="} +
                 wrong_state + "&code=a",
             callback + "&code=a#fragment",
             callback + "&code=" + std::string(8192, 'x'),
             std::string{"http://localhost:54321/wrong?state="} + state +
                 "&code=a",
             std::string{"http://localhost:54322/callback?state="} + state +
                 "&code=a",
             std::string{"http://127.0.0.1:54321/callback?state="} + state +
                 "&code=a",
             std::string{"http://localhost:54321/callback?state=wrong&code=a"},
         }) {
        if (session.accept_callback(invalid) ||
            session.state() != auth::AuthCodeState::awaiting_callback) {
            return test::fail("invalid callback consumed the core session");
        }
    }
    if (!session.accept_callback(callback + "&code=test%2Bcode") ||
        session.state() != auth::AuthCodeState::authorized ||
        !test::throws_with<std::logic_error>(
            [&] {
                static_cast<void>(
                    session.accept_callback(callback + "&code=replay")
                );
            },
            "already consumed"
        )) {
        return test::fail("core failed to authorize or reject callback replay");
    }
    test::QueuedHttpTransport transport{{
        http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})"
        },
    }};
    http::HttpTransport proxy{util::borrowed_proxy, transport};
    auth::DeviceAuthClient client{&proxy, options()};
    const auto tokens = session.exchange(client);
    if (!tokens || tokens->refresh_token != "refresh" ||
        session.state() != auth::AuthCodeState::completed ||
        transport.requests.size() != 1 ||
        parameter(transport.requests[0].body, "code") != "test%2Bcode" ||
        parameter(transport.requests[0].body, "redirect_uri") !=
            parameter(*url, "redirect_uri")) {
        return test::fail(
            "core token exchange lost callback data or final state"
        );
    }
    const auto verifier =
        parameter(transport.requests[0].body, "code_verifier");
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    EVP_Digest(
        verifier.data(),
        verifier.size(),
        digest.data(),
        &size,
        EVP_sha256(),
        nullptr
    );
    std::string challenge(45, '\0');
    EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(challenge.data()), digest.data(), 32
    );
    for (auto& character : challenge) {
        if (character == '+')
            character = '-';
        if (character == '/')
            character = '_';
    }
    challenge.resize(43);
    if (verifier.size() != 43 ||
        parameter(*url, "code_challenge") != challenge ||
        !test::throws_with<std::logic_error>(
            [&] { static_cast<void>(session.exchange(client)); }, "not waiting"
        ) ||
        !test::throws_with<std::logic_error>(
            [&] { static_cast<void>(session.begin("http://localhost/")); },
            "already started"
        ) ||
        transport.requests.size() != 1) {
        return test::fail(
            "core PKCE challenge was incorrect or completed session was "
            "reusable"
        );
    }
    session.fail();
    if (session.state() != auth::AuthCodeState::completed) {
        return test::fail("failure changed a completed session");
    }
    auth::AuthCodeSession second{options()};
    const auto second_url = second.begin("http://localhost:54321/callback");
    if (!second_url || parameter(*second_url, "state") == state ||
        parameter(*second_url, "code_challenge") == challenge) {
        return test::fail("core reused state or verifier between sessions");
    }
    return EXIT_SUCCESS;
}

int test_terminal_states() {
    using namespace std::chrono_literals;
    auto now = std::chrono::steady_clock::time_point{};
    for (const int stage : {0, 1, 2}) {
        auth::AuthCodeSession session{options(), [&] { return now; }};
        std::stop_source stop;
        if (stage == 0)
            stop.request_stop();
        const auto url =
            session.begin("http://localhost:12345/", stop.get_token());
        if (stage == 0) {
            if (url || session.state() != auth::AuthCodeState::cancelled) {
                return test::fail("pre-cancelled core session started");
            }
            continue;
        }
        if (!url)
            return test::fail("core cancellation test could not start");
        if (stage == 2 && !session.accept_callback(
                              "http://localhost:12345/?state=" +
                              parameter(*url, "state") + "&code=a"
                          )) {
            return test::fail("core cancellation test could not authorize");
        }
        stop.request_stop();
        const auto result = session.check(stop.get_token());
        if (result || result.error().code != auth::AuthErrorCode::cancelled ||
            session.state() != auth::AuthCodeState::cancelled ||
            session.check()) {
            return test::fail("core cancellation was not terminal");
        }
    }
    for (const bool authorized : {false, true}) {
        auth::AuthCodeSession session{options(), [&] { return now; }, 5s};
        const auto url = session.begin("http://localhost:12345/");
        if (!url)
            return test::fail("core timeout test could not start");
        if (authorized) {
            static_cast<void>(session.accept_callback(
                "http://localhost:12345/?state=" + parameter(*url, "state") +
                "&code=a"
            ));
        }
        now += 4999ms;
        if (!session.check())
            return test::fail("core session expired before the deadline");
        now += 1ms;
        const auto expired = session.check();
        if (expired || expired.error().code != auth::AuthErrorCode::expired ||
            session.state() != auth::AuthCodeState::expired ||
            session.accept_callback("http://localhost:12345/")) {
            return test::fail(
                "core deadline or expired terminal state was incorrect"
            );
        }
    }
    for (const std::string error : {"access_denied", "server_error"}) {
        auth::AuthCodeSession session{options()};
        const auto url = session.begin("http://localhost:12345/");
        const auto denied = session.accept_callback(
            "http://localhost:12345/?state=" + parameter(*url, "state") +
            "&error=" + error
        );
        if (denied || session.state() != auth::AuthCodeState::failed ||
            denied.error().code !=
                (error == "access_denied"
                     ? auth::AuthErrorCode::authorization_declined
                     : auth::AuthErrorCode::server)) {
            return test::fail(
                "core failed to terminate on authorization server rejection"
            );
        }
    }
    return EXIT_SUCCESS;
}

struct InspectingTransport {
    auth::AuthCodeSession* session;
    bool throwing;
    mutable bool observed{false};
    http::HttpResult perform(const http::HttpRequest&) const {
        observed = session->state() == auth::AuthCodeState::exchanging_token;
        if (throwing)
            throw std::runtime_error{"test transport exception"};
        return std::unexpected(
            http::HttpError{.message = "test transport failure"}
        );
    }
    http::HttpResult download(
        const http::HttpRequest&,
        const std::filesystem::path&,
        const http::DownloadProgress&,
        const http::DownloadData&,
        const http::DownloadCheckpoint&,
        const http::DownloadResponseGate&
    ) const {
        throw std::logic_error{"unexpected download"};
    }
};

int test_invalid_transitions_and_exchange_failures() {
    for (const bool throwing : {false, true}) {
        auth::AuthCodeSession session{options()};
        InspectingTransport transport{&session, throwing};
        http::HttpTransport proxy{util::borrowed_proxy, transport};
        auth::DeviceAuthClient client{&proxy, options()};
        if (!test::throws_with<std::logic_error>(
                [&] { static_cast<void>(session.check()); }, "not waiting"
            )) {
            return test::fail("unstarted core session accepted a wait");
        }
        const auto url = session.begin("http://localhost:12345/");
        if (!test::throws_with<std::logic_error>(
                [&] { static_cast<void>(session.exchange(client)); },
                "not received"
            )) {
            return test::fail("core exchanged a token before authorization");
        }
        static_cast<void>(session.accept_callback(
            "http://localhost:12345/?state=" + parameter(*url, "state") +
            "&code=a"
        ));
        if (throwing) {
            if (!test::throws_with<std::runtime_error>(
                    [&] { static_cast<void>(session.exchange(client)); },
                    "test transport exception"
                )) {
                return test::fail("core swallowed an exchange exception");
            }
        } else {
            const auto result = session.exchange(client);
            if (result ||
                result.error().code != auth::AuthErrorCode::transport) {
                return test::fail("core swallowed an exchange error");
            }
        }
        if (!transport.observed ||
            session.state() != auth::AuthCodeState::failed || session.check()) {
            return test::fail(
                "core failed exchange did not enter a terminal state"
            );
        }
    }
    for (const std::string redirect :
         {"https://localhost/",
          "http://example.test/",
          "http://localhost/?x=y",
          "http://localhost/#fragment",
          "http://user:password@localhost/",
          "http://localhost:invalid/",
          "http://localhost/?",
          "onedrivecpp://oauth/callback"}) {
        auth::AuthCodeSession session{options()};
        const auto result = session.begin(redirect);
        if (result ||
            result.error().code != auth::AuthErrorCode::invalid_configuration ||
            session.state() != auth::AuthCodeState::failed) {
            return test::fail("core accepted an unsupported redirect");
        }
    }
    for (const auto& endpoint : {
             std::string{"https://user:password@login.example.test"},
             std::string{"https://login.example.test?"},
             std::string{"https://login.example.test/#fragment"},
             std::string{"https://login.example.test"} + '\0' + "ignored",
         }) {
        auto invalid_options = options();
        invalid_options.auth_endpoint = endpoint;
        auth::AuthCodeSession session{invalid_options};
        const auto result = session.begin("http://localhost/");
        if (result || session.state() != auth::AuthCodeState::failed) {
            return test::fail(
                "core accepted an invalid authorization endpoint"
            );
        }
    }
    auto no_offline_access = options();
    no_offline_access.scope = "User.Read Files.ReadWrite not_offline_access";
    auth::AuthCodeSession missing_scope{no_offline_access};
    const auto rejected = missing_scope.begin("http://localhost/");
    if (rejected ||
        rejected.error().code != auth::AuthErrorCode::invalid_configuration) {
        return test::fail(
            "Auth Code did not apply shared offline_access validation"
        );
    }
    auto invalid_options = options();
    invalid_options.auth_endpoint = "http://login.example.test";
    auth::AuthCodeSession invalid{invalid_options};
    if (invalid.begin("http://localhost/") ||
        !test::throws_with<std::invalid_argument>(
            [] {
                auth::AuthCodeSession session{
                    options(), {}, std::chrono::seconds::zero()
                };
            },
            "positive"
        )) {
        return test::fail(
            "core accepted an insecure endpoint or nonpositive timeout"
        );
    }
    return EXIT_SUCCESS;
}

int test_entrypoint_cancellation_and_deadlines() {
    using namespace std::chrono_literals;
    for (const bool cancelled : {false, true}) {
        for (const bool exchange : {false, true}) {
            auto now = std::chrono::steady_clock::time_point{};
            auth::AuthCodeSession session{options(), [&] { return now; }, 5s};
            const auto url = session.begin("http://localhost:12345/");
            if (!url)
                return test::fail("entrypoint test could not begin");
            const auto callback =
                "http://localhost:12345/?state=" + parameter(*url, "state") +
                "&code=a";
            if (exchange && !session.accept_callback(callback)) {
                return test::fail("entrypoint test could not authorize");
            }
            std::stop_source stop;
            if (cancelled)
                stop.request_stop();
            else
                now += 5s;
            test::QueuedHttpTransport transport{{}};
            http::HttpTransport proxy{util::borrowed_proxy, transport};
            auth::DeviceAuthClient client{&proxy, options()};
            const auto expected = cancelled ? auth::AuthErrorCode::cancelled
                                            : auth::AuthErrorCode::expired;
            if (exchange) {
                const auto result = session.exchange(client, stop.get_token());
                if (result || result.error().code != expected) {
                    return test::fail(
                        "exchange ignored cancellation or deadline"
                    );
                }
            } else {
                const auto result =
                    session.accept_callback(callback, stop.get_token());
                if (result || result.error().code != expected) {
                    return test::fail(
                        "callback ignored cancellation or deadline"
                    );
                }
            }
            if (!transport.requests.empty() ||
                session.state() != (cancelled ? auth::AuthCodeState::cancelled
                                              : auth::AuthCodeState::expired)) {
                return test::fail(
                    "terminal entrypoint sent HTTP or lost terminal state"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

struct CancellingTransport {
    auth::AuthCodeSession* session;
    std::stop_source* stop;
    mutable bool observed{false};
    http::HttpResult perform(const http::HttpRequest& request) const {
        observed = session->state() == auth::AuthCodeState::exchanging_token &&
                   request.stop_token == stop->get_token();
        stop->request_stop();
        return http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})"
        };
    }
    http::HttpResult download(
        const http::HttpRequest&,
        const std::filesystem::path&,
        const http::DownloadProgress&,
        const http::DownloadData&,
        const http::DownloadCheckpoint&,
        const http::DownloadResponseGate&
    ) const {
        throw std::logic_error{"unexpected download"};
    }
};

int test_exchange_outcomes() {
    auth::AuthCodeSession cancelled{options()};
    const auto url = cancelled.begin("http://localhost:12345/");
    if (!url || !cancelled.accept_callback(
                    "http://localhost:12345/?state=" +
                    parameter(*url, "state") + "&code=a"
                )) {
        return test::fail("exchange cancellation test could not authorize");
    }
    std::stop_source stop;
    CancellingTransport cancelling{&cancelled, &stop};
    http::HttpTransport cancelling_proxy{util::borrowed_proxy, cancelling};
    auth::DeviceAuthClient cancelling_client{&cancelling_proxy, options()};
    const auto result = cancelled.exchange(cancelling_client, stop.get_token());
    const auto retry = cancelled.exchange(cancelling_client);
    if (result || retry || !cancelling.observed ||
        result.error().code != auth::AuthErrorCode::cancelled ||
        retry.error().code != auth::AuthErrorCode::cancelled ||
        cancelled.state() != auth::AuthCodeState::cancelled) {
        return test::fail(
            "in-flight cancellation accepted tokens or allowed another exchange"
        );
    }
    for (const auto& response : {
             http::HttpResponse{
                 .status_code = 400, .body = R"({"error":"invalid_grant"})"
             },
             http::HttpResponse{.status_code = 200, .body = "invalid JSON"},
             http::HttpResponse{
                 .status_code = 200,
                 .body = R"({"expires_in":3600,"refresh_token":"refresh"})"
             },
             http::HttpResponse{
                 .status_code = 200,
                 .body = R"({"expires_in":3600,"access_token":"access"})"
             },
         }) {
        auth::AuthCodeSession session{options()};
        const auto address = session.begin("http://localhost:12345/");
        if (!address || !session.accept_callback(
                            "http://localhost:12345/?state=" +
                            parameter(*address, "state") + "&code=a"
                        )) {
            return test::fail("exchange response test could not authorize");
        }
        test::QueuedHttpTransport transport{{response}};
        http::HttpTransport proxy{util::borrowed_proxy, transport};
        auth::DeviceAuthClient client{&proxy, options()};
        const auto failed = session.exchange(client);
        const auto repeated = session.exchange(client);
        const auto expected = response.status_code == 400
                                  ? auth::AuthErrorCode::server
                                  : auth::AuthErrorCode::invalid_response;
        if (failed || repeated || failed.error().code != expected ||
            repeated.error().code != expected ||
            session.state() != auth::AuthCodeState::failed ||
            transport.requests.size() != 1) {
            return test::fail(
                "failed token response lost error/state or retried the exchange"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_explicit_failures_and_clock_exception() {
    for (const auto code :
         {auth::AuthErrorCode::cancelled,
          auth::AuthErrorCode::expired,
          auth::AuthErrorCode::server}) {
        auth::AuthCodeSession session{options()};
        if (!session.begin("http://localhost/"))
            return test::fail("failure test could not begin");
        session.fail({.code = code, .message = "original failure"});
        session.fail(
            {.code = auth::AuthErrorCode::transport, .message = "replacement"}
        );
        const auto result = session.check();
        const auto expected = code == auth::AuthErrorCode::cancelled
                                  ? auth::AuthCodeState::cancelled
                              : code == auth::AuthErrorCode::expired
                                  ? auth::AuthCodeState::expired
                                  : auth::AuthCodeState::failed;
        if (result || result.error().code != code ||
            result.error().message != "original failure" ||
            session.state() != expected) {
            return test::fail(
                "explicit failure did not preserve its original terminal "
                "outcome"
            );
        }
    }
    auth::AuthCodeSession failed{options()};
    failed.fail();
    const auto default_error = failed.check();
    if (default_error ||
        default_error.error().code != auth::AuthErrorCode::server ||
        failed.state() != auth::AuthCodeState::failed) {
        return test::fail(
            "default failure did not terminate an unstarted session"
        );
    }
    auth::AuthCodeSession throwing{
        options(), []() -> std::chrono::steady_clock::time_point {
            throw std::runtime_error{"test clock failure"};
        }
    };
    if (!test::throws_with<std::runtime_error>(
            [&] { static_cast<void>(throwing.begin("http://localhost/")); },
            "test clock failure"
        ) ||
        throwing.state() != auth::AuthCodeState::failed || throwing.check()) {
        return test::fail(
            "begin exception did not terminate the session and propagate"
        );
    }
    return EXIT_SUCCESS;
}

int test_url_variants_and_missing_options() {
    for (const std::string redirect :
         {"http://localhost/",
          "http://127.0.0.1:12345/callback",
          "http://[::1]:12345/callback"}) {
        auto configured = options();
        configured.auth_endpoint += "///";
        auth::AuthCodeSession session{configured};
        const auto url = session.begin(redirect);
        if (!url ||
            !url->starts_with(
                "https://login.example.test/common/oauth2/v2.0/authorize?"
            ) ||
            parameter(*url, "response_mode") != "query" ||
            parameter(*url, "scope") !=
                "User.Read%20Files.ReadWrite%20offline_access" ||
            !session.accept_callback(
                redirect + "?state=" + parameter(*url, "state") +
                "&unknown=value&%63ode=lower%2fupper%2F+plus"
            )) {
            return test::fail(
                "valid URL variant or query decoding was rejected"
            );
        }
        test::QueuedHttpTransport transport{{http::HttpResponse{
            .status_code = 200,
            .body =
                R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})"
        }}};
        http::HttpTransport proxy{util::borrowed_proxy, transport};
        auth::DeviceAuthClient client{&proxy, configured};
        if (!session.exchange(client) || transport.requests.size() != 1 ||
            parameter(transport.requests[0].body, "code") !=
                "lower%2Fupper%2F%2Bplus") {
            return test::fail(
                "callback decoding did not preserve code punctuation"
            );
        }
    }
    for (const int field : {0, 1, 2}) {
        auto configured = options();
        if (field == 0)
            configured.application_id.clear();
        if (field == 1)
            configured.tenant_id.clear();
        if (field == 2)
            configured.scope.clear();
        auth::AuthCodeSession session{configured};
        const auto result = session.begin("http://localhost/");
        if (result ||
            result.error().code != auth::AuthErrorCode::invalid_configuration ||
            session.state() != auth::AuthCodeState::failed) {
            return test::fail("missing authentication option was accepted");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_success() != EXIT_SUCCESS ||
        test_terminal_states() != EXIT_SUCCESS ||
        test_invalid_transitions_and_exchange_failures() != EXIT_SUCCESS ||
        test_entrypoint_cancellation_and_deadlines() != EXIT_SUCCESS ||
        test_exchange_outcomes() != EXIT_SUCCESS ||
        test_explicit_failures_and_clock_exception() != EXIT_SUCCESS ||
        test_url_variants_and_missing_options() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
