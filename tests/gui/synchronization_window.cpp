#include "synchronization_support.hpp"
#include "synchronization_cases.hpp"

namespace onedrive::test::gui_sync {

void test_results_and_preflight() {
    for (const int outcome : {exit_code::blocked, exit_code::failure, 42, -1}) {
        Fixture fixture;
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file),
            {},
            nullptr,
            [outcome](
                config::Config, const events::Observer&, std::stop_token
            ) {
                if (outcome == -1) {
                    throw std::runtime_error{"test failure %1"};
                }
                return outcome;
            }
        };
        auto* sync = button(window, "startSyncButton");
        auto* status = window.findChild<QLabel*>("syncStatus");
        sync->click();
        require(
            wait_until([&] { return sync->isEnabled(); }),
            "failed task did not finish"
        );
        require(
            outcome == exit_code::blocked
                ? status->text() ==
                      "Synchronization incomplete: some items were blocked."
            : outcome == -1
                ? status->text() == "Synchronization failed: test failure %1"
                : status->text().contains(QString{"exit code %1"}.arg(outcome)),
            "GUI reported a failed or blocked task as successful"
        );
    }
    Fixture fixture;
    int calls = 0;
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        nullptr,
        [&](config::Config, const events::Observer&, std::stop_token) {
            ++calls;
            return exit_code::success;
        }
    };
    auto* preview = button(window, "previewSyncButton");
    {
        const app::detail::RuntimePreflight busy{
            fixture.config, app::detail::Operation::logout
        };
        preview->click();
        require(
            wait_until([&] { return preview->isEnabled(); }) && calls == 0 &&
                window.findChild<QLabel*>("syncStatus")
                    ->text()
                    .contains("another onedrive-cpp process"),
            "GUI preview bypassed the runtime lock"
        );
    }
    test::write_file(fixture.file, "[broken TOML");
    preview->click();
    require(
        wait_until([&] { return preview->isEnabled(); }) && calls == 0 &&
            window.findChild<QLabel*>("syncStatus")
                ->text()
                .startsWith("Preview failed: "),
        "GUI preview ignored a configuration load failure"
    );
}

void test_close_and_availability() {
    Fixture fixture;
    {
        auto state = gui::load_app_state_view_model(fixture.file);
        state.sync_error = "invalid sync path";
        gui::MainWindow window{state};
        require(
            !button(window, "startSyncButton")->isEnabled() &&
                !button(window, "previewSyncButton")->isEnabled() &&
                !button(window, "stopSyncButton")->isEnabled(),
            "invalid synchronization paths enabled a task"
        );
    }
    {
        auto state = gui::load_app_state_view_model(fixture.file);
        state.account_status = gui::AccountStatus::no_credentials;
        gui::MainWindow window{state};
        require(
            !button(window, "startSyncButton")->isEnabled() &&
                !button(window, "previewSyncButton")->isEnabled(),
            "signed-out accounts enabled synchronization"
        );
    }
    std::atomic<bool> entered{};
    std::atomic<bool> finish{};
    gui::MainWindow window{
        gui::load_app_state_view_model(fixture.file),
        {},
        nullptr,
        [&](config::Config, const events::Observer&, std::stop_token token) {
            entered = true;
            while (!token.stop_requested()) {
                QThread::msleep(1);
            }
            while (!finish) {
                QThread::msleep(1);
            }
            return exit_code::cancelled;
        }
    };
    struct FinishWorker {
        std::atomic<bool>& finish;
        ~FinishWorker() {
            finish = true;
        }
    } release{finish};
    window.show();
    button(window, "startSyncButton")->click();
    require(
        wait_until([&] { return entered.load(); }),
        "close test did not start a task"
    );
    window.close();
    require(
        window.isVisible() && !button(window, "stopSyncButton")->isEnabled(),
        "window closed without waiting for synchronization"
    );
    finish = true;
    require(
        wait_until([&] { return !window.isVisible(); }),
        "window did not join its task before closing"
    );
}

void test_shared_root_busy() {
    class IgnoreEvents final : public events::Observer {
        void on_event(const events::Event&) const override {
        }
    };
    for (const auto language :
         {gui::Language::english, gui::Language::simplified_chinese}) {
        Fixture fixture;
        gui::Localization localization{language};
        test::app::FakeRuntimeFactory implementation;
        const app::RuntimeFactory factory{util::borrowed_proxy, implementation};
        gui::MainWindow window{
            gui::load_app_state_view_model(fixture.file),
            {},
            &localization,
            [&](config::Config active,
                const events::Observer& observer,
                std::stop_token token) {
                return app::synchronize_account(
                    std::move(active), factory, observer, token
                );
            }
        };
        const auto root = account::AccountState::drive_data_directory(
            fixture.config.sync_data_directory, fixture.identity
        );
        const IgnoreEvents quiet;
        auto holder = app::detail::acquire_sync_directory_lock(
            root, fixture.root.path() / "external-state", "monitor", quiet
        );
        auto* preview = button(window, "previewSyncButton");
        preview->click();
        require(
            wait_until([&] { return preview->isEnabled(); }) &&
                implementation.item_store_count == 0 &&
                !std::filesystem::exists(root) &&
                window.findChild<QLabel*>("syncStatus")
                    ->text()
                    .contains("owner PID") &&
                window.findChild<QPlainTextEdit*>("logMessages")
                    ->toPlainText()
                    .contains("monitor"),
            "GUI ignored shared root ownership or lost the busy diagnostics"
        );
        holder.reset();
        preview->click();
        require(
            wait_until([&] { return preview->isEnabled(); }) &&
                implementation.item_store_count == 1,
            "GUI could not retry after the external root owner stopped"
        );
    }
}

} // namespace onedrive::test::gui_sync
