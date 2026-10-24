#include "support.hpp"

#include <ranges>
#include <stop_token>
#include "drive_cases.hpp"

namespace onedrive::test::graph::drive_cases {

using namespace onedrive::test::graph;

int test_unsupported_root_and_path_metadata() {
    using Json = nlohmann::json;
    for (const bool package : {false, true}) {
        for (const bool relaxed : {false, true}) {
            Json item{
                {"id", "notes"},
                {"name", package ? "Notebook" : "notes.one"},
                {"eTag", "etag"},
                {"parentReference", {{"path", "/drive/root:"}}},
            };
            if (package) {
                item["package"] = {{"type", "oneNote"}};
            } else {
                item["file"] = {
                    {"mimeType", "application/msonenote"},
                    {"hashes", {{"sha256Hash", "unsupported"}}},
                };
            }
            auto transport = std::make_unique<FakeTransport>(
                std::deque<onedrive::http::HttpResult>{
                    token_response(),
                    json_response(
                        200, Json{{"value", Json::array({item})}}.dump()
                    ),
                    json_response(200, item.dump()),
                }
            );
            onedrive::graph::MicrosoftGraphClient client{
                wrap_transport(std::move(transport)),
                wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
                auth_options(),
                {.endpoint = "https://graph.example.test/v1.0",
                 .relaxed_download_validation = relaxed},
            };
            const auto root = client.list_root();
            const auto looked_up =
                client.item_by_path(item["name"].get<std::string>());
            if (root.size() != 1 || root[0].unsupported_format != "onenote" ||
                root[0].directory != package || root[0].content_hash ||
                looked_up.unsupported_format != "onenote" ||
                looked_up.directory != package || looked_up.content_hash ||
                !looked_up.last_modified.empty() ||
                looked_up.validate_content == relaxed) {
                return fail(
                    "root/path metadata did not identify an unsupported format"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_onenote_ancestor_path_lookup() {
    using Json = nlohmann::json;
    for (const bool related : {false, true}) {
        for (const bool cached : {false, true}) {
            Json target{
                {"id", "child"},
                {"name", "data.bin"},
                {"eTag", "etag"},
                {"size", 4},
                {"parentReference",
                 {{"id", "recycle"},
                  {"path", "/drive/root:/Notebook/OneNote_RecycleBin"}}},
                {"file", {{"hashes", {{"sha256Hash", "unsupported"}}}}},
            };
            if (!related) {
                target["file"]["hashes"] = {
                    {"sha1Hash", "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd"}
                };
                target["fileSystemInfo"] = {
                    {"lastModifiedDateTime", "2026-10-14T00:00:00Z"}
                };
            }
            const Json recycle{
                {"id", "recycle"},
                {"name", "OneNote_RecycleBin"},
                {"folder", Json::object()},
                {"parentReference",
                 {{"id", "package"}, {"path", "/drive/root:/Notebook"}}},
            };
            Json parent{
                {"id", "package"},
                {"name", "Notebook"},
                {"parentReference", {{"id", "root"}, {"path", "/drive/root:"}}},
            };
            parent[related ? "package" : "folder"] =
                related ? Json{{"type", "oneNote"}} : Json::object();
            auto responses = std::deque<onedrive::http::HttpResult>{
                token_response(), json_response(200, target.dump())
            };
            std::vector<onedrive::graph::RemoteItem> context;
            if (cached) {
                context = {
                    {.id = "root",
                     .name = "Drive",
                     .directory = true,
                     .root = true},
                    {.id = "package",
                     .name = "Notebook",
                     .parent_id = "root",
                     .remote_path = "Notebook",
                     .directory = true,
                     .unsupported_format = related ? "onenote" : ""},
                    {.id = "recycle",
                     .name = "OneNote_RecycleBin",
                     .parent_id = "package",
                     .remote_path = "Notebook/OneNote_RecycleBin",
                     .directory = true},
                };
            } else {
                responses.push_back(json_response(200, recycle.dump()));
                responses.push_back(json_response(200, parent.dump()));
                if (!related) {
                    responses.push_back(json_response(
                        200,
                        Json{
                            {"id", "root"},
                            {"name", "Drive"},
                            {"root", Json::object()}
                        }.dump()
                    ));
                }
            }
            auto transport =
                std::make_unique<FakeTransport>(std::move(responses));
            const auto* recorded = transport.get();
            onedrive::graph::MicrosoftGraphClient client{
                wrap_transport(std::move(transport)),
                wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
                auth_options(),
                {.drive_id = "selected-drive",
                 .endpoint = "https://graph.example.test/v1.0"},
            };
            const auto item = client.item_by_path(
                "Notebook/OneNote_RecycleBin/data.bin", {}, context
            );
            if ((item.unsupported_format == "onenote") != related ||
                item.content_hash.has_value() == related ||
                recorded->queued.requests.size() != (cached    ? 2U
                                                     : related ? 4U
                                                               : 5U) ||
                (!cached &&
                 !recorded->queued.requests[2].url.starts_with(
                     "https://graph.example.test/v1.0/drives/selected-drive/"
                     "items/recycle?"
                 ))) {
                return fail(
                    "single path lookup lost ancestry, drive scope, or hash "
                    "validation for an ordinary same-name directory"
                );
            }
        }
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
                R"json({"id":"file-id","name":"report #1.txt","eTag":"file-etag","cTag":"file-ctag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-04T00:00:00Z"},"parentReference":{"id":"folder-id","path":"/drive/root:/Folder A"},"file":{"hashes":{"sha256Hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","quickXorHash":"ZAgDHcIAAAAAAAAABAAAAAAAAAA="}}})json",
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
            onedrive::util::FileHashAlgorithm::quick_xor ||
        item.content_hash->value != "ZAgDHcIAAAAAAAAABAAAAAAAAAA=" ||
        transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/me/drive/root:/Folder%20A/"
            "report%20%231.txt?$select=id,name,eTag,cTag,size,fileSystemInfo,"
            "parentReference,file,folder,deleted,malware,remoteItem,package" ||
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
                .transfer =
                    {
                        .connect_timeout = std::chrono::seconds{12},
                        .operation_timeout = std::chrono::seconds{600},
                        .low_speed_timeout = std::chrono::seconds{15},
                        .low_speed_limit_bytes_per_second = 128,
                        .http_version = onedrive::http::HttpVersion::http_2,
                        .ip_version = onedrive::http::IpVersion::ipv4,
                    },
                .maximum_send_speed_bytes_per_second = 900,
                .maximum_total_send_speed_bytes_per_second = 700,
            },
        },
    };
    std::stop_source stop;
    const auto root = client.create_directory("New #", stop.get_token());
    const auto nested =
        client.create_directory("New #/Child", stop.get_token());
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
    for (const auto& request : requests | std::views::drop(1)) {
        if (request.connect_timeout != std::chrono::seconds{12} ||
            request.operation_timeout != std::chrono::seconds{600} ||
            request.low_speed_timeout != std::chrono::seconds{15} ||
            request.low_speed_limit_bytes_per_second != 128 ||
            request.http_version != onedrive::http::HttpVersion::http_2 ||
            request.ip_version != onedrive::http::IpVersion::ipv4 ||
            request.stop_token != stop.get_token()) {
            return fail("directory creation lost configured transfer options");
        }
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

int test_drive_operations_propagate_cancellation() {
    const auto verify_cancelled = [](auto operation) {
        std::stop_source cancellation;
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                std::unexpected(
                    onedrive::http::HttpError{
                        .code = onedrive::http::HttpErrorCode::cancelled,
                        .message = "simulated Graph request cancellation",
                    }
                ),
            }
        );
        auto* transport_pointer = transport.get();
        transport_pointer->on_perform = [&](const auto& request) {
            if (request.url.starts_with("https://graph.example.test")) {
                cancellation.request_stop();
            }
        };
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(
                std::make_unique<FakeTokenStore>(
                    std::string{"existing-refresh"}
                )
            ),
            auth_options(),
            {
                .drive_id = "drive/id",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };
        try {
            operation(client, cancellation.get_token());
            return fail("Graph operation ignored request cancellation");
        } catch (const onedrive::graph::RequestCancelledError&) {
        }
        const auto& requests = transport_pointer->queued.requests;
        if (requests.size() != 2 ||
            !requests.back().stop_token.stop_requested()) {
            return fail("Graph operation did not pass its stop token");
        }
        return EXIT_SUCCESS;
    };

    if (const int result = verify_cancelled([](auto& client, auto stop_token) {
            static_cast<void>(
                client.item_by_path("Folder/file.txt", stop_token)
            );
        });
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = verify_cancelled([](auto& client, auto stop_token) {
            static_cast<void>(client.create_directory("Folder", stop_token));
        });
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = verify_cancelled([](auto& client, auto stop_token) {
            client.delete_item("file-id", "file-etag", stop_token);
        });
        result != EXIT_SUCCESS) {
        return result;
    }
    return verify_cancelled([](auto& client, auto stop_token) {
        static_cast<void>(client.move_item(
            "file-id", "file-etag", "Folder/file.txt", stop_token
        ));
    });
}

} // namespace onedrive::test::graph::drive_cases
