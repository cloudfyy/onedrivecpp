#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"

namespace onedrive::test::gui_sync {

void test_controller() {
    Fixture fixture;
    const auto ui_thread = std::this_thread::get_id();
    const auto logger = spdlog::default_logger();
    std::atomic<bool> worker_done{};
    bool worker_valid = false;
    gui::SyncController controller{[&](config::Config active,
                                       const events::Observer& observer,
                                       std::stop_token) {
        worker_valid = std::this_thread::get_id() != ui_thread &&
                       active.dry_run && !active.force_large_delete &&
                       test::throws_with<std::runtime_error>(
                           [&] {
                               const app::detail::RuntimePreflight busy{
                                   active, app::detail::Operation::logout
                               };
                           },
                           "already using this state directory"
                       );
        for (int index = 0; index < 1000; ++index) {
            observer.message(
                events::MessageKind::information, "test", std::to_string(index)
            );
        }
        for (std::uint64_t index = 0; index < 10000; ++index) {
            observer.download_progress(
                0, 1, index, 10000, util::ProgressState::ongoing
            );
        }
        worker_done = true;
        return exit_code::success;
    }};
    bool finished = false;
    bool delivered_on_ui = true;
    std::size_t count = 0;
    std::size_t omitted = 0;
    std::uint64_t latest_progress = 0;
    QObject::connect(
        &controller,
        &gui::SyncController::events_ready,
        [&](const auto& batch, std::size_t dropped) {
            delivered_on_ui =
                delivered_on_ui && std::this_thread::get_id() == ui_thread;
            count += batch.size();
            omitted += dropped;
            for (const auto& event : batch) {
                if (const auto* progress =
                        std::get_if<events::DownloadProgressEvent>(&event)) {
                    latest_progress = progress->downloaded;
                }
            }
        }
    );
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int result, const QString& error, bool preview) {
            require(
                result == exit_code::success && error.isEmpty() && preview &&
                    !controller.running(),
                "controller returned an incorrect result"
            );
            finished = true;
        }
    );
    controller.start(fixture.file, true);
    require(
        test::throws_with<std::logic_error>(
            [&] { controller.invalidate_preview(); }, "during synchronization"
        ),
        "controller allowed UI invalidation while worker owns the cache"
    );
    require(
        test::throws_with<std::logic_error>(
            [&] { controller.start(fixture.file, false); }, "already running"
        ),
        "controller accepted concurrent tasks"
    );
    QElapsedTimer timer;
    timer.start();
    while (!worker_done && timer.elapsed() < 5000) {
        QThread::msleep(1);
    }
    require(
        worker_done && wait_until([&] { return finished; }),
        "controller did not finish its background task"
    );
    require(
        worker_valid && delivered_on_ui && count == 500 && omitted == 501 &&
            latest_progress == 9999 && spdlog::default_logger() == logger,
        "controller lost its lock, event bound, latest progress or logger"
    );
}

void test_default_controller_without_usable_credentials() {
    Fixture fixture;
    // Nonempty on disk passes preflight; token loading trims it to empty
    // and fails before any HTTP request.
    test::write_file(fixture.token, " \t\n");
    gui::SyncController controller;
    bool finished = false;
    QString detail;
    int result = exit_code::success;
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int code, const QString& error, bool preview) {
            require(preview, "default controller lost preview mode");
            result = code;
            detail = error;
            finished = true;
        }
    );
    controller.start(fixture.file, true);
    require(
        wait_until([&] { return finished; }) && result == exit_code::failure &&
            detail.contains("no saved Microsoft authentication is available") &&
            !controller.running(),
        "default production synchronization did not expose local credential "
        "failure"
    );
}

void test_controller_lifecycle() {
    Fixture fixture;
    std::atomic<int> entered{};
    auto synchronize =
        [&](config::Config, const events::Observer&, std::stop_token token) {
            ++entered;
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        };
    gui::SyncController controller{synchronize};
    int completed = 0;
    QObject::connect(
        &controller,
        &gui::SyncController::finished,
        [&](int result, const QString& error, bool) {
            require(
                result == exit_code::cancelled && error.isEmpty(),
                "controller cancellation was reported as failure"
            );
            ++completed;
        }
    );
    for (int attempt = 1; attempt <= 2; ++attempt) {
        controller.start(fixture.file, false);
        require(
            wait_until([&] { return entered == attempt; }),
            "restarted task did not run"
        );
        controller.cancel();
        require(
            wait_until([&] { return completed == attempt; }) &&
                !controller.running(),
            "controller did not join a cancelled task before restart"
        );
        const app::detail::RuntimePreflight unlocked{
            fixture.config, app::detail::Operation::logout
        };
    }
    controller.start(fixture.file, false);
    controller.cancel();
    require(
        wait_until([&] { return completed == 3; }),
        "immediate cancellation did not finish"
    );
    const auto expected = entered + 1;
    const auto logger = spdlog::default_logger();
    {
        gui::SyncController destroyed{synchronize};
        destroyed.start(fixture.file, false);
        require(
            wait_until([&] { return entered == expected; }),
            "destructor test did not enter its task"
        );
    }
    QCoreApplication::processEvents();
    require(
        spdlog::default_logger() == logger,
        "destructor did not restore the logger"
    );
    const app::detail::RuntimePreflight unlocked{
        fixture.config, app::detail::Operation::logout
    };
}

void test_controller_preview_cache() {
    for (const int invalidation : {0, 1, 2, 3, 4, 5}) {
        Fixture fixture;
        test::app::FakeRuntimeFactory implementation;
        implementation.use_real_item_store = true;
        const app::RuntimeFactory factory{util::borrowed_proxy, implementation};
        const auto initial =
            test::app::FakeGraphClient{"me"}.list_delta(std::nullopt, {}, {});
        std::vector<std::optional<std::string>> requests;
        implementation.delta_override = [&](const auto& cursor,
                                            std::stop_token) {
            requests.push_back(cursor);
            if (cursor == initial.delta_link) {
                return graph::DeltaResult{
                    .changes = {},
                    .delta_link = "https://graph.example.test/after-preview",
                };
            }
            return initial;
        };
        gui::SyncController controller{[&](config::Config active,
                                           const events::Observer& observer,
                                           std::stop_token token,
                                           sync::PreviewCache* cache) {
            return app::synchronize_account(
                std::move(active), factory, observer, token, cache
            );
        }};
        bool finished = false;
        std::vector<std::string> messages;
        QObject::connect(
            &controller,
            &gui::SyncController::events_ready,
            [&](const auto& batch, std::size_t) {
                for (const auto& event : batch) {
                    if (const auto* message =
                            std::get_if<events::MessageEvent>(&event)) {
                        messages.push_back(message->event);
                    }
                }
            }
        );
        QObject::connect(
            &controller,
            &gui::SyncController::finished,
            [&](int result, const QString& error, bool) {
                require(
                    result == 0 && error.isEmpty(), "cached GUI task failed"
                );
                finished = true;
            }
        );
        const auto run = [&](bool preview) {
            finished = false;
            controller.start(fixture.file, preview);
            require(
                wait_until([&] { return finished; }),
                "cached task did not finish"
            );
        };
        const auto destination =
            account::AccountState::drive_data_directory(
                fixture.config.sync_data_directory, fixture.identity
            ) /
            "notes.txt";
        run(true);
        auto store =
            implementation.create_item_store(fixture.config, fixture.identity);
        store->open();
        require(
            requests == std::vector<std::optional<std::string>>{std::nullopt} &&
                !store->delta_link(fixture.identity.drive_id) &&
                store->drive_items(fixture.identity.drive_id).empty() &&
                !std::filesystem::exists(destination),
            "preview persisted its cursor, item state or files"
        );
        if (invalidation == 1) {
            test::write_file(
                fixture.file,
                test::read_file(fixture.file) + "\n# saved change\n"
            );
        } else if (invalidation == 2) {
            controller.invalidate_preview();
        } else if (invalidation == 3) {
            auto identity = fixture.identity;
            identity.user_id = "other-account";
            identity.user_display_name = "Other Account";
            implementation.identity_override = [identity](std::stop_token) {
                return identity;
            };
        } else if (invalidation == 4) {
            store->apply_delta({
                .drive_id = fixture.identity.drive_id,
                .delta_link = "https://graph.example.test/external-sync",
                .sync_filter_fingerprint = "external-filter",
            });
        } else if (invalidation == 5) {
            const auto alternate = fixture.root.path() / "alternate.toml";
            test::write_file(alternate, test::read_file(fixture.file));
            fixture.file = alternate;
        }
        store.reset();
        run(false);
        require(
            requests.size() == 2 &&
                requests.back() ==
                    (invalidation == 0
                         ? std::optional<std::string>{initial.delta_link}
                         : std::nullopt),
            "GUI repeated a full query or reused invalid "
            "configuration/account/state"
        );
        if (invalidation != 3) {
            store = implementation.create_item_store(
                fixture.config, fixture.identity
            );
            store->open();
            require(
                store->delta_link(fixture.identity.drive_id) ==
                        (invalidation == 0
                             ? "https://graph.example.test/after-preview"
                             : initial.delta_link) &&
                    store->drive_items(fixture.identity.drive_id).size() == 1 &&
                    test::read_file(destination) == std::string(42, 'x'),
                "cached sync did not apply retained metadata and latest cursor "
                "to SQLite"
            );
        }
        require(
            (std::ranges::find(messages, "preview_cache_reused") !=
             messages.end()) == (invalidation == 0),
            "GUI did not report the actual cache reuse decision"
        );
    }
}

void test_controller_pending_delta_restart() {
    Fixture fixture;
    test::app::FakeRuntimeFactory implementation;
    implementation.use_real_item_store = true;
    const app::RuntimeFactory factory{util::borrowed_proxy, implementation};
    const auto raw =
        test::app::FakeGraphClient{"me"}.list_delta(std::nullopt, {}, {});
    std::vector<std::optional<std::string>> requests;
    implementation.delta_override = [&](const auto& cursor, std::stop_token) {
        requests.push_back(cursor);
        if (cursor) {
            return graph::DeltaResult{
                .changes = {},
                .delta_link = "https://graph.example.test/restarted"
            };
        }
        return raw;
    };
    class CancelBeforeExecution final : public events::Observer {
    public:
        explicit CancelBeforeExecution(std::stop_source& stop)
            : stop_{stop} {
        }

    private:
        void on_event(const events::Event& event) const override {
            const auto* message = std::get_if<events::MessageEvent>(&event);
            if (message && message->event == "execution_started") {
                stop_.request_stop();
            }
        }
        std::stop_source& stop_;
    };
    bool restored = false;
    for (int phase = 0; phase < 2; ++phase) {
        bool finished = false;
        int result = -1;
        QString error;
        std::stop_source stop;
        gui::SyncController controller{[&](config::Config config,
                                           const events::Observer& observer,
                                           std::stop_token token,
                                           sync::PreviewCache* cache) {
            if (phase == 0) {
                CancelBeforeExecution cancelled{stop};
                return app::synchronize_account(
                    std::move(config),
                    factory,
                    cancelled,
                    stop.get_token(),
                    cache
                );
            }
            return app::synchronize_account(
                std::move(config), factory, observer, token, cache
            );
        }};
        QObject::connect(
            &controller,
            &gui::SyncController::events_ready,
            [&](const auto& events, std::size_t) {
                for (const auto& event : events) {
                    const auto* message =
                        std::get_if<events::MessageEvent>(&event);
                    restored |=
                        message && message->event == "pending_delta_resumed";
                }
            }
        );
        QObject::connect(
            &controller,
            &gui::SyncController::finished,
            [&](int outcome, const QString& detail, bool) {
                result = outcome;
                error = detail;
                finished = true;
            }
        );
        controller.start(fixture.file, false);
        require(
            wait_until([&] { return finished; }) && error.isEmpty() &&
                result ==
                    (phase == 0 ? exit_code::cancelled : exit_code::success),
            "recreated GUI controller could not restore a pending delta batch"
        );
        auto store =
            implementation.create_item_store(fixture.config, fixture.identity);
        store->open();
        require(
            phase == 0
                ? store->pending_delta(fixture.identity.drive_id).has_value() &&
                      !store->delta_link(fixture.identity.drive_id)
                : !store->pending_delta(fixture.identity.drive_id) &&
                      store->delta_link(fixture.identity.drive_id) ==
                          "https://graph.example.test/restarted",
            "GUI interruption or completion persisted the wrong cursor"
        );
    }
    require(
        restored &&
            requests ==
                std::vector<std::optional<std::string>>{
                    std::nullopt, raw.delta_link
                } &&
            test::read_file(
                account::AccountState::drive_data_directory(
                    fixture.config.sync_data_directory, fixture.identity
                ) /
                "notes.txt"
            ) == std::string(42, 'x'),
        "new GUI controller repeated the initial query or lost unapplied "
        "changes"
    );
}

} // namespace onedrive::test::gui_sync
