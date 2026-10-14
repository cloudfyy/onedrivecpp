#include "support.hpp"
#include "graph/delta_changes.hpp"
#include "graph/support.hpp"

namespace {

using namespace onedrive::test::graph;

int test_unsupported_delta_across_pages() {
    using Json = nlohmann::json;
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        token_response(),
        json_response(
            200,
            Json{
                {"value",
                 Json::array({
                     {{"id", "child"},
                      {"name", "data.bin"},
                      {"parentReference",
                       {{"id", "middle"},
                        {"path", "/drive/root:/Notebook/Sub"}}},
                      {"file", {{"hashes", {{"sha256Hash", "unsupported"}}}}}},
                     {{"id", "notes"},
                      {"name", "notes.one"},
                      {"parentReference", {{"path", "/drive/root:"}}},
                      {"file",
                       {{"mimeType", "application/msonenote"},
                        {"hashes", {{"sha1Hash", "unreliable"}}}}}},
                 })},
                {"@odata.nextLink", "https://graph.example.test/v1.0/page2"}
            }
                .dump()
        ),
        json_response(
            200,
            Json{
                {"value",
                 Json::array({
                     {{"id", "middle"},
                      {"name", "Sub"},
                      {"folder", Json::object()},
                      {"parentReference",
                       {{"id", "package"}, {"path", "/drive/root:/Notebook"}}}},
                     {{"id", "package"},
                      {"name", "Notebook"},
                      {"package", {{"type", "oneNote"}}},
                      {"parentReference", {{"path", "/drive/root:"}}}},
                     {{"id", "ordinary"},
                      {"name", "backup.one"},
                      {"parentReference", {{"path", "/drive/root:"}}},
                      {"fileSystemInfo",
                       {{"lastModifiedDateTime", "2026-10-02T00:00:00Z"}}},
                      {"file",
                       {{"mimeType", "text/plain"},
                        {"hashes",
                         {{"sha1Hash",
                           "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd"}}}}}},
                 })},
                {"@odata.deltaLink", "https://graph.example.test/v1.0/delta"}
            }
                .dump()
        ),
    });
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
        auth_options(),
        {.endpoint = "https://graph.example.test/v1.0"},
    };
    const auto delta = client.list_delta(std::nullopt);
    if (delta.changes.size() != 5 ||
        delta.changes[0].unsupported_format != "onenote" ||
        delta.changes[1].unsupported_format != "onenote" ||
        delta.changes[2].unsupported_format != "onenote" ||
        delta.changes[3].unsupported_format != "onenote" ||
        !delta.changes[3].directory || delta.changes[0].content_hash ||
        delta.changes[1].content_hash ||
        !delta.changes[4].unsupported_format.empty() ||
        !delta.changes[4].content_hash) {
        return fail(
            "late package metadata did not safely exclude its children"
        );
    }
    return EXIT_SUCCESS;
}

int test_onenote_partial_delta_ancestry() {
    namespace graph = onedrive::graph;
    using Json = nlohmann::json;
    for (const bool cached : {false, true}) {
        auto responses = std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page(
                Json::array(
                    {
                        {{"id", "child"},
                         {"name", "data.bin"},
                         {"parentReference",
                          {{"id", "recycle"},
                           {"path",
                            "/drive/root:/Notebook/OneNote_RecycleBin"}}},
                         {"file",
                          {{"hashes", {{"sha256Hash", "unsupported"}}}}}},
                    }
                )
                    .dump(),
                std::nullopt,
                "https://graph.example.test/v1.0/delta"
            )
        };
        std::vector<graph::RemoteItem> context;
        if (cached) {
            context = {
                {.id = "package",
                 .name = "Notebook",
                 .remote_path = "Notebook",
                 .directory = true,
                 .unsupported_format = "onenote"},
                {.id = "recycle",
                 .name = "OneNote_RecycleBin",
                 .parent_id = "package",
                 .remote_path = "Notebook/OneNote_RecycleBin",
                 .directory = true},
            };
        } else {
            responses.push_back(json_response(
                200,
                Json{
                    {"id", "recycle"},
                    {"name", "OneNote_RecycleBin"},
                    {"folder", Json::object()},
                    {"parentReference",
                     {{"id", "package"}, {"path", "/drive/root:/Notebook"}}},
                }
                    .dump()
            ));
            responses.push_back(json_response(
                200,
                Json{
                    {"id", "package"},
                    {"name", "Notebook"},
                    {"package", {{"type", "oneNote"}}},
                    {"parentReference", {{"path", "/drive/root:"}}},
                }
                    .dump()
            ));
        }
        auto transport = std::make_unique<FakeTransport>(std::move(responses));
        const auto* recorded = transport.get();
        graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        const auto result = client.list_delta(
            "https://graph.example.test/v1.0/old", {}, {}, context
        );
        if (result.changes.size() != (cached ? 1U : 3U) ||
            recorded->queued.requests.size() != (cached ? 2U : 4U) ||
            std::ranges::any_of(result.changes, [](const auto& item) {
                return item.unsupported_format != "onenote" ||
                       item.content_hash.has_value();
            })) {
            return fail(
                "partial delta ancestry was not resolved before strict hash "
                "validation or discovered package diagnostics were lost"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_onenote_ancestor_lookup_failures() {
    namespace od = onedrive;
    using Json = nlohmann::json;
    for (const bool cancelled : {false, true}) {
        auto transport =
            std::make_unique<FakeTransport>(std::deque<od::http::HttpResult>{
                token_response(),
                graph_page(
                    Json::array({
                                    {{"id", "recycle"},
                                     {"name", "OneNote_RecycleBin"},
                                     {"folder", Json::object()},
                                     {"parentReference",
                                      {{"id", "package"},
                                       {"path", "/drive/root:/Notebook"}}}},
                                })
                        .dump(),
                    std::nullopt,
                    "https://graph.example.test/v1.0/delta"
                ),
                cancelled
                    ? od::http::HttpResult{std::unexpected{od::http::HttpError{
                          .code = od::http::HttpErrorCode::cancelled,
                          .message = "ancestor request cancelled",
                      }}}
                    : od::http::HttpResult{graph_error_response(
                          403, "accessDenied", "ancestor access denied"
                      )},
            });
        const auto* recorded = transport.get();
        od::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        std::stop_source stop;
        bool rejected = false;
        try {
            static_cast<void>(
                client.list_delta(std::nullopt, {}, stop.get_token())
            );
        } catch (const od::graph::RequestCancelledError&) {
            rejected = cancelled;
        } catch (const std::runtime_error& error) {
            rejected = !cancelled && std::string_view{error.what()}.contains(
                                         "ancestor access denied"
                                     );
        }
        if (!rejected || recorded->queued.requests.size() != 3 ||
            !recorded->queued.requests.back().stop_token.stop_possible()) {
            return fail(
                "ancestor access failure/cancellation silently allowed a "
                "same-name directory or lost cancellation"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_shared_delta_changes() {
    namespace graph = onedrive::graph;
    graph::RemoteItem first{
        .id = "first",
        .name = "old.txt",
        .etag = "old",
        .remote_path = "old.txt",
        .malware = true,
        .content_hash =
            onedrive::util::FileHash{
                .algorithm = onedrive::util::FileHashAlgorithm::sha256,
                .value = std::string(64, '0'),
            },
        .validate_content = false,
        .unsupported_format = "onenote",
    };
    graph::RemoteItem second{.id = "second", .deleted = true};
    auto latest = first;
    latest.name = latest.remote_path = "new.txt";
    latest.etag = "new";
    latest.malware = false;
    latest.validate_content = true;
    latest.content_hash.reset();
    latest.unsupported_format.clear();
    graph::client_detail::DeltaChanges changes{
        std::vector<graph::RemoteItem>{first, second, latest}
    };
    if (changes.size() != 2 || changes.find("missing") ||
        !changes.find("first") || changes.find("first")->etag != "new") {
        return fail(
            "seeded delta merge did not preserve unique latest records"
        );
    }
    changes.upsert({.id = "third", .root = true});
    changes.upsert(
        {.id = "second", .name = "restored.txt", .remote_path = "restored.txt"}
    );
    const auto result = std::move(changes).release();
    if (result.size() != 3 || result[0].id != "first" ||
        result[0].name != "new.txt" || result[0].remote_path != "new.txt" ||
        result[0].malware || result[0].content_hash ||
        !result[0].validate_content || !result[0].unsupported_format.empty() ||
        result[1].id != "second" || result[1].deleted ||
        result[1].name != "restored.txt" || result[2].id != "third" ||
        !result[2].root) {
        return fail(
            "shared delta merge retained old flags or changed first-seen order"
        );
    }
    return EXIT_SUCCESS;
}

int test_delta_transport_and_shape_failures() {
    namespace od = onedrive;
    const auto rejected = [](od::http::HttpResult response,
                             std::string_view message,
                             bool cancelled = false) {
        auto transport =
            std::make_unique<FakeTransport>(std::deque<od::http::HttpResult>{
                token_response(), std::move(response)
            });
        auto* recorded = transport.get();
        od::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        try {
            static_cast<void>(client.list_delta(std::nullopt));
        } catch (const od::graph::RequestCancelledError&) {
            return cancelled && recorded->queued.requests.size() == 2;
        } catch (const std::runtime_error& error) {
            return !cancelled &&
                   std::string_view{error.what()}.contains(message) &&
                   recorded->queued.requests.size() == 2;
        }
        return false;
    };
    if (!rejected(
            std::unexpected{od::http::HttpError{
                .code = od::http::HttpErrorCode::transport,
                .message = "offline transport failed",
            }},
            "offline transport failed"
        ) ||
        !rejected(
            std::unexpected{od::http::HttpError{
                .code = od::http::HttpErrorCode::cancelled,
                .message = "cancelled transport",
            }},
            {},
            true
        )) {
        return fail(
            "delta transport error/cancellation was hidden or misclassified"
        );
    }
    const std::vector<std::pair<std::string, std::string>> cases{
        {R"({"value":[{"id":"","name":"Drive","root":{},"folder":{}}]})",
         "empty ID"},
        {R"({"value":[{"id":"root","name":"Drive","eTag":42,"root":{},"folder":{}}]})",
         "invalid eTag"},
        {R"({"value":[{"id":"root","name":"","root":{},"folder":{}}]})",
         "invalid metadata"},
        {R"({"value":[{"id":"root","name":"Drive","size":-1,"root":{},"folder":{}}]})",
         "invalid metadata"},
        {R"({"value":[{"name":"missing ID"}]})",
         "missing required drive item data"},
        {R"({"value":[]})", "final delta link"},
        {R"({"value":[],"@odata.deltaLink":42})", "final delta link"},
    };
    for (const auto& [body, message] : cases) {
        if (!rejected(json_response(200, body), message)) {
            return fail(
                "invalid delta metadata/page shape was accepted: " + message
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_delta_cancelled_before_request_or_between_pages() {
    namespace od = onedrive;
    for (const bool before_request : {false, true}) {
        auto transport =
            std::make_unique<FakeTransport>(std::deque<od::http::HttpResult>{
                token_response(),
                graph_page(
                    "[]", "https://graph.example.test/v1.0/delta?page=2"
                ),
            });
        auto* recorded = transport.get();
        od::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        std::stop_source stop;
        if (before_request) {
            stop.request_stop();
        }
        bool progress_seen = false;
        try {
            static_cast<void>(client.list_delta(
                std::nullopt,
                [&](auto, auto, auto state) {
                    progress_seen = state == od::util::ProgressState::ongoing;
                    stop.request_stop();
                },
                stop.get_token()
            ));
            return fail("cancelled delta returned a partial successful result");
        } catch (const od::graph::RequestCancelledError&) {
            if (recorded->queued.requests.size() !=
                    (before_request ? 0U : 2U) ||
                progress_seen == before_request) {
                return fail(
                    "delta cancellation issued an unexpected request/page"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_untrusted_pagination_url() {
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page("[]", "https://attacker.example/collect"),
        });
    auto* transport_pointer = transport.get();
    onedrive::graph::MicrosoftGraphClient client{
        wrap_transport(std::move(transport)),
        wrap_token_store(
            std::make_unique<FakeTokenStore>(std::string{"existing-refresh"})
        ),
        auth_options(),
    };

    try {
        static_cast<void>(client.list_root());
        return fail("untrusted Graph pagination URL was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("outside its endpoint")) {
            return fail("untrusted pagination URL error was not reported");
        }
    }
    if (transport_pointer->queued.requests.size() != 2) {
        return fail(
            "authorization token was sent to an untrusted pagination URL"
        );
    }
    return EXIT_SUCCESS;
}

int test_invalid_root_page_shapes() {
    const auto rejected = [](
                              std::string body,
                              std::string_view expected_message
                          ) {
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"expires_in":3600,"access_token":"access-secret"})",
                },
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body = std::move(body),
                },
            }
        );
        auto* transport_pointer = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>(
                std::string{"existing-refresh"}
            )),
            auth_options(),
        };
        try {
            static_cast<void>(client.list_root());
        } catch (const std::runtime_error& error) {
            return std::string_view{error.what()}.contains(expected_message) &&
                transport_pointer->queued.requests.size() == 2;
        }
        return false;
    };

    if (!rejected(
            R"json({"value":[],"@odata.nextLink":"https://graph.microsoft.com/v1.0/me/drive/root/children"})json",
            "repeated pagination URL"
        ) ||
        !rejected(
            R"json({"value":[],"@odata.nextLink":42})json",
            "invalid pagination URL"
        ) ||
        !rejected(R"json({})json", "missing field 'value'") ||
        !rejected(
            R"json({"value":{}})json",
            "field 'value' is not an array"
        )) {
        return fail("invalid Graph root page shape was accepted");
    }
    return EXIT_SUCCESS;
}

int test_delta_with_pagination() {
    auto transport = std::make_unique<
        FakeTransport>(std::deque<onedrive::http::HttpResult>{
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"expires_in":3600,"access_token":"access-secret"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body = R"({"value":[{"id":"root-id","name":"Drive",)"
                    R"("size":0,"root":{},"folder":{"childCount":1}},)"
                    R"({"id":"folder-id","name":"Documents",)"
                    R"("size":0,)"
                    R"("lastModifiedDateTime":"2026-10-02T00:00:00Z",)"
                    R"("parentReference":{"id":"root-id",)"
                    R"("path":"/drives/drive-id/root:"},)"
                    R"("folder":{"childCount":1}}],)"
                    R"("@odata.nextLink":)"
                    R"("https://graph.example.test/v1.0/delta?page=2"})",
        },
        onedrive::http::HttpResponse{
            .status_code = 200,
            .body =
                R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"file-etag","cTag":"file-ctag","size":42,"lastModifiedDateTime":"2026-10-02T00:01:00Z","fileSystemInfo":{"lastModifiedDateTime":"2026-10-01T23:59:58.123456789Z"},"parentReference":{"id":"folder-id","path":"/drives/drive-id/root:/Documents"},"file":{"mimeType":"text/plain","hashes":{"quickXorHash":"SgAAAAAAAAAAAAAAAQAAAAAAAAA=","sha256Hash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}}},{"id":"shortcut-id","name":"shared.txt","eTag":"shortcut-etag","size":7,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-01T20:00:00Z"},"remoteItem":{"fileSystemInfo":{"lastModifiedDateTime":"2026-10-01T21:00:00Z"},"malware":{}},"parentReference":{"id":"folder-id","path":"/drives/drive-id/root:/Documents"},"file":{"mimeType":"text/plain"}},{"id":"deleted-id","deleted":{"state":"deleted"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?token=final"})json",
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
            .drive_id = "drive id",
            .endpoint = "https://graph.example.test/v1.0",
            .relaxed_download_validation = true,
        },
    };

    using ProgressRecord =
        std::tuple<std::size_t, std::size_t, onedrive::util::ProgressState>;
    std::vector<ProgressRecord> progress;
    const auto delta = client.list_delta(
        std::nullopt,
        [&progress](
            std::size_t pages,
            std::size_t items,
            onedrive::util::ProgressState state
        ) { progress.emplace_back(pages, items, state); }
    );
    if (delta.changes.size() != 5 ||
        delta.delta_link !=
            "https://graph.example.test/v1.0/delta?token=final" ||
        !delta.changes[0].root || !delta.changes[0].remote_path.empty() ||
        !delta.changes[1].directory || !delta.changes[1].etag.empty() ||
        delta.changes[1].remote_path != "Documents" ||
        delta.changes[2].remote_path != "Documents/notes.txt" ||
        delta.changes[2].ctag != "file-ctag" ||
        delta.changes[2].parent_id != "folder-id" ||
        delta.changes[2].size != 42 ||
        delta.changes[2].last_modified != "2026-10-01T23:59:58.123456789Z" ||
        !delta.changes[2].content_hash ||
        delta.changes[2].content_hash->algorithm !=
            onedrive::util::FileHashAlgorithm::quick_xor ||
        delta.changes[2].content_hash->value !=
            "SgAAAAAAAAAAAAAAAQAAAAAAAAA=" ||
        delta.changes[2].validate_content ||
        delta.changes[3].last_modified != "2026-10-01T21:00:00Z" ||
        !delta.changes[3].malware || !delta.changes[4].deleted ||
        progress != std::vector<ProgressRecord>{
                        {1, 2, onedrive::util::ProgressState::ongoing},
                        {2, 5, onedrive::util::ProgressState::completed},
                    }) {
        return fail("Graph delta items or final link were not parsed");
    }
    if (transport_pointer->queued.requests.size() != 3 ||
        transport_pointer->queued.requests[1].url !=
            "https://graph.example.test/v1.0/drives/drive%20id/root/delta" ||
        transport_pointer->queued.requests[2].url !=
            "https://graph.example.test/v1.0/delta?page=2") {
        return fail("Graph delta pagination requests were incorrect");
    }
    return EXIT_SUCCESS;
}
int test_delta_requires_valid_file_system_modified_time() {
    const auto rejected = [](std::string body, std::string_view message) {
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body =
                        R"({"expires_in":3600,"access_token":"access-secret"})",
                },
                onedrive::http::HttpResponse{
                    .status_code = 200,
                    .body = std::move(body),
                },
            }
        );
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
            static_cast<void>(client.list_delta(std::nullopt));
        } catch (const std::runtime_error& error) {
            return std::string_view{error.what()}.contains(message);
        }
        return false;
    };

    constexpr std::string_view missing_time{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{missing_time},
            "missing fileSystemInfo.lastModifiedDateTime"
        )) {
        return fail(
            "Graph delta file without authoritative mtime was accepted"
        );
    }

    constexpr std::string_view invalid_time{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-02-30T00:00:00Z"},"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{invalid_time},
            "invalid Microsoft Graph modification time"
        )) {
        return fail(
            "Graph delta file with invalid authoritative mtime was accepted"
        );
    }

    constexpr std::string_view invalid_ctag{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","cTag":42,"size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-02T00:00:00Z"},"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(std::string{invalid_ctag}, "invalid cTag")) {
        return fail("Graph delta file with an invalid cTag was accepted");
    }

    constexpr std::string_view invalid_malware{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-02T00:00:00Z"},"malware":true,"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{invalid_malware}, "invalid drive item malware facet"
        )) {
        return fail(
            "Graph delta file with invalid malware metadata was accepted"
        );
    }

    constexpr std::string_view invalid_remote_malware{
        R"json({"value":[{"id":"file-id","name":"notes.txt","eTag":"etag","size":4,"fileSystemInfo":{"lastModifiedDateTime":"2026-10-02T00:00:00Z"},"remoteItem":{"malware":true},"parentReference":{"id":"root","path":"/drive/root:"},"file":{"mimeType":"text/plain"}}],"@odata.deltaLink":"https://graph.example.test/v1.0/delta?done"})json"
    };
    if (!rejected(
            std::string{invalid_remote_malware},
            "invalid remoteItem malware facet"
        )) {
        return fail(
            "Graph delta file with invalid remote malware metadata was accepted"
        );
    }
    return EXIT_SUCCESS;
}
int test_delta_resume_and_url_validation() {
    constexpr std::string_view saved_delta_link{
        "https://graph.example.test/v1.0/delta?token=saved"
    };
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page(
                "[]",
                std::nullopt,
                "https://graph.example.test/v1.0/delta?token=next"
            ),
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
    const auto delta = client.list_delta(std::string{saved_delta_link});
    if (!delta.changes.empty() ||
        delta.delta_link !=
            "https://graph.example.test/v1.0/delta?token=next" ||
        transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[1].url != saved_delta_link) {
        return fail("saved Graph delta link was not resumed");
    }

    auto untrusted_transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"value":[],"@odata.deltaLink":)"
                        R"("https://attacker.example/collect"})",
            },
        });
    onedrive::graph::MicrosoftGraphClient untrusted_client{
        wrap_transport(std::move(untrusted_transport)),
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
        static_cast<void>(untrusted_client.list_delta(std::nullopt));
        return fail("untrusted Graph delta link was accepted");
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("outside its endpoint")) {
            return fail("untrusted Graph delta link error was not reported");
        }
    }
    return EXIT_SUCCESS;
}
int test_invalid_delta_cursor_error() {
    constexpr std::string_view saved_delta_link{
        "https://graph.example.test/v1.0/delta?token=expired"
    };
    auto transport =
        std::make_unique<FakeTransport>(std::deque<onedrive::http::HttpResult>{
            onedrive::http::HttpResponse{
                .status_code = 200,
                .body = R"({"expires_in":3600,"access_token":"access-secret"})",
            },
            onedrive::http::HttpResponse{
                .status_code = 410,
                .body = R"({"error":{"code":"resyncRequired",)"
                        R"("message":"The delta token is no longer valid."}})",
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

    try {
        static_cast<void>(client.list_delta(std::string{saved_delta_link}));
        return fail("invalid saved Graph delta cursor was accepted");
    } catch (const onedrive::graph::DeltaCursorInvalidError& error) {
        if (!std::string{error.what()}.contains(
                "delta token is no longer valid"
            )) {
            return fail(
                "invalid Graph delta cursor error omitted the server detail"
            );
        }
    }
    if (transport_pointer->queued.requests.size() != 2 ||
        transport_pointer->queued.requests[1].url != saved_delta_link) {
        return fail("invalid Graph delta cursor request was incorrect");
    }
    return EXIT_SUCCESS;
}

int test_delta_request_cancellation() {
    const auto run_cancelled_request = [](std::size_t cancel_on_call) {
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                graph_page(
                    "[]",
                    std::nullopt,
                    "https://graph.example.test/v1.0/delta"
                ),
            }
        );
        auto* transport_pointer = transport.get();
        std::stop_source stop;
        std::size_t calls = 0;
        transport_pointer->on_perform =
            [&](const onedrive::http::HttpRequest&) {
                if (++calls == cancel_on_call) {
                    stop.request_stop();
                }
            };
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>(
                std::string{"existing-refresh"}
            )),
            auth_options(),
            {
                .drive_id = "drive id",
                .endpoint = "https://graph.example.test/v1.0",
            },
        };

        try {
            static_cast<void>(
                client.list_delta(std::nullopt, {}, stop.get_token())
            );
            return false;
        } catch (const onedrive::graph::RequestCancelledError&) {
            return transport_pointer->queued.requests.size() ==
                       cancel_on_call &&
                   transport_pointer->queued.requests.back()
                       .stop_token.stop_possible();
        }
    };

    if (!run_cancelled_request(1) || !run_cancelled_request(2)) {
        return fail(
            "Graph delta did not propagate cancellation through token refresh "
            "and page requests"
        );
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    if (test_unsupported_delta_across_pages() != EXIT_SUCCESS ||
        test_onenote_partial_delta_ancestry() != EXIT_SUCCESS ||
        test_onenote_ancestor_lookup_failures() != EXIT_SUCCESS ||
        test_shared_delta_changes() != EXIT_SUCCESS ||
        test_delta_transport_and_shape_failures() != EXIT_SUCCESS ||
        test_delta_cancelled_before_request_or_between_pages() !=
            EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (const int result = test_untrusted_pagination_url();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_root_page_shapes();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_delta_with_pagination();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result =
            test_delta_requires_valid_file_system_modified_time();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_delta_resume_and_url_validation();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_invalid_delta_cursor_error();
        result != EXIT_SUCCESS) {
        return result;
    }
    if (const int result = test_delta_request_cancellation();
        result != EXIT_SUCCESS) {
        return result;
    }
    return EXIT_SUCCESS;
}
