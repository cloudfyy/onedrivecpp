#include "onedrive/graph/graph_client.hpp"
#include "graph/support.hpp"

#include "util/ascii.hpp"
#include "util/uri.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace onedrive::graph {
namespace {

using client_detail::Json;

bool valid_sharepoint_hostname(std::string_view host) noexcept {
    if (host.empty() || !host.contains('.') || host.starts_with('.') ||
        host.ends_with('.') || host.starts_with('-') || host.ends_with('-') ||
        host.contains("..")) {
        return false;
    }
    return std::ranges::all_of(host, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '.' || character == '-';
    });
}

bool valid_site_id_token(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    return std::ranges::all_of(token, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '-' || character == '_';
    });
}

bool valid_sharepoint_site_id(std::string_view value) noexcept {
    const auto first_comma = value.find(',');
    if (first_comma == std::string_view::npos) {
        return false;
    }
    const auto second_comma = value.find(',', first_comma + 1);
    if (second_comma == std::string_view::npos ||
        value.find(',', second_comma + 1) != std::string_view::npos) {
        return false;
    }
    return valid_sharepoint_hostname(value.substr(0, first_comma)) &&
           valid_site_id_token(
               value.substr(first_comma + 1, second_comma - first_comma - 1)
           ) &&
           valid_site_id_token(value.substr(second_comma + 1));
}

bool contains_unsafe_site_target_characters(std::string_view value) noexcept {
    return std::ranges::any_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x20U || byte == 0x7FU || character == '?' ||
               character == '#' || character == '\\' || character == '@';
    });
}

std::string
build_host_site_path(std::string_view host, std::string_view relative_path) {
    if (!valid_sharepoint_hostname(host)) {
        throw std::invalid_argument(
            "SharePoint site target has an invalid hostname"
        );
    }
    while (relative_path.starts_with('/')) {
        relative_path.remove_prefix(1);
    }
    while (relative_path.ends_with('/')) {
        relative_path.remove_suffix(1);
    }
    const auto encoded_host =
        onedrive::util::percent_encode_uri_component(host);
    if (relative_path.empty()) {
        return "/sites/" + encoded_host;
    }
    return "/sites/" + encoded_host + ":/" +
           client_detail::percent_encode_remote_path(relative_path);
}

struct SiteQueryPlan {
    enum class Mode {
        direct_resource,
        keyword_search,
    };

    Mode mode{Mode::keyword_search};
    std::string target;
};

using SiteQueryStrategy =
    std::optional<SiteQueryPlan> (*)(std::string_view query);

std::optional<SiteQueryPlan> resolve_https_site_url(std::string_view query) {
    if (!query.contains("://")) {
        return std::nullopt;
    }
    constexpr std::string_view https_scheme{"https://"};
    if (!query.starts_with(https_scheme)) {
        throw std::invalid_argument("SharePoint site URL must use https://");
    }
    if (contains_unsafe_site_target_characters(query)) {
        throw std::invalid_argument(
            "SharePoint site URL contains unsupported characters"
        );
    }
    const auto rest = query.substr(https_scheme.size());
    const auto slash = rest.find('/');
    const auto host = rest.substr(0, slash);
    const auto path = slash == std::string_view::npos ? std::string_view{}
                                                      : rest.substr(slash + 1);
    return SiteQueryPlan{
        .mode = SiteQueryPlan::Mode::direct_resource,
        .target = build_host_site_path(host, path),
    };
}

std::optional<SiteQueryPlan>
resolve_host_relative_site_path(std::string_view query) {
    if (!query.contains(":/")) {
        return std::nullopt;
    }
    if (contains_unsafe_site_target_characters(query)) {
        throw std::invalid_argument(
            "SharePoint site path contains unsupported characters"
        );
    }
    const auto separator = query.find(":/");
    return SiteQueryPlan{
        .mode = SiteQueryPlan::Mode::direct_resource,
        .target = build_host_site_path(
            query.substr(0, separator), query.substr(separator + 2)
        ),
    };
}

std::optional<SiteQueryPlan>
resolve_direct_site_identifier(std::string_view query) {
    if (query != "root" && !valid_sharepoint_site_id(query)) {
        return std::nullopt;
    }
    return SiteQueryPlan{
        .mode = SiteQueryPlan::Mode::direct_resource,
        .target =
            "/sites/" + onedrive::util::percent_encode_uri_component(query),
    };
}

std::optional<SiteQueryPlan>
resolve_keyword_site_search(std::string_view query) {
    return SiteQueryPlan{
        .mode = SiteQueryPlan::Mode::keyword_search,
        .target = "/sites?search=" +
                  onedrive::util::percent_encode_uri_component(query) +
                  "&$select=id,name,displayName,webUrl",
    };
}

SiteQueryPlan resolve_site_query_plan(std::string_view raw_query) {
    const auto trimmed = onedrive::util::trim_ascii_whitespace(raw_query);
    if (trimmed.empty()) {
        throw std::invalid_argument(
            "SharePoint site discovery requires a non-empty search query"
        );
    }
    constexpr std::array<SiteQueryStrategy, 4> strategies{
        &resolve_https_site_url,
        &resolve_host_relative_site_path,
        &resolve_direct_site_identifier,
        &resolve_keyword_site_search,
    };
    for (const auto strategy : strategies) {
        if (auto plan = strategy(trimmed)) {
            return *std::move(plan);
        }
    }
    throw std::logic_error("unreachable SharePoint site query strategy");
}

std::string identity_name(const Json& value) {
    for (const auto* relationship : {"sharedBy", "owner", "createdBy"}) {
        const auto container = value.find(relationship);
        if (container == value.end() || !container->is_object()) {
            continue;
        }
        for (const auto* kind : {"user", "group", "site"}) {
            const auto identity = container->find(kind);
            if (identity != container->end() && identity->is_object()) {
                const auto name = identity->value("displayName", std::string{});
                if (!name.empty()) {
                    return name;
                }
            }
        }
    }
    return {};
}

SharedResource
parse_shared_resource(const Json& value, SharedResourceSource source) {
    try {
        const auto* remote = client_detail::remote_item_facet(value);
        if (remote == nullptr) {
            throw std::runtime_error(
                "Microsoft Graph returned a shared resource without a "
                "remoteItem facet"
            );
        }
        const auto& parent = remote->at("parentReference");
        SharedResource resource{
            .name = value.at("name").get<std::string>(),
            .target_name = remote->value("name", std::string{}),
            .drive_id = parent.at("driveId").get<std::string>(),
            .item_id = remote->at("id").get<std::string>(),
            .web_url =
                remote->value("webUrl", value.value("webUrl", std::string{})),
            .owner = {},
            .local_path = {},
            .directory = remote->contains("folder"),
            .source = source,
        };
        if (const auto shared = remote->find("shared");
            shared != remote->end() && shared->is_object()) {
            resource.owner = identity_name(*shared);
        }
        if (resource.owner.empty()) {
            resource.owner = identity_name(*remote);
        }
        if (resource.target_name.empty()) {
            resource.target_name = resource.name;
        }
        if (source == SharedResourceSource::shortcut) {
            resource.local_path =
                client_detail::item_remote_path(value, resource.name);
        }
        if (resource.name.empty() || resource.drive_id.empty() ||
            resource.item_id.empty()) {
            throw std::runtime_error(
                "Microsoft Graph returned incomplete shared resource metadata"
            );
        }
        return resource;
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph shared resource is missing required data: " +
            std::string{error.what()}
        );
    }
}

SiteInfo parse_site(const Json& value) {
    try {
        SiteInfo site{
            .id = value.at("id").get<std::string>(),
            .name = value.value("name", std::string{}),
            .display_name = value.value("displayName", std::string{}),
            .web_url = value.value("webUrl", std::string{}),
            .drives = {},
        };
        if (site.display_name.empty()) {
            site.display_name = site.name;
        }
        if (site.id.empty() || site.display_name.empty()) {
            throw std::runtime_error(
                "Microsoft Graph returned incomplete SharePoint site metadata"
            );
        }
        return site;
    } catch (const Json::exception& error) {
        throw std::runtime_error(
            "Microsoft Graph SharePoint site is missing required data: " +
            std::string{error.what()}
        );
    }
}

} // namespace

std::vector<SharedResource>
MicrosoftGraphClient::list_shared_resources() const {
    const auto fetch_page = [&](const std::string& url, std::string_view name) {
        return client_detail::parse_graph_json(
            graph_get(url, std::string{name} + " query"), name
        );
    };
    std::vector<SharedResource> resources;
    for (const auto& value : client_detail::paged_graph_values(
             options_.endpoint +
                 "/me/drive/sharedWithMe?$select=id,name,webUrl,remoteItem",
             options_.endpoint,
             "shared resources",
             [&](const std::string& url) {
                 return fetch_page(url, "shared resources");
             }
         )) {
        resources.push_back(
            parse_shared_resource(value, SharedResourceSource::shared_with_me)
        );
    }
    for (const auto& value : client_detail::paged_graph_values(
             client_detail::graph_drive_prefix(options_) +
                 "/root/delta?$select=id,name,webUrl,parentReference,"
                 "remoteItem,deleted",
             options_.endpoint,
             "shortcuts",
             [&](const std::string& url) {
                 return fetch_page(url, "shortcuts");
             }
         )) {
        if (!value.contains("deleted") &&
            client_detail::remote_item_facet(value) != nullptr) {
            resources.push_back(
                parse_shared_resource(value, SharedResourceSource::shortcut)
            );
        }
    }
    return resources;
}

std::vector<SiteInfo>
MicrosoftGraphClient::search_sites(const std::string& query) const {
    const auto plan = resolve_site_query_plan(query);
    const auto perform_site_get = [&](const std::string& url,
                                      std::string_view description,
                                      bool allow_not_found =
                                          false) -> std::optional<Json> {
        const auto response = graph_get_response(url, description);
        if (allow_not_found &&
            response.status_code == http::status::not_found) {
            return std::nullopt;
        }
        auto document = client_detail::parse_graph_json(response, description);
        if (!client_detail::successful_status(response.status_code)) {
            if (response.status_code == http::status::forbidden) {
                throw std::runtime_error(
                    client_detail::graph_error_message(
                        document, response.status_code
                    ) +
                    " (SharePoint discovery requires site permissions such as "
                    "Sites.Read.All, Sites.ReadWrite.All, or "
                    "Files.ReadWrite.All in auth.scopes, or Sites.Selected "
                    "with a direct site URL/ID; sign in again after updating "
                    "auth.scopes)"
                );
            }
            client_detail::require_successful_graph_response(
                document, response.status_code
            );
        }
        return std::optional<Json>{std::in_place, std::move(document)};
    };
    const auto populate_site_drives = [&](SiteInfo& site) {
        for (const auto& drive : client_detail::paged_graph_values(
                 options_.endpoint + "/sites/" +
                     onedrive::util::percent_encode_uri_component(site.id) +
                     "/drives?$select=id,name,driveType,webUrl,owner,quota",
                 options_.endpoint,
                 "SharePoint document libraries",
                 [&](const std::string& url) {
                     return *perform_site_get(
                         url, "SharePoint document libraries"
                     );
                 }
             )) {
            site.drives.push_back(
                client_detail::parse_drive_info(
                    drive, "SharePoint document library"
                )
            );
        }
    };

    std::vector<SiteInfo> sites;
    if (plan.mode == SiteQueryPlan::Mode::direct_resource) {
        const auto document = perform_site_get(
            options_.endpoint + plan.target +
                "?$select=id,name,displayName,webUrl",
            "SharePoint site",
            true
        );
        if (!document) {
            return sites;
        }
        auto site = parse_site(*document);
        populate_site_drives(site);
        sites.push_back(std::move(site));
        return sites;
    }

    for (const auto& value : client_detail::paged_graph_values(
             options_.endpoint + plan.target,
             options_.endpoint,
             "SharePoint sites",
             [&](const std::string& url) {
                 return *perform_site_get(url, "SharePoint sites");
             }
         )) {
        auto site = parse_site(value);
        populate_site_drives(site);
        sites.push_back(std::move(site));
    }
    return sites;
}

} // namespace onedrive::graph
