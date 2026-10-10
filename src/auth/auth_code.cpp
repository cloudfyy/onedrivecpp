#include "onedrive/auth/auth_code.hpp"

#include "util/uri.hpp"
#include "util/base64.hpp"
#include "onedrive/util/sha256.hpp"
#include "auth/options.hpp"
#include "auth/code_transaction.hpp"

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

namespace onedrive::auth {
namespace {

struct Url {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
};

std::optional<Url> parse_url(const std::string& value) {
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url{
        curl_url(), curl_url_cleanup
    };
    if (!url) {
        throw std::runtime_error{"cannot allocate authorization URL parser"};
    }
    if (value.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    const auto parsed =
        curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_PATH_AS_IS);
    if (parsed == CURLUE_OUT_OF_MEMORY) {
        throw std::runtime_error{
            "cannot allocate authorization URL components"
        };
    }
    if (parsed != CURLUE_OK) {
        return std::nullopt;
    }
    const auto get = [&](CURLUPart part) {
        char* raw = nullptr;
        const auto result = curl_url_get(url.get(), part, &raw, 0);
        const std::unique_ptr<char, decltype(&curl_free)> owned{raw, curl_free};
        if (result == CURLUE_OK) {
            return std::string{raw};
        }
        if (result == CURLUE_NO_USER || result == CURLUE_NO_PASSWORD ||
            result == CURLUE_NO_PORT || result == CURLUE_NO_QUERY) {
            return std::string{};
        }
        throw std::runtime_error{
            "cannot read authorization URL component: " +
            std::string{curl_url_strerror(result)}
        };
    };
    if (!get(CURLUPART_USER).empty() || !get(CURLUPART_PASSWORD).empty() ||
        value.find('#') != std::string::npos) {
        return std::nullopt;
    }
    return Url{
        get(CURLUPART_SCHEME),
        get(CURLUPART_HOST),
        get(CURLUPART_PORT),
        get(CURLUPART_PATH),
        get(CURLUPART_QUERY)
    };
}

std::string random_secret() {
    std::array<unsigned char, 32> bytes{};
    struct CleanBytes {
        explicit CleanBytes(std::array<unsigned char, 32>& buffer) noexcept
            : bytes{buffer} {
        }
        CleanBytes(const CleanBytes&) = delete;
        CleanBytes& operator=(const CleanBytes&) = delete;
        CleanBytes(CleanBytes&&) = delete;
        CleanBytes& operator=(CleanBytes&&) = delete;
        std::array<unsigned char, 32>& bytes;
        ~CleanBytes() {
            OPENSSL_cleanse(bytes.data(), bytes.size());
        }
    } cleanup{bytes};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error{
            "cannot generate browser authorization randomness"
        };
    }
    return util::base64url_encode(bytes);
}

std::optional<std::string> decode(std::string_view value) {
    std::string decoded;
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        char character = value[index];
        if (character == '%') {
            if (index + 2 >= value.size())
                return std::nullopt;
            const int high = hex(value[index + 1]);
            const int low = hex(value[index + 2]);
            if (high < 0 || low < 0)
                return std::nullopt;
            character = static_cast<char>(high * 16 + low);
            index += 2;
        }
        if (static_cast<unsigned char>(character) < 32 || character == 127) {
            return std::nullopt;
        }
        decoded.push_back(character);
    }
    return decoded;
}

AuthError invalid_callback() {
    return {
        .code = AuthErrorCode::invalid_response,
        .message = "invalid browser authorization callback"
    };
}

} // namespace

class AuthCodeSession::Implementation {
public:
    detail::CodeStorage transaction{detail::CreatedCode{}};

    [[nodiscard]] const AuthError* error() const noexcept {
        if (const auto* current =
                std::get_if<detail::CancelledCode>(&transaction))
            return &current->error;
        if (const auto* current =
                std::get_if<detail::ExpiredCode>(&transaction))
            return &current->error;
        if (const auto* current = std::get_if<detail::FailedCode>(&transaction))
            return &current->error;
        if (transaction.valueless_by_exception())
            std::terminate();
        return nullptr;
    }

    template <typename Terminal> void terminate(AuthError error) noexcept {
        const auto finish = [&](auto& current) noexcept {
            auto next = util::transition_transaction<Terminal>(
                std::move(current), [&](const auto&) noexcept {
                    return detail::CodeErrorPayload{std::move(error)};
                }
            );
            static_assert(std::is_nothrow_assignable_v<
                          detail::CodeStorage&,
                          decltype(next)>);
            transaction = std::move(next);
        };
        if (auto* current = std::get_if<detail::CreatedCode>(&transaction))
            finish(*current);
        else if (auto* waiting =
                     std::get_if<detail::AwaitingCode>(&transaction))
            finish(*waiting);
        else if (auto* authorized =
                     std::get_if<detail::AuthorizedCode>(&transaction))
            finish(*authorized);
        else if (auto* exchanging =
                     std::get_if<detail::ExchangingCode>(&transaction))
            finish(*exchanging);
        else if (transaction.valueless_by_exception())
            std::terminate();
    }
};

AuthCodeSession::AuthCodeSession(
    DeviceAuthOptions options, ClockFunction now, std::chrono::seconds timeout
)
    : options_{std::move(options)},
      now_{std::move(now)},
      timeout_{timeout},
      implementation_{std::make_unique<Implementation>()} {
    if (!now_)
        now_ = [] { return std::chrono::steady_clock::now(); };
    if (timeout_ <= std::chrono::seconds::zero()) {
        throw std::invalid_argument{
            "browser authorization timeout must be positive"
        };
    }
}

AuthCodeState AuthCodeSession::state() const noexcept {
    const auto& transaction = implementation_->transaction;
    if (std::holds_alternative<detail::CreatedCode>(transaction))
        return AuthCodeState::created;
    if (std::holds_alternative<detail::AwaitingCode>(transaction))
        return AuthCodeState::awaiting_callback;
    if (std::holds_alternative<detail::AuthorizedCode>(transaction))
        return AuthCodeState::authorized;
    if (std::holds_alternative<detail::ExchangingCode>(transaction))
        return AuthCodeState::exchanging_token;
    if (std::holds_alternative<detail::CompletedCode>(transaction))
        return AuthCodeState::completed;
    if (std::holds_alternative<detail::CancelledCode>(transaction))
        return AuthCodeState::cancelled;
    if (std::holds_alternative<detail::ExpiredCode>(transaction))
        return AuthCodeState::expired;
    if (std::holds_alternative<detail::FailedCode>(transaction))
        return AuthCodeState::failed;
    std::terminate();
}

AuthCodeSession::~AuthCodeSession() = default;

const AuthError&
AuthCodeSession::terminate(AuthCodeState state, AuthError error) {
    switch (state) {
    case AuthCodeState::cancelled:
        implementation_->terminate<detail::Cancelled>(std::move(error));
        break;
    case AuthCodeState::expired:
        implementation_->terminate<detail::Expired>(std::move(error));
        break;
    default:
        implementation_->terminate<detail::Failed>(std::move(error));
        break;
    }
    return *implementation_->error();
}

void AuthCodeSession::fail_exception() noexcept {
    try {
        fail();
    } catch (...) {
        // Termination must still cleanse secrets if constructing a message
        // fails.
        implementation_->terminate<detail::Failed>(
            {.code = AuthErrorCode::server, .message = {}}
        );
    }
}

void AuthCodeSession::fail(AuthError error) {
    if (state() == AuthCodeState::completed || implementation_->error())
        return;
    const auto state =
        error.code == AuthErrorCode::cancelled ? AuthCodeState::cancelled
        : error.code == AuthErrorCode::expired ? AuthCodeState::expired
                                               : AuthCodeState::failed;
    terminate(state, std::move(error));
}

AuthResult<std::string> AuthCodeSession::begin(
    const std::string& redirect_uri, const std::stop_token& stop_token
) {
    if (state() != AuthCodeState::created) {
        throw std::logic_error{
            "browser authorization session has already started"
        };
    }
    if (stop_token.stop_requested()) {
        return std::unexpected(terminate(
            AuthCodeState::cancelled,
            {.code = AuthErrorCode::cancelled,
             .message = "browser login cancelled"}
        ));
    }
    try {
        const auto endpoint = parse_url(options_.auth_endpoint);
        const auto redirect = parse_url(redirect_uri);
        if (!endpoint || endpoint->scheme != "https" ||
            endpoint->host.empty() || !endpoint->query.empty() ||
            options_.auth_endpoint.find('?') != std::string::npos ||
            !redirect || redirect->scheme != "http" ||
            (redirect->host != "localhost" && redirect->host != "127.0.0.1" &&
             redirect->host != "[::1]") ||
            !redirect->query.empty() ||
            redirect_uri.find('?') != std::string::npos ||
            !detail::valid_options(options_)) {
            return std::unexpected(terminate(
                AuthCodeState::failed,
                {.code = AuthErrorCode::invalid_configuration,
                 .message =
                     "browser login requires an application ID, tenant, "
                     "offline_access scope, a "
                     "valid HTTPS endpoint and an HTTP loopback redirect"}
            ));
        }
        detail::CallbackPayload payload{
            .redirect = redirect_uri,
            .deadline = {},
            .verifier = detail::CodeSecret{random_secret()},
            .csrf = detail::CodeSecret{random_secret()},
        };
        const auto challenge =
            util::base64url_encode(util::sha256(payload.verifier.get()));
        auto url = options_.auth_endpoint;
        while (url.ends_with('/'))
            url.pop_back();
        url += "/" + util::percent_encode_uri_component(options_.tenant_id) +
               "/oauth2/v2.0/authorize";
        const std::vector<std::pair<std::string_view, std::string_view>>
            parameters{
                {"client_id", options_.application_id},
                {"response_type", "code"},
                {"redirect_uri", redirect_uri},
                {"response_mode", "query"},
                {"scope", options_.scope},
                {"state", payload.csrf.get()},
                {"code_challenge", challenge},
                {"code_challenge_method", "S256"},
            };
        url += "?" + util::encode_uri_parameters(parameters);
        payload.deadline = now_() + timeout_;
        implementation_->transaction =
            util::transition_transaction<detail::AwaitingCallback>(
                detail::CreatedCode{}, [&](const detail::EmptyCodePayload&) {
                    return std::move(payload);
                }
            );
        return url;
    } catch (...) {
        fail_exception();
        throw;
    }
}

AuthResult<void> AuthCodeSession::check(const std::stop_token& stop_token) {
    if (const auto* error = implementation_->error())
        return std::unexpected(*error);
    if (state() != AuthCodeState::awaiting_callback &&
        state() != AuthCodeState::authorized) {
        throw std::logic_error{"browser authorization session is not waiting"};
    }
    if (stop_token.stop_requested()) {
        return std::unexpected(terminate(
            AuthCodeState::cancelled,
            {.code = AuthErrorCode::cancelled,
             .message = "browser login cancelled"}
        ));
    }
    const auto deadline = std::visit(
        [](const auto& current) {
            if constexpr (requires { current.deadline; })
                return current.deadline;
            else
                return std::chrono::steady_clock::time_point{};
        },
        implementation_->transaction
    );
    std::chrono::steady_clock::time_point current_time;
    try {
        current_time = now_();
    } catch (...) {
        fail_exception();
        throw;
    }
    if (current_time >= deadline) {
        return std::unexpected(terminate(
            AuthCodeState::expired,
            {.code = AuthErrorCode::expired,
             .message = "browser login timed out"}
        ));
    }
    return {};
}

AuthResult<void> AuthCodeSession::accept_callback(
    const std::string& callback_uri, const std::stop_token& stop_token
) {
    if (const auto ready = check(stop_token); !ready)
        return ready;
    if (state() != AuthCodeState::awaiting_callback) {
        throw std::logic_error{
            "browser authorization callback was already consumed"
        };
    }
    if (callback_uri.size() > 8192)
        return std::unexpected(invalid_callback());
    try {
        const auto callback = parse_url(callback_uri);
        auto& current =
            std::get<detail::AwaitingCode>(implementation_->transaction);
        const auto redirect = parse_url(current.redirect);
        if (!redirect) {
            throw std::logic_error{
                "invalid stored browser authorization redirect"
            };
        }
        if (!callback || callback->scheme != redirect->scheme ||
            callback->host != redirect->host ||
            callback->port != redirect->port ||
            callback->path != redirect->path) {
            return std::unexpected(invalid_callback());
        }
        detail::CodeSecret state, code;
        std::string error;
        unsigned int states = 0, codes = 0, errors = 0;
        std::string_view query{callback->query};
        while (!query.empty()) {
            const auto end = query.find('&');
            const auto item = query.substr(0, end);
            const auto equal = item.find('=');
            const auto key = decode(item.substr(0, equal));
            auto value = decode(
                equal == std::string_view::npos ? std::string_view{}
                                                : item.substr(equal + 1)
            );
            if (!key || !value)
                return std::unexpected(invalid_callback());
            const detail::CleanCodeString cleanup{*value};
            if (*key == "state") {
                ++states;
                state.assign(*value);
            }
            if (*key == "code") {
                ++codes;
                code.assign(*value);
            }
            if (*key == "error") {
                ++errors;
                error = *value;
            }
            if (end == std::string_view::npos)
                break;
            query.remove_prefix(end + 1);
        }
        if (states != 1 || state.get().size() != current.csrf.get().size() ||
            CRYPTO_memcmp(
                state.get().data(),
                current.csrf.get().data(),
                state.get().size()
            ) != 0 ||
            !((codes == 1 && !code.get().empty() && errors == 0) ||
              (errors == 1 && !error.empty() && codes == 0))) {
            return std::unexpected(invalid_callback());
        }
        if (errors == 1) {
            const bool declined = error == "access_denied";
            return std::unexpected(terminate(
                AuthCodeState::failed,
                {.code = declined ? AuthErrorCode::authorization_declined
                                  : AuthErrorCode::server,
                 .message = declined
                                ? "browser authorization was declined"
                                : "authorization server rejected browser login"}
            ));
        }
        implementation_->transaction =
            util::transition_transaction<detail::Authorized>(
                std::move(current), [&](detail::CallbackPayload payload) {
                    return detail::AuthorizedPayload{
                        std::move(payload.redirect),
                        payload.deadline,
                        std::move(payload.verifier),
                        std::move(code)
                    };
                }
            );
        return {};
    } catch (...) {
        fail_exception();
        throw;
    }
}

AuthResult<OAuthTokens> AuthCodeSession::exchange(
    const DeviceAuthClient& client, const std::stop_token& stop_token
) {
    if (const auto ready = check(stop_token); !ready)
        return std::unexpected(ready.error());
    if (state() != AuthCodeState::authorized) {
        throw std::logic_error{"browser authorization has not received a code"};
    }
    implementation_->transaction =
        util::transition_transaction<detail::ExchangingToken>(
            std::move(
                std::get<detail::AuthorizedCode>(implementation_->transaction)
            ),
            [](detail::AuthorizedPayload payload) {
                return detail::ExchangePayload{
                    std::move(payload.redirect),
                    std::move(payload.verifier),
                    std::move(payload.code)
                };
            }
        );
    try {
        const auto& current =
            std::get<detail::ExchangingCode>(implementation_->transaction);
        auto result = client.exchange_authorization_code(
            current.code.get(),
            current.redirect,
            current.verifier.get(),
            stop_token
        );
        if (!result) {
            return std::unexpected(terminate(
                result.error().code == AuthErrorCode::cancelled
                    ? AuthCodeState::cancelled
                    : AuthCodeState::failed,
                result.error()
            ));
        }
        implementation_
            ->transaction = util::transition_transaction<detail::Completed>(
            std::move(
                std::get<detail::ExchangingCode>(implementation_->transaction)
            ),
            [](const detail::ExchangePayload&) {
                return detail::EmptyCodePayload{};
            }
        );
        return result;
    } catch (...) {
        fail_exception();
        throw;
    }
}

} // namespace onedrive::auth
