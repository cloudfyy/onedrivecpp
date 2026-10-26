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

void emit_site_drive(
    const cli::Console& console,
    const graph::SiteInfo& site,
    const graph::DriveInfo& drive
) {
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
            {
                .label = "site URL:",
                .key = "site_web_url",
                .value = site.web_url,
            },
        }
    );
    console.section("site_drive", "SharePoint document library:", fields);
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
    std::size_t library_count = 0;
    for (const auto& site : outcome.sites) {
        library_count += site.drives.size();
    }
    if (library_count == 0 || outcome.used_wildcard_fallback) {
        console.message(
            cli::MessageKind::information,
            "no_sharepoint_sites",
            "No accessible SharePoint sites matched the query."
        );
        if (library_count == 0) {
            return exit_code::success;
        }
    }
    for (const auto& site : outcome.sites) {
        for (const auto& drive : site.drives) {
            emit_site_drive(console, site, drive);
        }
    }
    return exit_code::success;
}

} // namespace onedrive::app::detail
