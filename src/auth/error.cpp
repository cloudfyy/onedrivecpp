#include "auth/error.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>

namespace onedrive::auth {
namespace {

bool has_code(
    const AuthError& error, std::initializer_list<std::uint64_t> codes
) {
    return std::ranges::any_of(codes, [&](const auto code) {
        return std::ranges::find(error.server_error_codes, code) !=
               error.server_error_codes.end();
    });
}

std::string_view guidance(const AuthError& error) {
    if (error.code != AuthErrorCode::server) {
        return {};
    }
    if (error.oauth_error == "temporarily_unavailable" ||
        error.oauth_error == "server_error") {
        return "Microsoft authentication is temporarily unavailable. Try again "
               "later; changing permissions or deleting saved credentials is "
               "not required.";
    }
    // STS codes refine advice only; OAuth errors still control the protocol.
    if (has_code(error, {530036})) {
        return "Conditional Access restricts the authentication flow used by "
               "this grant (device code or authentication transfer). Stop "
               "monitoring, log out locally and use GUI browser sign-in only "
               "if your organization permits it. Ask the tenant administrator "
               "to review the authentication flows policy; browser sign-in "
               "does not bypass device or access requirements.";
    }
    if (has_code(error, {90094})) {
        return "Administrator consent is required. Ask your tenant "
               "administrator to approve this application and its configured "
               "delegated permissions. Do not add broader permissions or a "
               "client secret to bypass consent.";
    }
    if (has_code(error, {50076, 50079, 50097, 53000, 53001, 53002, 53003})) {
        return "Microsoft requires additional authentication or a compliant "
               "or approved device. Complete the required sign-in checks and "
               "ask your tenant administrator to review Conditional Access. "
               "Browser sign-in does not bypass these policies; automatic "
               "claims-challenge or device-broker authentication is not "
               "implemented.";
    }
    if (has_code(error, {65001}) || error.oauth_error == "consent_required") {
        return "Consent is required for the configured permissions. Sign in "
               "again; if your organization restricts user consent, ask its "
               "administrator to approve the application. Broader permissions "
               "are not required merely to use your own organizational "
               "OneDrive.";
    }
    if (has_code(error, {65004})) {
        return "Consent was declined. Review the requested permissions and "
               "sign in again if you intend to authorize this application.";
    }
    if (error.oauth_error == "interaction_required" ||
        error.oauth_error == "login_required") {
        return "Interactive sign-in is required. Stop monitoring and sign in "
               "again, completing your organization's browser sign-in checks. "
               "In the GUI, log out locally first. Automatic claims-challenge "
               "or device-broker authentication is not implemented.";
    }
    if (has_code(error, {50173, 70008, 700082, 700084, 70043}) ||
        error.oauth_error == "invalid_grant") {
        return "The authorization grant is invalid, expired or revoked. Stop "
               "monitoring and sign in again with 'onedrive-cpp account "
               "login', "
               "or log out locally before signing in from the GUI. This "
               "failure does not automatically remove saved credentials.";
    }
    if (error.oauth_error == "unauthorized_client" ||
        error.oauth_error == "unsupported_grant_type") {
        return "This public client is not permitted to use the requested "
               "authentication flow. Check the configured tenant, supported "
               "account types and Allow public client flows in the application "
               "registration. If device code flow is restricted, ask your "
               "administrator whether GUI browser sign-in is permitted. Do "
               "not add a client secret.";
    }
    if (error.oauth_error == "access_denied") {
        return "Microsoft denied authorization. Review the requested "
               "permissions and your organization's consent or access "
               "policies; this error alone does not identify the policy.";
    }
    return {};
}

} // namespace

AuthError detail::server_error(
    const nlohmann::json& response,
    std::string_view fallback,
    std::string_view configured_tenant
) {
    AuthError result{
        .code = AuthErrorCode::server,
        .message = std::string{fallback},
        .configured_tenant = std::string{configured_tenant},
    };
    const auto invalid = [&](std::string_view field) {
        return AuthError{
            .code = AuthErrorCode::invalid_response,
            .message = "Microsoft authentication returned an invalid error "
                       "response field: " +
                       std::string{field},
            .configured_tenant = std::string{configured_tenant},
        };
    };
    if (!response.is_object()) {
        return invalid("object");
    }
    if (const auto value = response.find("error"); value != response.end()) {
        if (!value->is_string()) {
            return invalid("error");
        }
        result.oauth_error = value->get<std::string>();
    }
    if (const auto value = response.find("error_description");
        value != response.end()) {
        if (!value->is_string()) {
            return invalid("error_description");
        }
        const auto description = value->get<std::string>();
        if (!description.empty()) {
            result.message = description;
        }
    }
    if (const auto values = response.find("error_codes");
        values != response.end()) {
        if (!values->is_array()) {
            return invalid("error_codes");
        }
        for (const auto& value : *values) {
            if (!value.is_number_integer() ||
                (!value.is_number_unsigned() &&
                 value.get<std::int64_t>() <= 0) ||
                (value.is_number_unsigned() &&
                 value.get<std::uint64_t>() == 0)) {
                return invalid("error_codes");
            }
            result.server_error_codes.push_back(value.get<std::uint64_t>());
        }
    }
    return result;
}

std::string describe_error(const AuthError& error) {
    auto message = error.message;
    if (!error.oauth_error.empty()) {
        message += "\nOAuth error: " + error.oauth_error;
    }
    if (!error.server_error_codes.empty()) {
        message += "\nMicrosoft diagnostic codes:";
        for (const auto code : error.server_error_codes) {
            message += " AADSTS" + std::to_string(code);
        }
    }
    if (!error.configured_tenant.empty()) {
        message += "\nConfigured tenant/selector (not a verified tenant ID): " +
                   error.configured_tenant;
    }
    if (const auto hint = guidance(error); !hint.empty()) {
        message += '\n';
        message += hint;
    }
    return message;
}

} // namespace onedrive::auth
