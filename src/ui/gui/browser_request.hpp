#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onedrive::gui::detail {

enum class BrowserPhase { listening, reading, validating, writing, closed };
enum class BrowserCommand {
    read,
    validate,
    write,
    flush,
    close_connection,
    listen,
    finish,
};
enum class CallbackOutcome { invalid, accepted, rejected, interrupted };

// One reducer can process several invalid connections before a terminal
// callback.
class BrowserRequest {
public:
    using Time = std::chrono::milliseconds;
    static constexpr std::size_t request_limit = 8192;
    static constexpr Time request_timeout{2000};
    static constexpr Time write_timeout{100};

    [[nodiscard]] BrowserPhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] const std::string& request() const noexcept {
        return request_;
    }
    [[nodiscard]] std::string_view remaining_reply() const noexcept {
        return std::string_view{reply_}.substr(written_);
    }

    std::vector<BrowserCommand> connected(Time now) {
        require(BrowserPhase::listening);
        request_.clear();
        deadline_ = now + request_timeout;
        phase_ = BrowserPhase::reading;
        return {BrowserCommand::read};
    }
    std::vector<BrowserCommand> bytes(std::string_view data, Time now) {
        require(BrowserPhase::reading);
        if (now >= deadline_ ||
            data.size() >= request_limit - request_.size()) {
            return reply(CallbackOutcome::invalid, now);
        }
        request_.append(data);
        if (request_.find("\r\n\r\n") != std::string::npos) {
            phase_ = BrowserPhase::validating;
            return {BrowserCommand::validate};
        }
        return {BrowserCommand::read};
    }
    std::vector<BrowserCommand> tick(Time now) {
        if (phase_ == BrowserPhase::reading) {
            return now >= deadline_ ? reply(CallbackOutcome::invalid, now)
                                    : std::vector{BrowserCommand::read};
        }
        if (phase_ == BrowserPhase::writing) {
            if (now >= deadline_)
                return close();
            return {
                written_ == reply_.size() ? BrowserCommand::flush
                                          : BrowserCommand::write
            };
        }
        throw std::logic_error{"browser request cannot tick in this phase"};
    }
    std::vector<BrowserCommand> validated(CallbackOutcome outcome, Time now) {
        require(BrowserPhase::validating);
        return reply(outcome, now);
    }
    // Qt write() reports bytes accepted into its buffer, not bytes sent.
    std::vector<BrowserCommand> accepted(std::size_t bytes, Time now) {
        require(BrowserPhase::writing);
        if (bytes > reply_.size() - written_)
            throw std::logic_error{"browser reply write exceeded its size"};
        written_ += bytes;
        return tick(now);
    }
    std::vector<BrowserCommand> flushed(std::size_t pending, Time now) {
        require(BrowserPhase::writing);
        if (written_ == reply_.size() && pending == 0)
            return close();
        return tick(now);
    }
    std::vector<BrowserCommand> peer_closed(Time now) {
        if (phase_ == BrowserPhase::reading)
            return reply(CallbackOutcome::invalid, now);
        if (phase_ == BrowserPhase::writing)
            return close();
        throw std::logic_error{"unexpected browser connection closure"};
    }
    std::vector<BrowserCommand> cancel() {
        if (phase_ == BrowserPhase::closed)
            return {};
        terminal_ = true;
        return close();
    }

private:
    std::vector<BrowserCommand> reply(CallbackOutcome outcome, Time now) {
        terminal_ = outcome != CallbackOutcome::invalid;
        const bool received = outcome == CallbackOutcome::accepted ||
                              outcome == CallbackOutcome::rejected;
        const std::string body =
            received ? "Authorization received. Return to OneDrive C++. "
                       "You may close this tab."
                     : "Invalid authorization callback.";
        reply_ =
            received ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n";
        reply_ += "Content-Type: text/plain; charset=utf-8\r\nCache-Control: "
                  "no-store\r\nConnection: close\r\nContent-Length: " +
                  std::to_string(body.size()) + "\r\n\r\n" + body;
        written_ = 0;
        deadline_ = now + write_timeout;
        phase_ = BrowserPhase::writing;
        return {BrowserCommand::write};
    }
    void require(BrowserPhase phase) const {
        if (phase_ != phase)
            throw std::logic_error{"unexpected browser request event"};
    }
    std::vector<BrowserCommand> close() {
        phase_ = terminal_ ? BrowserPhase::closed : BrowserPhase::listening;
        return {
            BrowserCommand::close_connection,
            terminal_ ? BrowserCommand::finish : BrowserCommand::listen
        };
    }

    BrowserPhase phase_{BrowserPhase::listening};
    Time deadline_{};
    std::string request_;
    std::string reply_;
    std::size_t written_{0};
    bool terminal_{false};
};

inline std::optional<std::string_view>
browser_request_target(std::string_view request, std::string_view host) {
    if (request.size() >= BrowserRequest::request_limit ||
        request.find("\r\n\r\n") == std::string_view::npos)
        return std::nullopt;
    const auto end = request.find("\r\n");
    const auto line = request.substr(0, end);
    if (!line.starts_with("GET ") || !line.ends_with(" HTTP/1.1"))
        return std::nullopt;
    const auto target = line.substr(4, line.size() - 4 - 9);
    if (!target.starts_with('/') || target.starts_with("//") ||
        target.find_first_of(" #\r\n") != std::string_view::npos)
        return std::nullopt;
    const auto hex = [](char value) {
        return (value >= '0' && value <= '9') ||
               (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
    };
    for (std::size_t index = 0; index < target.size(); ++index) {
        const auto value = static_cast<unsigned char>(target[index]);
        if (value < 32 || value == 127)
            return std::nullopt;
        if (value == '%') {
            if (index + 2 >= target.size() || !hex(target[index + 1]) ||
                !hex(target[index + 2]))
                return std::nullopt;
            index += 2;
        }
    }
    const auto lower = [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
    };
    const auto equal = [&](std::string_view left, std::string_view right) {
        return left.size() == right.size() &&
               std::equal(
                   left.begin(),
                   left.end(),
                   right.begin(),
                   [&](auto a, auto b) { return lower(a) == lower(b); }
               );
    };
    unsigned int hosts = 0;
    bool matches = false;
    request.remove_prefix(end + 2);
    while (!request.empty()) {
        const auto next = request.find('\n');
        auto header = request.substr(0, next);
        if (header.size() >= 5 && equal(header.substr(0, 5), "host:")) {
            ++hosts;
            header.remove_prefix(5);
            while (!header.empty() &&
                   (header.front() == ' ' || header.front() == '\t'))
                header.remove_prefix(1);
            while (!header.empty() &&
                   (header.back() == ' ' || header.back() == '\t' ||
                    header.back() == '\r'))
                header.remove_suffix(1);
            matches = equal(header, host);
        }
        if (next == std::string_view::npos)
            break;
        request.remove_prefix(next + 1);
    }
    return hosts == 1 && matches ? std::optional{target} : std::nullopt;
}

} // namespace onedrive::gui::detail
