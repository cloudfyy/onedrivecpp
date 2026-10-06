#include "config/parse.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace onedrive::config::detail {

void load_proxy_options(Config& config, const toml::table& root, const std::filesystem::path& config_path) {
    if (const auto* proxy = optional_table(root, "proxy", "proxy")) {
        validate_keys(
            *proxy,
            {
                "url",
                "no_proxy",
                "username",
                "password_file",
                "auth",
                "ca_file",
            },
            "proxy"
        );
        const auto url = optional_value<std::string>(
            *proxy,
            "url",
            "proxy.url",
            "a string"
        );
        if (!url) {
            throw std::runtime_error(
                "TOML configuration value 'proxy.url' is required"
            );
        }
        validate_proxy_url(*url);
        config.proxy.url = *url;
        if (proxy->contains("no_proxy")) {
            config.proxy.no_proxy = proxy_bypass_list(*proxy);
        }
        if (const auto username = optional_value<std::string>(
                *proxy,
                "username",
                "proxy.username",
                "a string"
            )) {
            if (username->empty() || username->contains('\0')) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.username' must not "
                    "be empty or contain NUL bytes"
                );
            }
            config.proxy.username = *username;
        }
        if (const auto password_file = optional_value<std::string>(
                *proxy,
                "password_file",
                "proxy.password_file",
                "a string"
            )) {
            if (password_file->empty() ||
                password_file->contains('\0')) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.password_file' must "
                    "not be empty or contain NUL bytes"
                );
            }
            auto resolved = std::filesystem::path{*password_file};
            if (resolved.is_relative()) {
                resolved = config_path.parent_path() / resolved;
            }
            config.proxy.password_file = resolved.lexically_normal();
        }
        if (const auto auth = optional_value<std::string>(
                *proxy,
                "auth",
                "proxy.auth",
                "a string"
            )) {
            config.proxy.auth = parse_proxy_auth(*auth);
        }
        if (const auto ca_file = optional_value<std::string>(
                *proxy,
                "ca_file",
                "proxy.ca_file",
                "a string"
            )) {
            if (ca_file->empty() || ca_file->contains('\0')) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.ca_file' must not "
                    "be empty or contain NUL bytes"
                );
            }
            if (!url->starts_with("https://")) {
                throw std::runtime_error(
                    "TOML configuration value 'proxy.ca_file' requires "
                    "an HTTPS proxy URL"
                );
            }
            auto resolved = std::filesystem::path{*ca_file};
            if (resolved.is_relative()) {
                resolved = config_path.parent_path() / resolved;
            }
            config.proxy.ca_file = resolved.lexically_normal();
        }
        if (config.proxy.password_file && !config.proxy.username) {
            throw std::runtime_error(
                "TOML configuration value 'proxy.password_file' requires "
                "'proxy.username'"
            );
        }
    }

}

void load_transfer_options(Config& config, const toml::table& root) {
    if (const auto* transfer =
            optional_table(root, "transfer", "transfer")) {
        auto& options = config.transfer_transport;
        validate_keys(
            *transfer,
            {
                "connect_timeout_seconds",
                "operation_timeout_seconds",
                "stall_timeout_seconds",
                "stall_minimum_bytes_per_second",
                "http_version",
                "ip_version",
                "order",
            },
            "transfer"
        );
        if (const auto value = optional_value<std::string>(
                *transfer,
                "order",
                "transfer.order",
                "a string"
            )) {
            config.transfer_order = parse_transfer_order(*value);
        }
        if (transfer->contains("connect_timeout_seconds")) {
            options.connect_timeout = duration_value<std::chrono::seconds>(
                *transfer,
                "connect_timeout_seconds",
                "transfer.connect_timeout_seconds"
            );
            if (options.connect_timeout == std::chrono::seconds::zero()) {
                throw std::runtime_error(
                    "transfer.connect_timeout_seconds must be greater than 0"
                );
            }
        }
        if (transfer->contains("operation_timeout_seconds")) {
            options.operation_timeout = duration_value<std::chrono::seconds>(
                *transfer,
                "operation_timeout_seconds",
                "transfer.operation_timeout_seconds"
            );
            if (options.operation_timeout == std::chrono::seconds::zero()) {
                throw std::runtime_error(
                    "transfer.operation_timeout_seconds must be greater than 0"
                );
            }
        }
        if (transfer->contains("stall_timeout_seconds")) {
            options.low_speed_timeout = duration_value<std::chrono::seconds>(
                *transfer,
                "stall_timeout_seconds",
                "transfer.stall_timeout_seconds"
            );
        }
        if (transfer->contains("stall_minimum_bytes_per_second")) {
            options.low_speed_limit_bytes_per_second = unsigned_value(
                *transfer,
                "stall_minimum_bytes_per_second",
                "transfer.stall_minimum_bytes_per_second"
            );
            if (options.low_speed_limit_bytes_per_second == 0) {
                throw std::runtime_error(
                    "transfer.stall_minimum_bytes_per_second must be greater "
                    "than 0"
                );
            }
        }
        if (const auto value = optional_value<std::string>(
                *transfer,
                "http_version",
                "transfer.http_version",
                "a string"
            )) {
            options.http_version = parse_http_version(*value);
        }
        if (const auto value = optional_value<std::string>(
                *transfer,
                "ip_version",
                "transfer.ip_version",
                "a string"
            )) {
            options.ip_version = parse_ip_version(*value);
        }
    }

}

void load_auth_options(Config& config, const toml::table& root) {
    if (const auto* auth = optional_table(root, "auth", "auth")) {
        validate_keys(
            *auth,
            {"application_id", "tenant_id", "endpoint", "scopes"},
            "auth"
        );
        if (const auto value = optional_value<std::string>(
                *auth,
                "application_id",
                "auth.application_id",
                "a string"
            )) {
            config.application_id = *value;
        }
        if (const auto value = optional_value<std::string>(
                *auth,
                "tenant_id",
                "auth.tenant_id",
                "a string"
            )) {
            config.azure_tenant_id = *value;
        }
        if (const auto value = optional_value<std::string>(
                *auth,
                "endpoint",
                "auth.endpoint",
                "a string"
            )) {
            config.auth_endpoint = *value;
        }
        if (auth->contains("scopes")) {
            config.auth_scope =
                string_array_value(*auth, "scopes", "auth.scopes");
        }
    }

}

void load_graph_options(Config& config, const toml::table& root) {
    if (const auto* graph = optional_table(root, "graph", "graph")) {
        validate_keys(*graph, {"endpoint", "throttle"}, "graph");
        if (const auto value = optional_value<std::string>(
                *graph,
                "endpoint",
                "graph.endpoint",
                "a string"
            )) {
            config.graph_endpoint = *value;
        }
        if (const auto* throttle =
                optional_table(*graph, "throttle", "graph.throttle")) {
            validate_keys(
                *throttle,
                {
                    "maximum_retries",
                    "initial_delay_seconds",
                    "maximum_delay_seconds",
                },
                "graph.throttle"
            );
            if (throttle->contains("maximum_retries")) {
                config.graph_maximum_throttle_retries = size_value(
                    *throttle,
                    "maximum_retries",
                    "graph.throttle.maximum_retries"
                );
            }
            if (throttle->contains("initial_delay_seconds")) {
                config.graph_initial_throttle_delay =
                    duration_value<std::chrono::seconds>(
                        *throttle,
                        "initial_delay_seconds",
                        "graph.throttle.initial_delay_seconds"
                    );
            }
            if (throttle->contains("maximum_delay_seconds")) {
                config.graph_maximum_throttle_delay =
                    duration_value<std::chrono::seconds>(
                        *throttle,
                        "maximum_delay_seconds",
                        "graph.throttle.maximum_delay_seconds"
                    );
            }
        }
    }

}

}  // namespace onedrive::config::detail
