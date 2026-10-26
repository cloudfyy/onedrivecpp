#include "discover.hpp"
#include "drive_fields.hpp"

#include "onedrive/graph/graph_client.hpp"
#include "onedrive/exit_code.hpp"

#include <string>

namespace onedrive::app::detail {
namespace {

std::string source_name(graph::SharedResourceSource source) {
    return source == graph::SharedResourceSource::shared_with_me
               ? "shared_with_me"
               : "shortcut";
}

std::vector<cli::Field> site_fields(const graph::SiteInfo& site) {
    return {
        {.label = "name:", .key = "name", .value = site.display_name},
        {.label = "site id:", .key = "site_id", .value = site.id},
        {.label = "web URL:", .key = "web_url", .value = site.web_url},
    };
}

} // namespace

int show_shared(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console
) {
    const auto resources =
        runtime_factory.create_graph_info_client(config)->list_shared_resources(
        );
    if (resources.empty()) {
        console.message(
            cli::MessageKind::information,
            "no_shared_resources",
            "No shared resources or OneDrive shortcuts were found."
        );
        return exit_code::success;
    }
    for (const auto& resource : resources) {
        console.section(
            "shared_resource",
            "Shared OneDrive resource:",
            {
                {
                    .label = "source:",
                    .key = "source",
                    .value = source_name(resource.source),
                },
                {.label = "name:", .key = "name", .value = resource.name},
                {
                    .label = "target name:",
                    .key = "target_name",
                    .value = resource.target_name,
                },
                {
                    .label = "kind:",
                    .key = "kind",
                    .value = resource.directory ? "folder" : "file",
                },
                {
                    .label = "drive id:",
                    .key = "drive_id",
                    .value = resource.drive_id,
                },
                {
                    .label = "item id:",
                    .key = "item_id",
                    .value = resource.item_id,
                },
                {
                    .label = "local path:",
                    .key = "local_path",
                    .value = resource.local_path,
                },
                {.label = "owner:", .key = "owner", .value = resource.owner},
                {
                    .label = "web URL:",
                    .key = "web_url",
                    .value = resource.web_url,
                },
            }
        );
    }
    return exit_code::success;
}

int show_sites(
    const config::Config& config,
    const RuntimeFactory& runtime_factory,
    const cli::Console& console,
    const std::string& query
) {
    const auto client = runtime_factory.create_graph_info_client(config);
    const auto outcome = graph::search_sites_with_fallback(
        query,
        [&](const std::string& target) { return client->search_sites(target); }
    );
    if (outcome.sites.empty() || outcome.used_wildcard_fallback) {
        console.message(
            cli::MessageKind::information,
            "no_sharepoint_sites",
            "No accessible SharePoint sites matched the query."
        );
        for (const auto& site : outcome.sites) {
            console.section(
                "available_site",
                "Available SharePoint site:",
                site_fields(site)
            );
        }
        return exit_code::success;
    }
    for (const auto& site : outcome.sites) {
        console.section("site", "SharePoint site:", site_fields(site));
        for (const auto& drive : site.drives) {
            auto fields = drive_fields(drive);
            fields.insert(
                fields.begin(),
                {
                    {
                        .label = "site:",
                        .key = "site",
                        .value = site.display_name,
                    },
                    {
                        .label = "site id:",
                        .key = "site_id",
                        .value = site.id,
                    },
                }
            );
            console.section(
                "site_drive",
                "SharePoint document library:",
                fields
            );
        }
    }
    return exit_code::success;
}

} // namespace onedrive::app::detail
