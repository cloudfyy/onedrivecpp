#include "support.hpp"
#include "onedrive/exit_code.hpp"

#include "onedrive/app/synchronization.hpp"
#include "onedrive/ui/common/observer.hpp"
#include "onedrive/sync/core/engine.hpp"

#include <algorithm>
#include <map>
#include <spdlog/spdlog.h>
#include <stop_token>
#include <vector>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::app;

int test_multiple_drives(const onedrive::events::Observer& observer) {
    for (const std::string scenario :
         {"success",
          "drive_failure",
          "authentication",
          "alias",
          "cancel",
          "cache_targets",
          "delta_failure",
          "delta_authentication",
          "different_account"}) {
        TemporaryDirectory temporary;
        auto config = onedrive::config::Config::defaults();
        config.state_directory = temporary.path() / "state";
        config.sync_data_directory = temporary.path() / "sync";
        config.drive_ids = {"first", "second"};
        if (scenario == "different_account") {
            config.drive_ids.push_back("third");
        }
        config.dry_run = true;
        std::filesystem::create_directories(config.state_directory);
        std::filesystem::create_directories(config.sync_data_directory);
        std::vector<std::string> calls;
        std::stop_source stop;
        FakeRuntimeFactory implementation;
        implementation.graph_client_override = [&](const auto& selected) {
            const auto id = selected.drive_id;
            return std::make_unique<onedrive::graph::GraphClient>(
                std::in_place_type<FakeGraphClient>,
                id,
                false,
                false,
                false,
                [&, id](std::stop_token) {
                    calls.push_back("identity:" + id);
                    if ((id == "first" && scenario == "drive_failure") ||
                        id.starts_with("missing")) {
                        throw std::runtime_error{"Drive unavailable"};
                    }
                    if (id == "first" && scenario == "authentication") {
                        throw onedrive::graph::AccountAuthenticationError{
                            "account consent revoked"
                        };
                    }
                    auto identity = onedrive::test::test_drive_identity(id);
                    identity.drive_id = scenario == "alias" ? "same" : id;
                    if (id == "second" && scenario == "different_account") {
                        identity.user_id = "another-user";
                    }
                    return identity;
                },
                [&, id](const std::optional<std::string>&, std::stop_token) {
                    calls.push_back("delta:" + id);
                    if (id == "first" && scenario == "delta_failure") {
                        throw std::runtime_error{"delta failed for this Drive"};
                    }
                    if (id == "first" && scenario == "delta_authentication") {
                        throw onedrive::graph::AccountAuthenticationError{
                            "account token expired during Delta"
                        };
                    }
                    if (scenario == "cancel") {
                        stop.request_stop();
                    }
                    return onedrive::graph::DeltaResult{
                        .changes = {}, .delta_link = "cursor-" + id
                    };
                }
            );
        };
        const onedrive::app::RuntimeFactory factory{
            onedrive::util::borrowed_proxy, implementation
        };
        int result = exit_code::failure;
        onedrive::sync::PreviewCache cache;
        try {
            result = onedrive::app::synchronize_account(
                config, factory, observer, stop.get_token(), &cache
            );
            if (scenario == "authentication" ||
                scenario == "delta_authentication" ||
                scenario == "different_account") {
                return fail("account error did not stop the multi-drive round");
            }
        } catch (const onedrive::graph::AccountAuthenticationError&) {
            const auto expected =
                scenario == "different_account"
                    ? std::vector<
                          std::
                              string>{"identity:first", "delta:first", "identity:second"}
                : scenario == "authentication"
                    ? std::vector<std::string>{"identity:first"}
                    : std::vector<std::string>{"identity:first", "delta:first"};
            if ((scenario != "authentication" &&
                 scenario != "delta_authentication" &&
                 scenario != "different_account") ||
                calls != expected) {
                return fail("account failure continued to another Drive");
            }
            continue;
        }
        if (scenario == "cache_targets") {
            if (result != exit_code::success || !cache.available()) {
                return fail(
                    "multi-drive preview did not retain per-target metadata"
                );
            }
            config.drive_ids = {"missing-first", "missing-second"};
            if (onedrive::app::synchronize_account(
                    config, factory, observer, {}, &cache
                ) != exit_code::failure ||
                cache.available()) {
                return fail(
                    "removed or failed Drive previews remained available"
                );
            }
        }
        if (scenario == "success" &&
            (result != exit_code::success ||
             calls !=
                 std::vector<std::string>{
                     "identity:first",
                     "delta:first",
                     "identity:second",
                     "delta:second"
                 } ||
             implementation.item_store_sync_directories.size() != 2 ||
             implementation.item_store_sync_directories[0] ==
                 implementation.item_store_sync_directories[1])) {
            return fail(
                "multi-drive synchronization was not ordered and isolated"
            );
        }
        if (scenario == "drive_failure" &&
            (result != exit_code::failure ||
             calls != std::vector<std::string>{
                          "identity:first", "identity:second", "delta:second"
                      })) {
            return fail(
                "one Drive failure prevented other Drives from synchronizing"
            );
        }
        if (scenario == "delta_failure" &&
            (result != exit_code::failure || calls != std::vector<std::string>{
                                                          "identity:first",
                                                          "delta:first",
                                                          "identity:second",
                                                          "delta:second"
                                                      })) {
            return fail(
                "a Drive Delta failure prevented remaining targets from running"
            );
        }
        if (scenario == "alias" &&
            (result != exit_code::failure ||
             std::ranges::count(calls, "delta:first") != 1 ||
             std::ranges::count(calls, "delta:second") != 0)) {
            return fail("Drive aliases synchronized the same target twice");
        }
        if (scenario == "cancel" &&
            (result != exit_code::cancelled ||
             std::ranges::find(calls, "identity:second") != calls.end())) {
            return fail("multi-drive cancellation started another target");
        }
    }
    return EXIT_SUCCESS;
}

class RecordingObserver final : public onedrive::events::Observer {
public:
    explicit RecordingObserver(
        std::function<void(const onedrive::events::Event&)> callback = {}
    )
        : callback_{std::move(callback)} {
    }
    [[nodiscard]] std::vector<onedrive::events::OperationState>
    operation_states() const {
        std::vector<onedrive::events::OperationState> states;
        for (const auto& event : events) {
            if (const auto* state =
                    std::get_if<onedrive::events::OperationStateEvent>(
                        &event
                    )) {
                states.push_back(state->state);
            }
        }
        return states;
    }

    [[nodiscard]] bool has_delta_summary() const {
        return std::ranges::any_of(events, [](const auto& event) {
            return std::holds_alternative<
                onedrive::events::DeltaSummaryEvent
            >(event);
        });
    }

    [[nodiscard]] std::map<std::string, std::string>
    fields(std::string_view name) const {
        std::map<std::string, std::string> result;
        for (const auto& event : events) {
            const auto* section =
                std::get_if<onedrive::events::SectionEvent>(&event);
            if (section && section->event == name) {
                for (const auto& field : section->fields) {
                    result[field.key] = field.value;
                }
            }
        }
        return result;
    }

protected:
    void on_event(const onedrive::events::Event& event) const override {
        events.push_back(event);
        if (callback_) {
            callback_(event);
        }
    }

private:
    mutable std::vector<onedrive::events::Event> events;
    std::function<void(const onedrive::events::Event&)> callback_;
};

int test_multiple_drive_result_priority() {
    for (const bool blocked_first : {false, true}) {
        for (const bool other_fails : {false, true}) {
            TemporaryDirectory temporary;
            auto config = onedrive::config::Config::defaults();
            config.state_directory = temporary.path() / "state";
            config.sync_data_directory = temporary.path() / "sync";
            config.drive_ids = {"first", "second"};
            config.dry_run = true;
            std::filesystem::create_directories(config.state_directory);
            std::filesystem::create_directories(config.sync_data_directory);
            FakeRuntimeFactory implementation;
            std::vector<std::string> visited;
            implementation.graph_client_override = [&](const auto& selected) {
                const auto id = selected.drive_id;
                const bool blocked = (id == "first") == blocked_first;
                return std::make_unique<onedrive::graph::GraphClient>(
                    std::in_place_type<FakeGraphClient>,
                    id,
                    false,
                    false,
                    false,
                    [id](std::stop_token) {
                        return onedrive::test::test_drive_identity(id, id);
                    },
                    [&, id, blocked](const auto&, std::stop_token) {
                        visited.push_back(id);
                        if (!blocked && other_fails) {
                            throw std::runtime_error{"Drive unavailable"};
                        }
                        onedrive::graph::DeltaResult delta{
                            .changes = {}, .delta_link = "cursor-" + id
                        };
                        if (blocked) {
                            delta.changes.push_back({
                                .id = "notes",
                                .name = "notes.one",
                                .remote_path = "notes.one",
                                .unsupported_format = "onenote",
                            });
                        }
                        return delta;
                    }
                );
            };
            const onedrive::app::RuntimeFactory factory{
                onedrive::util::borrowed_proxy, implementation
            };
            RecordingObserver observer;
            const auto result =
                onedrive::app::synchronize_account(config, factory, observer);
            const std::map<std::string, std::string> expected{
                {"completed", other_fails ? "0" : "1"},
                {"failed", other_fails ? "1" : "0"},
                {"blocked", "1"},
            };
            if (result !=
                    (other_fails ? exit_code::failure : exit_code::blocked) ||
                visited != std::vector<std::string>{"first", "second"} ||
                observer.fields("drive_sync_summary") != expected) {
                return fail(
                    "multi-drive result/summary lost failure > blocked > "
                    "success priority"
                );
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_multiple_drive_preview_lifecycle() {
    for (const std::string outcome : {"apply", "authentication", "cancel"}) {
        TemporaryDirectory temporary;
        auto config = onedrive::config::Config::defaults();
        config.state_directory = temporary.path() / "state";
        config.sync_data_directory = temporary.path() / "sync";
        config.drive_ids = {"first", "second", "third"};
        config.dry_run = true;
        std::filesystem::create_directories(config.state_directory);
        std::filesystem::create_directories(config.sync_data_directory);
        FakeRuntimeFactory implementation;
        implementation.use_real_item_store = true;
        bool preview = true;
        using Query = std::pair<std::string, std::optional<std::string>>;
        std::vector<Query> queries;
        std::vector<std::string> resolved;
        implementation.graph_client_override = [&](const auto& selected) {
            const auto id = selected.drive_id;
            return std::make_unique<onedrive::graph::GraphClient>(
                std::in_place_type<FakeGraphClient>,
                id,
                false,
                false,
                false,
                [&, id](std::stop_token) {
                    if (!preview) {
                        resolved.push_back(id);
                        if (id == "second" && outcome == "authentication") {
                            throw onedrive::graph::AccountAuthenticationError{
                                "account token revoked"
                            };
                        }
                        if (id == "second" && outcome == "cancel") {
                            throw onedrive::graph::RequestCancelledError{
                                "Drive identity request cancelled"
                            };
                        }
                    }
                    return onedrive::test::test_drive_identity(id, id);
                },
                [&, id](const auto& cursor, std::stop_token) {
                    queries.emplace_back(id, cursor);
                    return onedrive::graph::DeltaResult{
                        .changes = {},
                        .delta_link = (preview ? "preview-" : "applied-") + id
                    };
                }
            );
        };
        const onedrive::app::RuntimeFactory factory{
            onedrive::util::borrowed_proxy, implementation
        };
        RecordingObserver observer;
        onedrive::sync::PreviewCache cache;
        if (onedrive::app::synchronize_account(
                config, factory, observer, {}, &cache
            ) != exit_code::success ||
            !cache.available()) {
            return fail(
                "cannot populate multi-drive preview lifecycle fixture"
            );
        }
        preview = false;
        config.dry_run = false;
        int result = exit_code::failure;
        try {
            result = onedrive::app::synchronize_account(
                config, factory, observer, {}, &cache
            );
            if (outcome == "authentication") {
                return fail("authentication failure accepted a cached preview");
            }
        } catch (const onedrive::graph::AccountAuthenticationError&) {
            if (outcome != "authentication") {
                throw;
            }
        }
        std::vector<Query> expected_queries{
            {"first", std::nullopt},
            {"second", std::nullopt},
            {"third", std::nullopt},
            {"first", "preview-first"},
        };
        if (outcome == "apply") {
            expected_queries.emplace_back("second", "preview-second");
            expected_queries.emplace_back("third", "preview-third");
        }
        if (cache.available() || queries != expected_queries ||
            resolved !=
                (outcome == "apply"
                     ? std::vector<std::string>{"first", "second", "third"}
                     : std::vector<std::string>{"first", "second"}) ||
            (outcome == "cancel" && result != exit_code::cancelled) ||
            (outcome == "apply" && result != exit_code::success)) {
            return fail(
                "multi-drive preview reuse or fatal cleanup was not isolated"
            );
        }
        if (outcome == "apply") {
            for (const auto& id : config.drive_ids) {
                const auto identity =
                    onedrive::test::test_drive_identity(id, id);
                auto items = implementation.create_item_store(config, identity);
                items->open_read_only();
                if (items->delta_link(id) != "applied-" + id) {
                    return fail(
                        "a Drive applied another target's preview cursor"
                    );
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

int test_multiple_drives_observer_failure() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";
    config.drive_ids = {"first", "second"};
    config.dry_run = true;
    std::filesystem::create_directories(config.state_directory);
    std::filesystem::create_directories(config.sync_data_directory);
    std::vector<std::string> identities;
    FakeRuntimeFactory implementation;
    implementation.identity_override = [&](std::stop_token) {
        identities.push_back("resolved");
        return onedrive::test::test_drive_identity("first");
    };
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer{[](const onedrive::events::Event& event) {
        if (std::holds_alternative<onedrive::events::DeltaProgressEvent>(
                event
            )) {
            throw std::runtime_error{"frontend callback failed"};
        }
    }};
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::app::synchronize_account(
                    config, factory, observer
                ));
            },
            "frontend callback failed"
        ) ||
        identities.size() != 1) {
        return fail(
            "multi-drive recovery swallowed a frontend callback failure"
        );
    }
    return EXIT_SUCCESS;
}

int test_multiple_drive_cancellation_boundaries() {
    for (const bool cancel_during_identity : {false, true}) {
        TemporaryDirectory temporary;
        auto config = onedrive::config::Config::defaults();
        config.state_directory = temporary.path() / "state";
        config.sync_data_directory = temporary.path() / "sync";
        config.drive_ids = {"first", "second"};
        config.dry_run = true;
        std::filesystem::create_directories(config.state_directory);
        std::filesystem::create_directories(config.sync_data_directory);
        FakeRuntimeFactory implementation;
        std::stop_source stop;
        int identities = 0;
        int deltas = 0;
        implementation.identity_override = [&](std::stop_token) {
            ++identities;
            if (cancel_during_identity) {
                stop.request_stop();
            }
            return onedrive::test::test_drive_identity("first");
        };
        implementation.delta_override = [&](const auto&, std::stop_token) {
            ++deltas;
            return onedrive::graph::DeltaResult{
                .changes = {}, .delta_link = "cursor-first"
            };
        };
        const onedrive::app::RuntimeFactory factory{
            onedrive::util::borrowed_proxy, implementation
        };
        RecordingObserver observer{[&](const onedrive::events::Event& event) {
            const auto* section =
                std::get_if<onedrive::events::SectionEvent>(&event);
            if (section && section->event == "drive_sync_result") {
                stop.request_stop();
            }
        }};
        onedrive::sync::PreviewCache cache;
        if (onedrive::app::synchronize_account(
                config, factory, observer, stop.get_token(), &cache
            ) != exit_code::cancelled ||
            cache.available() || identities != 1 ||
            deltas != (cancel_during_identity ? 0 : 1) ||
            observer.operation_states() !=
                std::vector<onedrive::events::OperationState>{
                    onedrive::events::OperationState::syncing,
                    onedrive::events::OperationState::stopping,
                    onedrive::events::OperationState::ready,
                }) {
            return fail(
                "cancellation crossed a Drive preparation/round boundary"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_synchronization_service() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";
    config.dry_run = true;
    std::filesystem::create_directories(config.state_directory);
    std::filesystem::create_directories(config.sync_data_directory);

    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;

    const auto result = onedrive::app::synchronize_account(
        config, runtime_factory, observer
    );
    const auto identity = onedrive::test::test_drive_identity("me");
    const auto expected_sync_directory =
        onedrive::account::AccountState::drive_data_directory(
            config.sync_data_directory, identity
        );
    if (result != exit_code::success || !observer.has_delta_summary() ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::syncing,
                onedrive::events::OperationState::ready,
            } ||
        implementation.graph_client_count != 1 ||
        implementation.item_store_count != 1 ||
        implementation.item_store_open_count != 1 ||
        implementation.metrics_count != 1 ||
        implementation.last_item_store_sync_directory !=
            expected_sync_directory) {
        return fail(
            "synchronization service did not prepare and run the sync "
            "through its injected dependencies"
        );
    }
    return EXIT_SUCCESS;
}

int test_synchronization_service_cancellation() {
    TemporaryDirectory temporary;
    auto config = onedrive::config::Config::defaults();
    config.state_directory = temporary.path() / "state";
    config.sync_data_directory = temporary.path() / "sync";

    FakeRuntimeFactory implementation;
    const onedrive::app::RuntimeFactory runtime_factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;
    std::stop_source stop;
    stop.request_stop();

    const auto result = onedrive::app::synchronize_account(
        config, runtime_factory, observer, stop.get_token()
    );
    if (result != exit_code::cancelled ||
        implementation.graph_client_count != 0 ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::syncing,
                onedrive::events::OperationState::stopping,
                onedrive::events::OperationState::ready,
            } ||
        implementation.item_store_count != 0 ||
        implementation.metrics_count != 0) {
        return fail("synchronization service did not preserve cancellation");
    }
    return EXIT_SUCCESS;
}

int test_identity_cancellation() {
    for (const bool throws : {false, true}) {
        auto config = onedrive::config::Config::defaults();
        FakeRuntimeFactory implementation;
        std::stop_source stop;
        bool forwarded = false;
        implementation.identity_override = [&](std::stop_token token) {
            forwarded = token == stop.get_token();
            stop.request_stop();
            if (throws) {
                throw onedrive::graph::RequestCancelledError{
                    "identity cancelled"
                };
            }
            return onedrive::test::test_drive_identity("me");
        };
        const onedrive::app::RuntimeFactory factory{
            onedrive::util::borrowed_proxy, implementation
        };
        RecordingObserver observer;
        if (onedrive::app::synchronize_account(
                config, factory, observer, stop.get_token()
            ) != exit_code::cancelled ||
            !forwarded || implementation.graph_client_count != 1 ||
            implementation.item_store_count != 0 ||
            implementation.metrics_count != 0 ||
            observer.operation_states() !=
                std::vector<onedrive::events::OperationState>{
                    onedrive::events::OperationState::syncing,
                    onedrive::events::OperationState::stopping,
                    onedrive::events::OperationState::ready,
                }) {
            return fail(
                "identity cancellation created stores or failed to return "
                "cancelled"
            );
        }
    }
    FakeRuntimeFactory implementation;
    implementation.identity_override =
        [](std::stop_token) -> onedrive::account::DriveIdentity {
        throw std::runtime_error{"identity failed"};
    };
    const onedrive::app::RuntimeFactory factory{
        onedrive::util::borrowed_proxy, implementation
    };
    RecordingObserver observer;
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::app::synchronize_account(
                    onedrive::config::Config::defaults(), factory, observer
                ));
            },
            "identity failed"
        ) ||
        observer.operation_states() !=
            std::vector<onedrive::events::OperationState>{
                onedrive::events::OperationState::syncing,
                onedrive::events::OperationState::failed,
            }) {
        return fail("identity failure was hidden or treated as cancellation");
    }
    return EXIT_SUCCESS;
}

int test_populated_cache_cleanup() {
    namespace od = onedrive;
    using State = od::events::OperationState;
    for (const int failure_stage : {0, 1, 2, 3, 4, 5}) {
        TemporaryDirectory temporary;
        auto config = od::config::Config::defaults();
        config.state_directory = temporary.path() / "state";
        config.sync_data_directory = temporary.path() / "sync";
        config.dry_run = true;
        FakeRuntimeFactory implementation;
        const od::app::RuntimeFactory factory{
            od::util::borrowed_proxy, implementation
        };
        od::sync::PreviewCache cache;
        RecordingObserver seed_observer;
        if (od::app::synchronize_account(
                config, factory, seed_observer, {}, &cache
            ) != exit_code::success ||
            !cache.available()) {
            return fail("cannot seed application preview cache");
        }
        const auto graph_count = implementation.graph_client_count;
        const auto store_count = implementation.item_store_count;
        const auto metrics_count = implementation.metrics_count;
        std::stop_source stop;
        if (failure_stage == 0) {
            stop.request_stop();
        } else if (failure_stage == 1 || failure_stage == 2) {
            implementation.identity_override = [&](std::stop_token token) {
                if (token != stop.get_token()) {
                    throw std::logic_error{
                        "identity did not receive caller stop token"
                    };
                }
                stop.request_stop();
                if (failure_stage == 2) {
                    throw od::graph::RequestCancelledError{
                        "identity cancelled"
                    };
                }
                return od::test::test_drive_identity("me");
            };
        } else if (failure_stage == 3) {
            implementation.identity_override =
                [](std::stop_token) -> od::account::DriveIdentity {
                throw std::runtime_error{
                    "identity failed with populated cache"
                };
            };
        } else if (failure_stage == 4) {
            implementation.delta_override = [&](const auto&, std::stop_token) {
                stop.request_stop();
                return od::graph::DeltaResult{};
            };
        }
        RecordingObserver observer{[&](const od::events::Event& event) {
            const auto* state =
                std::get_if<od::events::OperationStateEvent>(&event);
            if (failure_stage == 5 && state && state->state == State::syncing) {
                throw std::runtime_error{"startup observer failed"};
            }
        }};
        const bool failure = failure_stage == 3 || failure_stage == 5;
        if (failure) {
            if (!od::test::throws_with<std::runtime_error>(
                    [&] {
                        static_cast<void>(od::app::synchronize_account(
                            config, factory, observer, stop.get_token(), &cache
                        ));
                    },
                    failure_stage == 3 ? "identity failed with populated cache"
                                       : "startup observer failed"
                ) ||
                observer.operation_states() !=
                    (failure_stage == 5
                         ? std::vector<State>{State::syncing}
                         : std::vector<State>{State::syncing, State::failed})) {
                return fail(
                    "application cache failure was hidden or state was not "
                    "failed"
                );
            }
        } else if (od::app::synchronize_account(
                       config, factory, observer, stop.get_token(), &cache
                   ) != exit_code::cancelled ||
                   observer.operation_states() !=
                       std::vector<State>{
                           State::syncing, State::stopping, State::ready
                       }) {
            return fail(
                "application cache cancellation changed lifecycle semantics"
            );
        }
        if (cache.available() ||
            implementation.graph_client_count !=
                graph_count +
                    (failure_stage == 0 || failure_stage == 5 ? 0 : 1) ||
            implementation.item_store_count !=
                store_count + (failure_stage == 4 ? 1 : 0) ||
            implementation.metrics_count !=
                metrics_count + (failure_stage == 4 ? 1 : 0)) {
            return fail(
                "early cancellation/failure retained a cache or created "
                "unexpected dependencies"
            );
        }
    }
    return EXIT_SUCCESS;
}

int test_cli_pending_delta_restart() {
    CliFixture fixture;
    fixture.runtime_factory.use_real_item_store = true;
    // CLI shutdown clears the logger; direct database inspection still needs
    // it.
    const auto logger = spdlog::default_logger();
    const auto authenticated = fixture.authenticate();
    spdlog::set_default_logger(logger);
    if (authenticated.exit_code != exit_code::success) {
        return fail("cannot initialize CLI restart account fixture");
    }
    auto raw = FakeGraphClient{"me"}.list_delta(std::nullopt, {}, {});
    const auto fetched = raw.delta_link;
    std::vector<std::optional<std::string>> requests;
    fixture.runtime_factory.delta_override = [&](const auto& cursor,
                                                 std::stop_token) {
        requests.push_back(cursor);
        if (requests.size() == 1) {
            auto interrupted = raw;
            interrupted.changes.front().size = 43;
            return interrupted;
        }
        raw.delta_link = "https://graph.example.test/latest";
        return raw;
    };
    const auto run = [&] {
        auto result = run_application(
            fixture.runtime_factory,
            {"onedrive-cpp",
             "transfer",
             "sync",
             "--config",
             fixture.config_path.string(),
             "--output",
             "json"}
        );
        spdlog::set_default_logger(logger);
        return result;
    };
    const auto first = run();
    if (first.exit_code != exit_code::failure || requests.size() != 1) {
        return fail(
            "CLI fixture did not interrupt after fetching metadata: " +
            first.standard_error + first.standard_output
        );
    }
    const auto config = onedrive::config::Config::load(fixture.config_path);
    const auto identity = onedrive::test::test_drive_identity("me");
    {
        auto store =
            fixture.runtime_factory.create_item_store(config, identity);
        store->open();
        if (!store->pending_delta(identity.drive_id) ||
            store->delta_link(identity.drive_id)) {
            return fail("interrupted CLI did not preserve pending metadata");
        }
    }
    const auto resumed = run();
    const auto destination =
        onedrive::account::AccountState::drive_data_directory(
            fixture.sync_path, identity
        ) /
        "notes.txt";
    if (resumed.exit_code != exit_code::success ||
        requests !=
            std::vector<std::optional<std::string>>{std::nullopt, fetched} ||
        !resumed.standard_output.contains("pending_delta_resumed") ||
        onedrive::test::read_file(destination) != std::string(42, 'x')) {
        return fail(
            "recreated CLI application repeated the initial delta query"
        );
    }
    auto store = fixture.runtime_factory.create_item_store(config, identity);
    store->open();
    if (store->pending_delta(identity.drive_id) ||
        store->delta_link(identity.drive_id) != raw.delta_link) {
        return fail(
            "CLI completion did not commit and clear the pending batch"
        );
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    RecordingObserver multi_drive_observer;
    if (test_multiple_drives(multi_drive_observer) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    if (test_multiple_drive_result_priority() != EXIT_SUCCESS ||
        test_multiple_drive_preview_lifecycle() != EXIT_SUCCESS ||
        test_multiple_drive_cancellation_boundaries() != EXIT_SUCCESS ||
        test_multiple_drives_observer_failure() != EXIT_SUCCESS ||
        test_synchronization_service() != EXIT_SUCCESS ||
        test_synchronization_service_cancellation() != EXIT_SUCCESS ||
        test_identity_cancellation() != EXIT_SUCCESS ||
        test_populated_cache_cleanup() != EXIT_SUCCESS ||
        test_cli_pending_delta_restart() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
