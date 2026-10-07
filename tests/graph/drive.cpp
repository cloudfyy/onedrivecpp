#include "support.hpp"

namespace {

using namespace onedrive::test::graph;

int test_list_root_with_refresh_and_pagination() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"rotated-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"value":[{"id":"folder-id","name":"Documents",)"
                    R"("eTag":"folder-etag","folder":{"childCount":2}}],)"
                    R"("@odata.nextLink":)"
                    R"("https://graph.example.test/v1.0/next?page=2"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","malware":{},"file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA="}}}]})json",
        },
    });
    auto* transport_pointer = transport.get();
    auto token_store =
        std::make_unique<FakeTokenStore>(std::string{"existing-refresh"});
    auto* token_store_pointer = token_store.get();

    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::move(token_store)),
        auth_options(),
        {
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0/",
        },
    };
    const auto items = client.list_root();

    if (items.size() != 2 || !items[0].directory || items[1].directory ||
        items[0].name != "Documents" || items[1].etag != "file-etag" ||
        !items[1].malware || !items[1].content_hash ||
        items[1].content_hash->algorithm !=
            onedrive::util::FileHashAlgorithm::quick_xor ||
        items[1].content_hash->value != "SgAAAAAAAAAAAAAAAQAAAAAAAAA=") {
        return fail("Graph drive items were not parsed across pages");
    }

    if (token_store_pointer->saved_tokens !=
        std::vector<std::string>{"rotated-refresh"}) {
        return fail("rotated refresh token was not persisted");
    }
    if (transport_pointer->queued.requests.size() != 3 ||
        transport_pointer->queued.requests[0].method !=
            onedrive::http::HttpMethod::post ||
        !transport_pointer->queued.requests[0].body.contains(
            "refresh_token=existing-refresh"
        ) ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/root/children" ||
        transport_pointer->queued.requests[2].url !=
            "https://graph.example.test/v1.0/next?page=2" ||
        !has_header(
            transport_pointer->queued.requests[1],
            "Authorization: Bearer access-secret"
        )) {
        return fail("Graph authentication or pagination request was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_drive_information_and_quota() {
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
                R"json({"value":[{"id":"drive-1","name":"OneDrive","driveType":"personal","webUrl":"https://example.test/one","owner":{"user":{"displayName":"Alice"}},"quota":{"total":1000,"used":400,"remaining":600,"deleted":25,"state":"normal"}}],"@odata.nextLink":"https://graph.example.test/v1.0/drives-next"})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"drive-2","name":"Team","driveType":"business","owner":{"group":{"displayName":"Team Owner"}}}]})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"drive-1","name":"OneDrive","driveType":"personal","webUrl":"https://example.test/one","owner":{"user":{"displayName":"Alice"}},"quota":{"total":1000,"used":400,"remaining":600,"deleted":25,"state":"normal"}})json",
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
    const auto drives = client.list_drives();
    const auto current = client.drive_info();
    const auto& requests = transport_pointer->queued.requests;
    if (drives.size() != 2 || drives[0].owner != "Alice" || !drives[0].quota ||
        drives[0].quota->remaining != 600 || drives[1].owner != "Team Owner" ||
        drives[1].quota || current.id != "drive-1" || !current.quota ||
        current.quota->deleted != 25 || requests.size() != 4 ||
        requests[1].url !=
            "https://graph.example.test/v1.0/me/drives?$select=id,name,"
            "driveType,webUrl,owner,quota" ||
        requests[2].url != "https://graph.example.test/v1.0/drives-next" ||
        requests[3].url !=
            "https://graph.example.test/v1.0/me/drive?$select=id,name,"
            "driveType,webUrl,owner,quota") {
        return fail("Graph drive information or quota was not parsed");
    }
    return EXIT_SUCCESS;
}

int expect_invalid_drive_info(
    std::string body, std::string_view failure_message
) {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = std::move(body),
            },
        });
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
    try {
        static_cast<void>(client.drive_info());
        return fail(failure_message);
    } catch (const std::runtime_error&) {
        return EXIT_SUCCESS;
    }
}

int test_rejects_invalid_drive_metadata() {
    {
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
                    R"json({"value":[],"@odata.nextLink":"https://attacker.example/drives"})json",
            },
        });
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };
        try {
            static_cast<void>(client.list_drives());
            return fail("an external drives pagination URL was accepted");
        } catch (const std::runtime_error&) {
        }
    }
    {
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
                    R"json({"id":"drive-1","name":"OneDrive","quota":{"total":1000,"used":400,"remaining":-1,"deleted":25}})json",
            },
        });
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };
        try {
            static_cast<void>(client.drive_info());
            return fail("a negative drive quota was accepted");
        } catch (const std::runtime_error&) {
        }
    }
    if (const int result = expect_invalid_drive_info(
            R"json({"id":"drive-1","name":"OneDrive","quota":[]})json",
            "a non-object drive quota was accepted"
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = expect_invalid_drive_info(
            R"json({"id":"drive-1","name":"OneDrive","quota":{"total":1000}})json",
            "an incomplete drive quota was accepted"
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = expect_invalid_drive_info(
            R"json({"id":"","name":"OneDrive"})json",
            "incomplete drive metadata was accepted"
        );
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}

int test_item_lookup_by_encoded_path() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret",)"
                    R"("refresh_token":"existing-refresh"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"id":"file-id","name":"report #1.txt","eTag":"file-etag","cTag":"file-ctag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T00:00:00Z"},"parentReference":{"id":"folder-id","path":"/drive/root:/Folder A"},"file":{"hashes":{"sha256Hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}})json",
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

    const auto item = client.item_by_path("Folder A/report #1.txt");
    if (item.id != "file-id" || item.name != "report #1.txt" ||
        item.remote_path != "Folder A/report #1.txt" ||
        item.parent_id != "folder-id" || item.ctag != "file-ctag" ||
        item.size != 4 || item.directory ||
        item.last_modified != "2026-10-04T00:00:00Z" || !item.content_hash ||
        item.content_hash->algorithm !=
            onedrive::util::FileHashAlgorithm::sha256 ||
        transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/me/drive/root:/Folder%20A/"
            "report%20%231.txt?$select=id,name,eTag,cTag,size,fileSystemInfo,"
            "parentReference,file,folder,deleted,malware,remoteItem" ||
        !has_header(
            transport_pointer->queued.requests[1], "Authorization: ******"
        )) {
        return fail("Graph path lookup was not encoded or parsed correctly");
    }

    try {
        static_cast<void>(client.item_by_path("../unsafe.txt"));
        return fail("unsafe Graph path lookup was accepted");
    } catch (const std::invalid_argument&) {
    }
    if (transport_pointer->queued.requests.size() != 2) {
        return fail("unsafe Graph path lookup made an HTTP request");
    }
    return EXIT_SUCCESS;
}
int test_directory_creation() {
    const auto root_directory =
        R"json({"id":"root-directory","name":"New #","eTag":"root-etag","size":0,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T09:00:00Z"},"parentReference":{"id":"root","path":"/drive/root:"},"folder":{"childCount":0}})json";
    const auto nested_directory =
        R"json({"id":"nested-directory","name":"Child","eTag":"nested-etag","size":0,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T09:00:00Z"},"parentReference":{"id":"parent","path":"/drive/root:/New #"},"folder":{"childCount":0}})json";
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 201,
                .body = root_directory,
            },
            onedrive::http::HttpResponse{
                .status_code = 201,
                .body = nested_directory,
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
            .drive_id = "drive/id",
            .endpoint = "https://graph.example.test/v1.0",
            .upload_transport = {
                .maximum_send_speed_bytes_per_second = 900,
                .maximum_total_send_speed_bytes_per_second = 700,
            },
        },
    };
    const auto root = client.create_directory("New #");
    const auto nested = client.create_directory("New #/Child");
    const auto& requests = transport_pointer->queued.requests;
    if (!root.directory || root.remote_path != "New #" || !nested.directory ||
        nested.remote_path != "New #/Child" || requests.size() != 3 ||
        requests[1].url != "https://graph.example.test/v1.0/drives/drive%2Fid/"
                           "root/children" ||
        requests[2].url != "https://graph.example.test/v1.0/drives/drive%2Fid/"
                           "root:/New%20%23:/children" ||
        requests[1].method != onedrive::http::HttpMethod::post ||
        !has_header(requests[1], "Authorization: ******") ||
        !requests[1].body.contains(R"("name":"New #")") ||
        !requests[1].body.contains(R"("folder":{})") ||
        !requests[1].body.contains(
            R"("@microsoft.graph.conflictBehavior":"fail")"
        ) ||
        requests[1].maximum_send_speed_bytes_per_second != 700) {
        return fail("Graph directory creation request was invalid");
    }

    auto conflict_transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 409,
            .body =
                R"json({"error":{"code":"nameAlreadyExists","message":"exists"}})json",
        },
        onedrive::http::HttpResponse{
            .status_code = 507,
            .body =
                R"json({"error":{"code":"storageLimitExceeded","message":"quota full"}})json",
        },
    });
    onedrive::graph::MicrosoftGraphClient conflict_client{
        wrap_transport(std::move(conflict_transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    try {
        static_cast<void>(conflict_client.create_directory("Existing"));
        return fail("Graph directory conflict was accepted");
    } catch (const onedrive::graph::UploadConflictError&) {
    }
    try {
        static_cast<void>(conflict_client.create_directory("Quota"));
        return fail("Graph directory quota failure was accepted");
    } catch (const onedrive::graph::UploadResourceError& error) {
        if (error.reason_code() != "remote_quota") {
            return fail("Graph directory quota failure lost its reason");
        }
    }
    return EXIT_SUCCESS;
}
int test_item_deletion() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"token_type":"Bearer","expires_in":3600,)"
                    R"("access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{.status_code = 204},
        onedrive::http::HttpResponse{.status_code = 404},
        onedrive::http::HttpResponse{
            .status_code = 412,
            .body =
                R"json({"error":{"code":"preconditionFailed","message":"changed"}})json",
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
            .drive_id = "drive/id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    client.delete_item("file/id", "file-etag");
    client.delete_item("missing", "missing-etag");
    try {
        client.delete_item("changed", "old-etag");
        return fail("Graph deletion conflict was accepted");
    } catch (const onedrive::graph::UploadConflictError&) {
    }
    const auto& requests = transport_pointer->queued.requests;
    if (requests.size() != 4 ||
        requests[1].method != onedrive::http::HttpMethod::delete_ ||
        requests[1].url != "https://graph.example.test/v1.0/drives/drive%2Fid/"
                           "items/file%2Fid" ||
        !has_header(requests[1], "If-Match: file-etag") ||
        !has_header(requests[1], "Authorization: ******")) {
        return fail("Graph item deletion request was invalid");
    }
    return EXIT_SUCCESS;
}
int test_item_move() {
    const auto moved_json =
        R"json({"id":"file/id","name":"renamed.txt","eTag":"new-etag","size":7,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-05T02:00:00Z"},"parentReference":{"id":"parent-id","path":"/drive/root:/Target"},"file":{}})json";
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"token_type":"Bearer","expires_in":3600,)"
                        R"("access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = moved_json,
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
            .drive_id = "drive/id",
            .endpoint = "https://graph.example.test/v1.0",
        },
    };
    const auto moved =
        client.move_item("file/id", "old-etag", "Target/renamed.txt");
    const auto& request = transport_pointer->queued.requests[1];
    if (moved.id != "file/id" || moved.remote_path != "Target/renamed.txt" ||
        request.method != onedrive::http::HttpMethod::patch ||
        request.url != "https://graph.example.test/v1.0/drives/drive%2Fid/"
                       "items/file%2Fid" ||
        !has_header(request, "If-Match: old-etag") ||
        !request.body.contains(R"("name":"renamed.txt")") ||
        !request.body.contains(R"("path":"/drive/root:/Target")")) {
        return fail("Graph item move request was invalid");
    }
    return EXIT_SUCCESS;
}
int test_drive_identity_and_profile_photo() {
    FakeTransport transport{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"user-id","displayName":"Alice Example"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"canonical-drive-id","name":"Alice Drive"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .headers =
                {
                    {.name = "Content-Type", .value = "image/png"},
                },
            .body = "photo-bytes",
        },
    }};
    onedrive::http::HttpTransport transport_proxy{
        onedrive::util::borrowed_proxy, transport
    };
    const auto identity = onedrive::graph::fetch_drive_identity(
        transport_proxy,
        "access-token",
        {
            .drive_id = "me",
            .endpoint = "https://graph.example.test/v1.0",
        }
    );
    if (identity.user_id != "user-id" ||
        identity.user_display_name != "Alice Example" ||
        identity.configured_drive_id != "me" ||
        identity.drive_id != "canonical-drive-id" ||
        identity.drive_name != "Alice Drive" || !identity.photo ||
        identity.photo->content_type != "image/png" ||
        std::string{
            identity.photo->bytes.begin(), identity.photo->bytes.end()
        } != "photo-bytes" ||
        transport.queued.requests.size() != 3 ||
        transport.queued.requests[0].url !=
            "https://graph.example.test/v1.0/me?$select=id,displayName" ||
        transport.queued.requests[1].url !=
            "https://graph.example.test/v1.0/me/drive?$select=id,name" ||
        transport.queued.requests[2].url !=
            "https://graph.example.test/v1.0/me/photo/$value" ||
        !has_header(transport.queued.requests[2], "Authorization: ******")) {
        return fail("Graph account and drive identity were not loaded");
    }
    FakeTransport without_photo{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"user-id","displayName":"Alice Example"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"id":"canonical-drive-id","name":"Alice Drive"})",
        },
        onedrive::http::HttpResponse{.status_code = 404},
    }};
    onedrive::http::HttpTransport without_photo_proxy{
        onedrive::util::borrowed_proxy, without_photo
    };
    if (onedrive::graph::fetch_drive_identity(
            without_photo_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        )
            .photo) {
        return fail("missing Graph profile photo was not treated as optional");
    }
    FakeTransport invalid_identity{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = "{",
        },
    }};
    onedrive::http::HttpTransport invalid_identity_proxy{
        onedrive::util::borrowed_proxy, invalid_identity
    };
    try {
        static_cast<void>(onedrive::graph::fetch_drive_identity(
            invalid_identity_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        ));
        return fail("invalid Graph identity JSON was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("invalid user identity JSON")) {
            return fail("invalid Graph JSON lost its request context");
        }
    }
    FakeTransport denied_identity{std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 403,
            .body =
                R"json({"error":{"code":"accessDenied","message":"denied"}})json",
        },
    }};
    onedrive::http::HttpTransport denied_identity_proxy{
        onedrive::util::borrowed_proxy, denied_identity
    };
    try {
        static_cast<void>(onedrive::graph::fetch_drive_identity(
            denied_identity_proxy,
            "access-token",
            {
                .drive_id = "me",
                .endpoint = "https://graph.example.test/v1.0",
            }
        ));
        return fail("denied Graph identity query was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "Microsoft Graph user identity query failed: denied"
            )) {
            return fail("Graph identity error lost its request context");
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (const int result = test_list_root_with_refresh_and_pagination();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_drive_information_and_quota();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_rejects_invalid_drive_metadata();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_lookup_by_encoded_path();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_directory_creation(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_deletion(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_item_move(); result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_drive_identity_and_profile_photo();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
