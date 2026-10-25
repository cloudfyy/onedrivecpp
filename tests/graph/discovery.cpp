#include "support/progress.hpp"
#include "support.hpp"

#include <stop_token>

namespace {

using namespace onedrive::test::graph;

std::unique_ptr<onedrive::graph::MicrosoftGraphClient>
make_client(std::deque<onedrive::http::HttpResult> responses) {
    responses.push_front(
        token_response()
    );
    return std::make_unique<onedrive::graph::MicrosoftGraphClient>(
        wrap_transport(std::make_unique<FakeTransport>(std::move(responses))),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        onedrive::graph::GraphOptions{
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        }
    );
}

int test_shared_resources_and_shortcuts() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"wrapper-id","name":"Shared Folder","remoteItem":{"id":"target-id","name":"Team Folder","webUrl":"https://example.test/team","parentReference":{"driveId":"team-drive"},"folder":{},"shared":{"sharedBy":{"user":{"displayName":"Alice"}}}}}],"@odata.nextLink":"https://graph.example.test/v1.0/shared-next"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"json({"value":[]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"shortcut-id","name":"Team Shortcut","parentReference":{"path":"/drives/current/root:/Links"},"remoteItem":{"id":"shortcut-target-id","webUrl":"https://example.test/docs","parentReference":{"driveId":"team-drive"},"folder":{},"createdBy":{"group":{"displayName":"Engineering"}}}},{"id":"ordinary-id","name":"ordinary.txt"},{"id":"deleted-id","name":"Deleted Shortcut","deleted":{},"remoteItem":{"id":"deleted-target","name":"Deleted","parentReference":{"driveId":"team-drive"},"folder":{}}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta-token"})json",
        },
    });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "current",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    const auto resources = client.list_shared_resources();
    if (resources.size() != 2 ||
        resources[0].source !=
            onedrive::graph::SharedResourceSource::shared_with_me ||
        resources[0].name != "Shared Folder" ||
        resources[0].target_name != "Team Folder" ||
        resources[0].drive_id != "team-drive" ||
        resources[0].item_id != "target-id" || resources[0].owner != "Alice" ||
        !resources[0].directory ||
        resources[1].source !=
            onedrive::graph::SharedResourceSource::shortcut ||
        resources[1].target_name != "Team Shortcut" ||
        resources[1].local_path != "Links/Team Shortcut" ||
        resources[1].owner != "Engineering" ||
        transport_pointer->queued.requests.size() != 4 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/me/drive/sharedWithMe?"
            "$select=id,name,webUrl,remoteItem" ||
        transport_pointer->queued.requests[2].url !=
            "https://graph.example.test/v1.0/shared-next" ||
        transport_pointer->queued.requests[3].url !=
            "https://graph.example.test/v1.0/drives/current/root/delta?"
            "$select=id,name,webUrl,parentReference,remoteItem,deleted") {
        return fail("shared resources or shortcuts were not discovered");
    }
    return EXIT_SUCCESS;
}

int test_sites_and_document_libraries() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"example.sharepoint.test,site-id,web-id","name":"engineering","displayName":"Engineering & Operations","webUrl":"https://example.sharepoint.test/engineering"}]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"documents-id","name":"Documents","driveType":"documentLibrary","webUrl":"https://example.sharepoint.test/docs","owner":{"group":{"displayName":"Engineering"}}}],"@odata.nextLink":"https://graph.example.test/v1.0/library-next"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"assets-id","name":"Assets","driveType":"documentLibrary"}]})json",
        },
    });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };

    const auto sites = client.search_sites("Engineering & Ops");
    if (sites.size() != 1 ||
        sites[0].display_name != "Engineering & Operations" ||
        sites[0].drives.size() != 2 ||
        sites[0].drives[0].id != "documents-id" ||
        sites[0].drives[0].owner != "Engineering" ||
        sites[0].drives[1].id != "assets-id" ||
        transport_pointer->queued.requests.size() != 4 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/sites?search=Engineering%20%26%20"
            "Ops&$select=id,name,displayName,webUrl" ||
        transport_pointer->queued.requests[2].url !=
            "https://graph.example.test/v1.0/sites/"
            "example.sharepoint.test%2Csite-id%2Cweb-id/drives?"
            "$select=id,name,driveType,webUrl,owner,quota" ||
        transport_pointer->queued.requests[3].url !=
            "https://graph.example.test/v1.0/library-next") {
        return fail("SharePoint sites or document libraries were not parsed");
    }

    auto direct_transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        token_response(),
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"example.sharepoint.test,site-id,web-id","name":"engineering","displayName":"Engineering","webUrl":"https://example.sharepoint.test/sites/engineering"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"documents-id","name":"Documents","driveType":"documentLibrary"}]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"example.sharepoint.test,root-id,web-id","name":"root","displayName":"Root Site","webUrl":"https://example.sharepoint.test"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"json({"value":[]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"example.sharepoint.test,ops-id,web-id","name":"ops","displayName":"Operations","webUrl":"https://example.sharepoint.test/teams/ops"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"json({"value":[]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"example.sharepoint.test,site-id,web-id","name":"engineering","displayName":"Engineering","webUrl":"https://example.sharepoint.test/sites/engineering"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"json({"value":[]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"example.sharepoint.test,root-id,web-id","name":"root","displayName":"Root Site","webUrl":"https://example.sharepoint.test"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"json({"value":[]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 404,
            .body =
                R"json({"error":{"code":"itemNotFound","message":"Site not found"}})json",
        },
    });
    auto* direct_pointer = direct_transport.get();
    onedrive::graph::MicrosoftGraphClient direct_client{
        wrap_transport(std::move(direct_transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    const auto by_url = direct_client.search_sites(
        "  https://example.sharepoint.test/sites/engineering/  "
    );
    const auto by_root_url =
        direct_client.search_sites("https://example.sharepoint.test/");
    const auto by_colon_path =
        direct_client.search_sites("example.sharepoint.test:/teams/ops");
    const auto by_site_id =
        direct_client.search_sites("example.sharepoint.test,site-id,web-id");
    const auto by_root_keyword = direct_client.search_sites("root");
    const auto missing_site = direct_client.search_sites(
        "https://example.sharepoint.test/sites/missing"
    );
    if (by_url.size() != 1 || by_url[0].drives.size() != 1 ||
        by_root_url.size() != 1 || by_colon_path.size() != 1 ||
        by_site_id.size() != 1 || by_root_keyword.size() != 1 ||
        !missing_site.empty() || direct_pointer->queued.requests.size() != 12 ||
        direct_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/sites/example.sharepoint.test:/"
            "sites/engineering?$select=id,name,displayName,webUrl" ||
        direct_pointer->queued.requests[3].url !=
            "https://graph.example.test/v1.0/sites/example.sharepoint.test?"
            "$select=id,name,displayName,webUrl" ||
        direct_pointer->queued.requests[5].url !=
            "https://graph.example.test/v1.0/sites/example.sharepoint.test:/"
            "teams/ops?$select=id,name,displayName,webUrl" ||
        direct_pointer->queued.requests[7].url !=
            "https://graph.example.test/v1.0/sites/"
            "example.sharepoint.test%2Csite-id%2Cweb-id?"
            "$select=id,name,displayName,webUrl" ||
        direct_pointer->queued.requests[9].url !=
            "https://graph.example.test/v1.0/sites/root?"
            "$select=id,name,displayName,webUrl" ||
        direct_pointer->queued.requests[11].url !=
            "https://graph.example.test/v1.0/sites/example.sharepoint.test:/"
            "sites/missing?$select=id,name,displayName,webUrl") {
        return fail("direct SharePoint site resolution failed");
    }

    const auto fallback_hit = onedrive::graph::search_sites_with_fallback(
        "unmatched",
        [&](const std::string& target)
            -> std::vector<onedrive::graph::SiteInfo> {
            if (target == "*") {
                return by_url;
            }
            return {};
        }
    );
    const auto fallback_forbidden = onedrive::graph::search_sites_with_fallback(
        "https://example.sharepoint.test/sites/missing",
        [&](const std::string& target)
            -> std::vector<onedrive::graph::SiteInfo> {
            if (target == "*") {
                throw std::runtime_error{"Access denied on wildcard"};
            }
            return {};
        }
    );
    if (!fallback_hit.used_wildcard_fallback ||
        fallback_hit.effective_query != "unmatched" ||
        fallback_hit.sites.size() != 1 ||
        fallback_forbidden.used_wildcard_fallback ||
        !fallback_forbidden.sites.empty()) {
        return fail(
            "search_sites_with_fallback did not handle wildcard fallback "
            "scenarios"
        );
    }
    return EXIT_SUCCESS;
}

int test_rejects_invalid_discovery_data() {
    for (const auto* invalid_query :
         {"",
          "   ",
          "http://example.sharepoint.test/sites/team",
          "https://invalid_host/sites/team",
          "https://example.sharepoint.test/sites/../secret",
          "https://example.sharepoint.test/sites/team?query=1",
          "invalid_host:/sites/team"}) {
        try {
            static_cast<void>(make_client({})->search_sites(invalid_query));
            return fail("an invalid SharePoint site query was accepted");
        } catch (const std::invalid_argument&) {
        }
    }
    try {
        static_cast<void>(
            make_client(
                {
                    onedrive::http::HttpResponse{
                        .status_code = 403,
                        .body =
                            R"json({"error":{"code":"accessDenied","message":"Access denied"}})json",
                    },
                }
            )
                ->search_sites("*")
        );
        return fail("a forbidden SharePoint site query was accepted");
    } catch (const std::runtime_error& error) {
        const std::string_view message{error.what()};
        if (!message.contains("Access denied") ||
            !message.contains("Sites.Read.All") ||
            !message.contains("Sites.Selected")) {
            return fail(
                "forbidden SharePoint site query did not include permission "
                "guidance"
            );
        }
    }
    try {
        static_cast<void>(
            make_client(
                {
                    onedrive::http::HttpResponse{
                        .status_code = 200,
                        .body =
                            R"json({"value":[],"@odata.nextLink":"https://attacker.example/shared"})json",
                    },
                }
            )
                ->list_shared_resources()
        );
        return fail("an external shared-resource page was accepted");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(
            make_client({
                            std::unexpected(
                                onedrive::http::HttpError{
                                    .message =
                                        "simulated discovery transport failure",
                                }
                            ),
                        })
                ->list_shared_resources()
        );
        return fail("a shared-resource transport failure was ignored");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(
            make_client(
                {
                    onedrive::http::HttpResponse{
                        .status_code = 403,
                        .body =
                            R"json({"error":{"code":"accessDenied","message":"denied"}})json",
                    },
                }
            )
                ->list_shared_resources()
        );
        return fail("a shared-resource Graph error was ignored");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(
            make_client(
                {
                    onedrive::http::HttpResponse{
                        .status_code = 200,
                        .body =
                            R"json({"value":[{"id":"wrapper","name":"Broken"}]})json",
                    },
                }
            )
                ->list_shared_resources()
        );
        return fail("a shared resource without remoteItem was accepted");
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(
            make_client(
                {
                    onedrive::http::HttpResponse{
                        .status_code = 200,
                        .body =
                            R"json({"value":[{"name":"broken","displayName":"Broken"}]})json",
                    },
                }
            )
                ->search_sites("Broken")
        );
        return fail("incomplete SharePoint site metadata was accepted");
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}

int test_identity_startup_cancellation() {
    for (const int stage : {0, 1, 2}) {
        std::stop_source stop;
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                std::unexpected(
                    onedrive::http::HttpError{.message = "request interrupted"}
                ),
            }
        );
        auto* requests = transport.get();
        bool forwarded = true;
        int count = 0;
        transport->on_perform =
            [&](const onedrive::http::HttpRequest& request) {
                forwarded = forwarded && request.stop_token == stop.get_token();
                if (++count == stage) {
                    stop.request_stop();
                }
            };
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"refresh"})
            ),
            auth_options(),
            {.drive_id = "me", .endpoint = "https://graph.example.test/v1.0"},
        };
        if (stage == 0) {
            stop.request_stop();
        }
        if (!onedrive::test::throws_with<
                onedrive::graph::RequestCancelledError>(
                [&] {
                    static_cast<void>(client.drive_identity(stop.get_token()));
                },
                "cancelled"
            ) ||
            !forwarded ||
            requests->queued.requests.size() !=
                static_cast<std::size_t>(stage)) {
            return fail(
                "Graph identity startup did not cancel token refresh or HTTP "
                "discovery"
            );
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{4};
    if (const int result = progress.run(
            "shared_resources_and_shortcuts",
            test_shared_resources_and_shortcuts
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "sites_and_document_libraries", test_sites_and_document_libraries
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = progress.run(
            "rejects_invalid_discovery_data",
            test_rejects_invalid_discovery_data
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return progress.finish(progress.run(
        "identity_startup_cancellation", test_identity_startup_cancellation
    ));
}
