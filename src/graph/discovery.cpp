#include "onedrive/graph/graph_client.hpp"
#include "graph/support.hpp"

#include "util/uri.hpp"

#include <stdexcept>
#include <string_view>

namespace onedrive::graph {
namespace {

using client_detail::Json;

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
    if (query.empty()) {
        throw std::invalid_argument(
            "SharePoint site discovery requires a non-empty search query"
        );
    }
    std::vector<SiteInfo> sites;
    for (const auto& value : client_detail::paged_graph_values(
             options_.endpoint + "/sites?search=" +
                 onedrive::util::percent_encode_uri_component(query) +
                 "&$select=id,name,displayName,webUrl",
             options_.endpoint,
             "SharePoint sites",
             [&](const std::string& url) {
                 return client_detail::parse_graph_json(
                     graph_get(url, "SharePoint sites query"),
                     "SharePoint sites"
                 );
             }
         )) {
        auto site = parse_site(value);
        for (const auto& drive : client_detail::paged_graph_values(
                 options_.endpoint + "/sites/" +
                     onedrive::util::percent_encode_uri_component(site.id) +
                     "/drives?$select=id,name,driveType,webUrl,owner,quota",
                 options_.endpoint,
                 "SharePoint document libraries",
                 [&](const std::string& url) {
                     return client_detail::parse_graph_json(
                         graph_get(url, "SharePoint document libraries query"),
                         "SharePoint document libraries"
                     );
                 }
             )) {
            site.drives.push_back(
                client_detail::parse_drive_info(
                    drive, "SharePoint document library"
                )
            );
        }
        sites.push_back(std::move(site));
    }
    return sites;
}

} // namespace onedrive::graph
