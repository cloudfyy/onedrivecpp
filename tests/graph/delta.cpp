#include "support.hpp"
#include "graph/delta_changes.hpp"
#include "graph/delta_hierarchy.hpp"
#include "graph/support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/storage/item_database.hpp"
#include "sync/support.hpp"

#include "delta_cases.hpp"

namespace onedrive::test::graph::delta_cases {

using namespace onedrive::test::graph;

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
} // namespace onedrive::test::graph::delta_cases

using namespace onedrive::test::graph::delta_cases;

int main() {
    if (test_delta_mixed_metadata_capabilities() != EXIT_SUCCESS ||
        test_business_delta_paths_across_pages() != EXIT_SUCCESS ||
        test_business_delta_saved_and_fetched_ancestors() != EXIT_SUCCESS ||
        test_delta_rejects_malformed_sizes() != EXIT_SUCCESS ||
        test_delta_cached_ancestor_validation() != EXIT_SUCCESS ||
        test_delta_injected_ancestor_failures() != EXIT_SUCCESS ||
        test_business_delta_moves_and_restart() != EXIT_SUCCESS ||
        test_business_delta_invalid_ancestry_and_cancellation() !=
            EXIT_SUCCESS ||
        test_unsupported_delta_across_pages() != EXIT_SUCCESS ||
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
