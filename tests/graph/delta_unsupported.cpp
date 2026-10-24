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

} // namespace onedrive::test::graph::delta_cases
