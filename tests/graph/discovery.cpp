#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

std::unique_ptr<onedrive::graph::MicrosoftGraphClient>
make_client(std::deque<onedrive::http::HttpResult> responses) {
    responses.push_front(
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        }
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
    return EXIT_SUCCESS;
}

int test_rejects_invalid_discovery_data() {
    try {
        static_cast<void>(make_client({})->search_sites(""));
        return fail("an empty SharePoint site query was accepted");
    } catch (const std::invalid_argument&) {
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

} // namespace

int main() {
    if (const int result = test_shared_resources_and_shortcuts();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_sites_and_document_libraries();
        result != EXIT_SUCCESS) {
        return result;
    }
    return test_rejects_invalid_discovery_data();
}
