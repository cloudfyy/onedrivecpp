#include "ui/gui/browser_request.hpp"
#include "support/common.hpp"

namespace {

using namespace onedrive::gui::detail;
using namespace std::chrono_literals;
using Commands = std::vector<BrowserCommand>;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}

const std::string valid_request =
    "GET /?state=s&code=c HTTP/1.1\r\nHost: localhost:1234\r\n\r\n";

void test_fragments_and_validation() {
    for (std::size_t split = 0; split < valid_request.size(); ++split) {
        BrowserRequest request;
        require(
            request.connected(0ms) == Commands{BrowserCommand::read},
            "connection did not enter reading"
        );
        require(
            request.bytes(
                std::string_view{valid_request}.substr(0, split), 1ms
            ) == Commands{BrowserCommand::read},
            "incomplete header entered validation"
        );
        require(
            request.bytes(std::string_view{valid_request}.substr(split), 2ms) ==
                Commands{BrowserCommand::validate},
            "fragmented header did not enter validation"
        );
        require(
            browser_request_target(request.request(), "LOCALHOST:1234") ==
                "/?state=s&code=c",
            "fragmented request lost its target or host matching"
        );
    }
    for (const auto request : {
             "",
             "GET / HTTP/1.1\r\nHost: localhost:1234\r\n",
             "GET / HTTP/1.1\r\n\r\n",
             "POST / HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET / HTTP/1.0\r\nHost: localhost:1234\r\n\r\n",
             "GET / extra HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET relative HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET //evil/ HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /#fragment HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /% HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /%A HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /%ZZ HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /%0Z HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /%/0 HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /%g0 HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /%G0 HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /\t HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET /\x7f HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
             "GET / HTTP/1.1\r\nHost: evil\r\n\r\n",
             "GET / HTTP/1.1\r\nHost: localhosx:1234\r\n\r\n",
             "GET / HTTP/1.1\r\nTest: localhost:1234\r\n\r\n",
             "GET / HTTP/1.1\r\nHost:\n\r\n\r\n",
             "GET / HTTP/1.1\r\nHost: \t\r\n\r\n",
             "GET / HTTP/1.1\r\nHost: localhost:1234\r\nHOST: "
             "localhost:1234\r\n\r\n",
         }) {
        require(
            !browser_request_target(request, "localhost:1234"),
            "malformed request passed HTTP validation"
        );
    }
    require(
        browser_request_target(
            "GET / HTTP/1.1\r\nhOsT:\tLOCALHOST:1234 \t\r\n\r\n",
            "localhost:1234"
        ) == "/",
        "valid host case/whitespace variant was rejected"
    );
    require(
        browser_request_target(
            "GET /%aa%BB%00 HTTP/1.1\r\nHost: localhost:1234\r\n\r\n",
            "localhost:1234"
        ) == "/%aa%BB%00",
        "valid percent-encoded target was rejected before callback validation"
    );
    require(
        browser_request_target(
            "GET / HTTP/1.1\r\nHost: localhost:1234\r\n\r\nignored",
            "localhost:1234"
        ) == "/",
        "unrelated trailing bytes changed callback target validation"
    );
    require(
        !browser_request_target(
            valid_request + std::string(BrowserRequest::request_limit, 'x'),
            "localhost:1234"
        ),
        "oversized complete request passed validation"
    );
}

void test_deadlines_and_bounds() {
    BrowserRequest request;
    request.connected(100ms);
    require(
        request.tick(2099ms) == Commands{BrowserCommand::read},
        "request expired early"
    );
    require(
        request.tick(2100ms) == Commands{BrowserCommand::write} &&
            request.remaining_reply().starts_with("HTTP/1.1 400"),
        "request deadline did not reject incomplete input"
    );
    require(
        request.tick(2199ms) == Commands{BrowserCommand::write},
        "response expired early"
    );
    require(
        request.tick(2200ms) ==
            Commands{BrowserCommand::close_connection, BrowserCommand::listen},
        "write deadline did not close and resume listening"
    );
    for (const std::size_t size : {8191U, 8192U, 8193U}) {
        BrowserRequest bounded;
        bounded.connected(0ms);
        const auto result = bounded.bytes(std::string(size, 'x'), 1ms);
        require(
            result ==
                Commands{
                    size < 8192 ? BrowserCommand::read : BrowserCommand::write
                },
            "request cap boundary was incorrect"
        );
        require(
            bounded.request().size() < BrowserRequest::request_limit,
            "reducer retained oversized input"
        );
    }
    BrowserRequest late;
    late.connected(0ms);
    require(
        late.bytes(valid_request, 2000ms) == Commands{BrowserCommand::write} &&
            late.remaining_reply().starts_with("HTTP/1.1 400"),
        "complete input at the deadline bypassed timeout"
    );
    BrowserRequest incremental;
    incremental.connected(0ms);
    incremental.bytes(std::string(8190, 'x'), 1ms);
    require(
        incremental.bytes("xx", 2ms) == Commands{BrowserCommand::write},
        "fragmented oversized request bypassed its cap"
    );
}

void test_partial_writes_and_connections() {
    BrowserRequest request;
    for (const auto outcome :
         {CallbackOutcome::invalid,
          CallbackOutcome::invalid,
          CallbackOutcome::accepted}) {
        request.connected(0ms);
        request.bytes(valid_request, 1ms);
        require(
            request.validated(outcome, 2ms) == Commands{BrowserCommand::write},
            "callback result did not enter writing"
        );
        require(
            request.remaining_reply().starts_with(
                outcome == CallbackOutcome::invalid ? "HTTP/1.1 400"
                                                    : "HTTP/1.1 200"
            ),
            "callback reply status was incorrect"
        );
        const auto full = std::string{request.remaining_reply()};
        require(
            request.accepted(0, 3ms) == Commands{BrowserCommand::write},
            "zero accepted bytes were confused with flush completion"
        );
        require(
            request.accepted(7, 4ms) == Commands{BrowserCommand::write} &&
                request.remaining_reply() == std::string_view{full}.substr(7),
            "partial write did not retain the unaccepted suffix"
        );
        require(
            request.flushed(0, 4ms) == Commands{BrowserCommand::write},
            "draining a partial write lost its unaccepted suffix"
        );
        require(
            request.accepted(full.size() - 7, 5ms) ==
                Commands{BrowserCommand::flush},
            "fully accepted reply did not wait for Qt buffer flushing"
        );
        require(
            request.flushed(1, 6ms) == Commands{BrowserCommand::flush},
            "accepted bytes were incorrectly treated as drained"
        );
        require(
            request.flushed(0, 7ms) ==
                Commands{
                    BrowserCommand::close_connection,
                    outcome == CallbackOutcome::invalid ? BrowserCommand::listen
                                                        : BrowserCommand::finish
                },
            "close did not preserve callback terminal status"
        );
    }
    require(
        request.phase() == BrowserPhase::closed && request.cancel().empty(),
        "terminal callback was reusable"
    );
    for (const auto outcome :
         {CallbackOutcome::rejected, CallbackOutcome::interrupted}) {
        BrowserRequest terminal;
        terminal.connected(0ms);
        terminal.bytes(valid_request, 1ms);
        terminal.validated(outcome, 2ms);
        require(
            terminal.remaining_reply().starts_with(
                outcome == CallbackOutcome::rejected ? "HTTP/1.1 200"
                                                     : "HTTP/1.1 400"
            ),
            "terminal error response changed HTTP semantics"
        );
        require(
            terminal.peer_closed(3ms) ==
                Commands{
                    BrowserCommand::close_connection, BrowserCommand::finish
                },
            "peer closure lost terminal callback outcome"
        );
    }
}

void test_cancellation_and_invalid_events() {
    for (const int stage : {0, 1, 2, 3}) {
        BrowserRequest request;
        if (stage > 0)
            request.connected(0ms);
        if (stage > 1)
            request.bytes(valid_request, 1ms);
        if (stage > 2)
            request.validated(CallbackOutcome::invalid, 2ms);
        require(
            request.cancel() ==
                    Commands{
                        BrowserCommand::close_connection, BrowserCommand::finish
                    } &&
                request.cancel().empty(),
            "cancellation was not terminal and idempotent"
        );
    }
    BrowserRequest peer;
    peer.connected(0ms);
    require(
        peer.peer_closed(1ms) == Commands{BrowserCommand::write},
        "incomplete peer closure did not produce 400"
    );
    require(
        peer.peer_closed(2ms) ==
            Commands{BrowserCommand::close_connection, BrowserCommand::listen},
        "closed invalid peer stopped authorization"
    );
    BrowserRequest invalid;
    require(
        onedrive::test::throws_with<std::logic_error>([&] {
            invalid.bytes("x", 0ms);
        }) &&
            onedrive::test::throws_with<std::logic_error>([&] {
                invalid.tick(0ms);
            }) &&
            onedrive::test::throws_with<std::logic_error>([&] {
                invalid.peer_closed(0ms);
            }),
        "illegal lifecycle events were accepted"
    );
    invalid.connected(0ms);
    require(
        onedrive::test::throws_with<std::logic_error>([&] {
            invalid.validated(CallbackOutcome::accepted, 0ms);
        }),
        "callback validation bypassed request completion"
    );
    invalid.bytes(valid_request, 1ms);
    invalid.validated(CallbackOutcome::accepted, 2ms);
    require(
        onedrive::test::throws_with<std::logic_error>([&] {
            invalid.accepted(10000, 3ms);
        }),
        "over-reported accepted bytes were allowed"
    );
    const auto size = invalid.remaining_reply().size();
    invalid.accepted(size, 3ms);
    require(
        invalid.flushed(1, 102ms) ==
            Commands{BrowserCommand::close_connection, BrowserCommand::finish},
        "blocked flush did not obey its deadline"
    );
}

} // namespace

int main() {
    try {
        test_fragments_and_validation();
        test_deadlines_and_bounds();
        test_partial_writes_and_connections();
        test_cancellation_and_invalid_events();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        return onedrive::test::fail(error.what());
    }
}
