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

namespace {

nlohmann::json business_delta_item(
    std::string id, std::string name, std::string parent, bool directory = false
) {
    nlohmann::json item{
        {"id", id},
        {"name", name},
        {"eTag", "etag-" + id},
        {"size", directory ? 0 : 4},
        {"parentReference", {{"id", parent}}},
    };
    if (directory) {
        item["folder"] = nlohmann::json::object();
    } else {
        item["file"] = {
            {"hashes",
             {{"sha1Hash", "a17c9aaa61e80a1bf71d0d850af4e5baa9800bbd"}}}
        };
        item["fileSystemInfo"] = {
            {"lastModifiedDateTime", "2026-10-24T00:00:00Z"}
        };
    }
    return item;
}

} // namespace

int test_delta_mixed_metadata_capabilities() {
    using Json = nlohmann::json;
    for (const bool relaxed : {false, true}) {
        auto personal =
            business_delta_item("personal", "personal.bin", "folder");
        personal["cTag"] = "personal-content-tag";
        personal["parentReference"]["path"] = "/drive/root:/Stale";
        auto business =
            business_delta_item("business", "business.bin", "folder");
        auto legacy = business_delta_item("legacy", "legacy.bin", "");
        legacy["parentReference"] = {{"path", "/drive/root:/Legacy"}};
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                graph_page(
                    Json::array({
                                    personal,
                                    business,
                                    legacy,
                                    business_delta_item(
                                        "folder", "Current", "root", true
                                    ),
                                    {{"id", "root"},
                                     {"name", "Drive"},
                                     {"root", Json::object()}},
                                })
                        .dump(),
                    std::nullopt,
                    "https://graph.example.test/v1.0/cursor"
                ),
            }
        );
        auto* recorded = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.drive_id = "unclassified-drive",
             .endpoint = "https://graph.example.test/v1.0",
             .relaxed_download_validation = relaxed},
        };
        const auto delta = client.list_delta(std::nullopt);
        if (delta.changes.size() != 5 ||
            delta.changes[0].remote_path != "Current/personal.bin" ||
            delta.changes[1].remote_path != "Current/business.bin" ||
            delta.changes[2].remote_path != "Legacy/legacy.bin" ||
            delta.changes[0].ctag != "personal-content-tag" ||
            !delta.changes[1].ctag.empty() || !delta.changes[0].content_hash ||
            !delta.changes[1].content_hash ||
            delta.changes[0].validate_content == relaxed ||
            delta.changes[1].validate_content == relaxed ||
            recorded->queued.requests.size() != 2) {
            return fail(
                "shared Delta normalization or path strategy depended on "
                "account type"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_business_delta_paths_across_pages() {
    using Json = nlohmann::json;
    for (const bool stale_path_hint : {false, true}) {
        auto child = business_delta_item("child", "data.bin", "nested");
        if (stale_path_hint) {
            child["parentReference"]["path"] = "/drive/root:/Old/Nested";
        }
        auto transport = std::make_unique<
            FakeTransport>(std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page(
                Json::array(
                    {
                        child,
                        business_delta_item("nested", "Nested", "outer", true),
                        business_delta_item("outer", "Old", "root", true),
                    }
                )
                    .dump(),
                "https://graph.example.test/v1.0/page2"
            ),
            graph_page(
                Json::array(
                    {
                        business_delta_item("outer", "New", "root", true),
                        business_delta_item("nested", "Renamed", "outer", true),
                        {{"id", "root"},
                         {"name", "Drive"},
                         {"root", Json::object()}},
                        {{"id", "gone"}, {"deleted", Json::object()}},
                    }
                )
                    .dump(),
                std::nullopt,
                "https://graph.example.test/v1.0/cursor"
            ),
        });
        auto* recorded = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        const auto delta = client.list_delta(std::nullopt);
        if (delta.changes.size() != 5 ||
            delta.changes[0].remote_path != "New/Renamed/data.bin" ||
            delta.changes[1].remote_path != "New/Renamed" ||
            delta.changes[2].remote_path != "New" ||
            !delta.changes[0].ctag.empty() || !delta.changes[0].content_hash ||
            !delta.changes[3].root || !delta.changes[3].directory ||
            !delta.changes[3].remote_path.empty() ||
            !delta.changes[4].deleted ||
            recorded->queued.requests.size() != 3) {
            return fail(
                "Business Delta did not resolve latest cross-page parent IDs"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_business_delta_saved_and_fetched_ancestors() {
    namespace graph = onedrive::graph;
    using Json = nlohmann::json;
    for (const std::string scenario :
         {"saved", "lookup", "renamed", "package"}) {
        auto values = Json::array({
            business_delta_item("first", "first.bin", "nested"),
            business_delta_item("second", "second.bin", "nested"),
        });
        std::vector<graph::RemoteItem> known;
        if (scenario == "saved" || scenario == "renamed") {
            known = {
                {.id = "outer",
                 .name = "Old",
                 .remote_path = "Old",
                 .directory = true},
                {.id = "nested",
                 .name = "Nested",
                 .parent_id = "outer",
                 .remote_path = "Old/Nested",
                 .directory = true},
            };
        }
        if (scenario == "renamed") {
            values.push_back(business_delta_item("outer", "New", "root", true));
        }
        auto responses = std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page(
                values.dump(),
                std::nullopt,
                "https://graph.example.test/v1.0/cursor"
            ),
        };
        if (scenario == "lookup" || scenario == "package") {
            responses.push_back(json_response(
                200,
                business_delta_item("nested", "Nested", "outer", true).dump()
            ));
            auto outer = business_delta_item("outer", "Old", "root", true);
            if (scenario == "package") {
                outer.erase("folder");
                outer["package"] = {{"type", "oneNote"}};
            }
            responses.push_back(json_response(200, outer.dump()));
        }
        if (scenario != "saved") {
            responses.push_back(json_response(
                200,
                Json{
                    {"id", "root"}, {"name", "Drive"}, {"root", Json::object()}
                }.dump()
            ));
        }
        auto transport = std::make_unique<FakeTransport>(std::move(responses));
        auto* recorded = transport.get();
        graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        const auto delta = client.list_delta(
            "https://graph.example.test/v1.0/baseline", {}, {}, known
        );
        const auto prefix =
            scenario == "renamed" ? "New/Nested/" : "Old/Nested/";
        if (delta.changes[0].remote_path != std::string{prefix} + "first.bin" ||
            delta.changes[1].remote_path !=
                std::string{prefix} + "second.bin" ||
            recorded->queued.requests.size() != (scenario == "saved" ? 2U
                                                 : scenario == "renamed"
                                                     ? 3U
                                                     : 5U) ||
            (scenario == "package" &&
             (delta.changes[0].unsupported_format != "onenote" ||
              delta.changes[1].unsupported_format != "onenote" ||
              delta.changes[0].content_hash ||
              delta.changes[1].content_hash))) {
            return fail(
                "Business Delta lost cached ancestry or repeated ancestor "
                "requests"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_business_delta_moves_and_restart() {
    namespace sg = onedrive::test::sync;
    namespace storage = onedrive::storage;
    using Json = nlohmann::json;
    for (const bool interrupt : {false, true}) {
        onedrive::test::TemporaryDirectory temporary;
        auto config = sg::config_for(temporary.path() / "sync", false);
        config.download_concurrency = 1;
        const auto identity = onedrive::test::test_drive_identity(
            config.drive_id, config.drive_id
        );
        sg::FakeMetrics metrics;
        class StopBeforeExecution final : public onedrive::events::Observer {
        public:
            mutable std::stop_source stop;
            bool enabled{false};

        private:
            void on_event(const onedrive::events::Event& event) const override {
                if (const auto* message =
                        std::get_if<onedrive::events::MessageEvent>(&event);
                    enabled && message &&
                    message->event == "execution_started") {
                    stop.request_stop();
                }
            }
        } observer;
        sg::FakeGraphClient seed;
        auto outer = sg::file("outer", "Old", 0);
        outer.directory = true;
        outer.parent_id = "root";
        auto nested = sg::file("nested", "Old/Sub", 0);
        nested.directory = true;
        nested.parent_id = "outer";
        auto child = sg::file("child", "Old/Sub/data.bin", 4);
        child.parent_id = "nested";
        seed.contents["child"] = "data";
        seed.delta_override = [=](const auto&) {
            return onedrive::graph::DeltaResult{
                .changes = {outer, nested, child},
                .delta_link = "https://graph.example.test/v1.0/applied",
            };
        };
        {
            storage::ItemDatabase database{config.state_directory, identity};
            database.open();
            if (onedrive::sync::SyncEngine{
                    config, seed, database, metrics, &observer
                }
                    .synchronize() != 0) {
                return fail("cannot seed the Business directory tree");
            }
        }
        auto responses = std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page(
                Json::array(
                    {
                        business_delta_item("outer", "New", "root", true),
                        business_delta_item("nested", "Renamed", "outer", true),
                    }
                )
                    .dump(),
                std::nullopt,
                "https://graph.example.test/v1.0/fetched"
            ),
            json_response(
                200,
                Json{
                    {"id", "root"}, {"name", "Drive"}, {"root", Json::object()}
                }.dump()
            ),
        };
        if (interrupt) {
            responses.push_back(graph_page(
                "[]", std::nullopt, "https://graph.example.test/v1.0/resumed"
            ));
        }
        auto transport = std::make_unique<FakeTransport>(std::move(responses));
        auto* recorded = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        observer.enabled = interrupt;
        {
            storage::ItemDatabase database{config.state_directory, identity};
            database.open();
            const auto result =
                onedrive::sync::SyncEngine{
                    config, client, database, metrics, &observer
                }
                    .synchronize(
                        interrupt ? observer.stop.get_token()
                                  : std::stop_token{}
                    );
            if (result != (interrupt ? onedrive::exit_code::cancelled : 0) ||
                (interrupt &&
                 (!database.pending_delta(config.drive_id) ||
                  database.delta_link(config.drive_id) !=
                      "https://graph.example.test/v1.0/applied"))) {
                return fail(
                    "Business directory move lost its recovery batch or cursor"
                );
            }
        }
        storage::ItemDatabase reopened{config.state_directory, identity};
        reopened.open();
        observer.enabled = false;
        if (interrupt &&
            onedrive::sync::SyncEngine{
                config, client, reopened, metrics, &observer
            }
                    .synchronize() != 0) {
            return fail("Business directory move did not resume from SQLite");
        }
        const auto saved = reopened.find(config.drive_id, "child");
        const auto expected_cursor =
            interrupt ? "https://graph.example.test/v1.0/resumed"
                      : "https://graph.example.test/v1.0/fetched";
        if (!saved || saved->remote_path != "New/Renamed/data.bin" ||
            saved->parent_id != "nested" || !saved->ctag.empty() ||
            onedrive::test::read_file(
                config.sync_data_directory / "New/Renamed/data.bin"
            ) != "data" ||
            std::filesystem::exists(config.sync_data_directory / "Old") ||
            reopened.pending_delta(config.drive_id) ||
            reopened.delta_link(config.drive_id) != expected_cursor ||
            !recorded->download_requests.empty() ||
            recorded->queued.requests.size() != (interrupt ? 4U : 3U) ||
            (interrupt && recorded->queued.requests.back().url !=
                              "https://graph.example.test/v1.0/fetched")) {
            return fail(
                "Business nested moves or restart changed descendant data"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_delta_rejects_malformed_sizes() {
    using Json = nlohmann::json;
    for (const auto& size : Json::array({nullptr, "4", 4.5, true})) {
        auto child = business_delta_item("child", "data.bin", "root");
        child["parentReference"]["path"] = "/drive/root:";
        child["size"] = size;
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                graph_page(
                    Json::array({child}).dump(),
                    std::nullopt,
                    "https://graph.example.test/v1.0/cursor"
                ),
            }
        );
        auto* recorded = transport.get();
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] { static_cast<void>(client.list_delta(std::nullopt)); },
                "invalid size"
            ) ||
            recorded->queued.requests.size() != 2) {
            return fail("invalid Delta size was silently converted to zero");
        }
    }
    return EXIT_SUCCESS;
}

int test_delta_cached_ancestor_validation() {
    namespace graph = onedrive::graph;
    for (const std::string scenario : {"file", "deleted", "unnamed"}) {
        auto child = business_delta_item("child", "data.bin", "parent");
        child["parentReference"]["path"] = "/drive/root:/Legacy";
        std::vector<graph::RemoteItem> known{
            {.id = "parent",
             .name = "Parent",
             .parent_id = "root",
             .directory = true},
            {.id = "root", .name = "Drive", .directory = true, .root = true},
        };
        known[0].directory = scenario != "file";
        known[0].deleted = scenario == "deleted";
        if (scenario == "unnamed") {
            known[0].name.clear();
        }
        auto transport = std::make_unique<FakeTransport>(
            std::deque<onedrive::http::HttpResult>{
                token_response(),
                graph_page(
                    nlohmann::json::array({child}).dump(),
                    std::nullopt,
                    "https://graph.example.test/v1.0/cursor"
                ),
            }
        );
        auto* recorded = transport.get();
        graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        client.list_delta(std::nullopt, {}, {}, known)
                    );
                },
                scenario == "unnamed" ? "missing an item name"
                                      : "invalid delta ancestor"
            ) ||
            recorded->queued.requests.size() != 2) {
            return fail(
                "invalid cached ancestry was hidden by a legacy path hint"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_delta_injected_ancestor_failures() {
    namespace graph = onedrive::graph;
    using Resolver = graph::client_detail::DeltaHierarchyResolver;
    for (const std::string scenario :
         {"deleted", "cancel", "lookup_failure", "authentication"}) {
        std::stop_source stop;
        std::vector<std::string> looked_up;
        const auto resolve = [&] {
            static_cast<void>(Resolver{
                {{.id = "child", .name = "data.bin", .parent_id = "parent"}},
                {},
                [&](const std::string& id) -> graph::RemoteItem {
                    looked_up.push_back(id);
                    if (scenario == "lookup_failure") {
                        throw std::runtime_error{"ancestor lookup unavailable"};
                    }
                    if (scenario == "authentication") {
                        throw graph::AccountAuthenticationError{
                            "account consent revoked"
                        };
                    }
                    if (scenario == "cancel") {
                        stop.request_stop();
                    }
                    return {
                        .id = id,
                        .name = "Drive",
                        .directory = true,
                        .deleted = scenario == "deleted",
                        .root = true,
                    };
                },
                stop.get_token(),
            }
                                  .resolve());
        };
        const bool rejected =
            scenario == "cancel"
                ? onedrive::test::throws_with<graph::RequestCancelledError>(
                      resolve, "path resolution was cancelled"
                  )
            : scenario == "authentication"
                ? onedrive::test::throws_with<
                      graph::AccountAuthenticationError>(
                      resolve, "account consent revoked"
                  )
                : onedrive::test::throws_with<std::runtime_error>(
                      resolve,
                      scenario == "deleted" ? "invalid delta ancestor"
                                            : "ancestor lookup unavailable"
                  );
        if (!rejected || looked_up != std::vector<std::string>{"parent"}) {
            return fail(
                "injected ancestor failure was swallowed, retried or "
                "misclassified"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_business_delta_invalid_ancestry_and_cancellation() {
    using Json = nlohmann::json;
    for (const std::string scenario :
         {"missing",
          "bad_parent",
          "bad_parent_id",
          "cycle",
          "file_parent",
          "deleted_parent",
          "mismatched",
          "cancel"}) {
        auto child = business_delta_item("child", "data.bin", "parent");
        auto values = Json::array({child});
        if (scenario == "missing") {
            values[0].erase("parentReference");
        } else if (scenario == "bad_parent") {
            values[0]["parentReference"] = 42;
        } else if (scenario == "bad_parent_id") {
            values[0]["parentReference"]["id"] = 42;
        } else if (scenario == "cycle") {
            values.push_back(
                business_delta_item("parent", "Parent", "child", true)
            );
            values[0]["folder"] = Json::object();
        }
        auto responses = std::deque<onedrive::http::HttpResult>{
            token_response(),
            graph_page(
                values.dump(),
                std::nullopt,
                "https://graph.example.test/v1.0/cursor"
            ),
        };
        const bool missing_parent = scenario == "missing" ||
                                    scenario == "bad_parent" ||
                                    scenario == "bad_parent_id";
        if (!missing_parent && scenario != "cycle") {
            auto parent = business_delta_item(
                scenario == "mismatched" ? "other" : "parent",
                "Parent",
                "root",
                scenario != "file_parent"
            );
            if (scenario == "deleted_parent") {
                parent["deleted"] = Json::object();
            }
            responses.push_back(json_response(200, parent.dump()));
        }
        std::stop_source stop;
        auto transport = std::make_unique<FakeTransport>(std::move(responses));
        auto* recorded = transport.get();
        if (scenario == "cancel") {
            recorded->on_perform = [&](const auto& request) {
                if (request.url.contains("/items/parent")) {
                    stop.request_stop();
                }
            };
        }
        onedrive::graph::MicrosoftGraphClient client{
            wrap_transport(std::move(transport)),
            wrap_token_store(std::make_unique<FakeTokenStore>("refresh")),
            auth_options(),
            {.endpoint = "https://graph.example.test/v1.0"},
        };
        try {
            static_cast<void>(
                client.list_delta(std::nullopt, {}, stop.get_token())
            );
            return fail(
                "invalid or cancelled Business ancestry produced a Delta cursor"
            );
        } catch (const onedrive::graph::RequestCancelledError&) {
            if (scenario != "cancel" || recorded->queued.requests.size() != 3) {
                return fail("Business ancestry cancellation was misclassified");
            }
        } catch (const std::runtime_error& error) {
            const std::string_view message{error.what()};
            if (scenario == "cancel" ||
                !(message.contains("parent ID") || message.contains("cyclic") ||
                  message.contains("invalid delta ancestor") ||
                  message.contains("invalid ancestry metadata")) ||
                recorded->queued.requests.size() !=
                    (missing_parent || scenario == "cycle" ? 2U : 3U)) {
                return fail(
                    "Business ancestry error was hidden or issued extra "
                    "requests"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

} // namespace onedrive::test::graph::delta_cases
