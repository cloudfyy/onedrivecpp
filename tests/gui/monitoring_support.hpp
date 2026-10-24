#pragma once

#include "app/support.hpp"
#include "app/preflight.hpp"
#include "app/lock.hpp"
#include "app/checks.hpp"
#include "localization.hpp"
#include "main_window.hpp"
#include "monitor_controller.hpp"
#include "sync_controller.hpp"
#include "onedrive/app/monitoring.hpp"
#include "onedrive/monitor/monitor.hpp"
#include "onedrive/logging/logging.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <QApplication>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include "gui/support.hpp"

namespace onedrive::test::gui_monitor {

namespace app = onedrive::app;
using namespace onedrive;
using namespace std::chrono_literals;
using gui_support::require;

template <typename Predicate> bool wait_until(Predicate predicate) {
    return gui_support::wait_until(std::move(predicate), 6000, 2);
}

struct Fixture {
    test::TemporaryDirectory temporary;
    std::filesystem::path file{temporary.path() / "config.toml"};
    config::Config config;
    account::DriveIdentity identity{test::test_drive_identity("me")};
    std::filesystem::path token;
    Fixture() {
        test::write_file(
            file,
            "config_version = 2\n[auth]\napplication_id = \"test-client\"\n"
            "[sync]\ndata_directory = \"" +
                (temporary.path() / "sync").string() +
                "\"\nmode = \"download_only\"\n[state]\ndirectory = \"" +
                (temporary.path() / "state").string() + "\"\n"
        );
        config = config::Config::load(file);
        app::detail::secure_state_directory(config.state_directory);
        token = account::AccountState::activate(
                    config.state_directory, identity, "saved-token"
                )
                    .token_directory /
                "refresh_token";
        std::filesystem::create_directories(config.sync_data_directory);
    }
};

class Observer final : public events::Observer {
    void on_event(const events::Event&) const override {
    }
};

inline QPushButton* button(QWidget& window, const char* name) {
    auto* found = window.findChild<QPushButton*>(name);
    require(found, "missing Monitor action");
    return found;
}

inline QString text(QWidget& window, const char* name) {
    auto* found = window.findChild<QLabel*>(name);
    require(found, "missing Monitor status");
    return found->text();
}

inline int read_round(const std::filesystem::path& directory) {
    std::ifstream input{directory / "round"};
    int count{};
    input >> count;
    return count;
}

class Child {
public:
    Child(const Fixture& fixture, const std::filesystem::path& gates) {
        process.start(
            QCoreApplication::applicationFilePath(),
            {"--monitor-child",
             QString::fromStdString(fixture.file.string()),
             QString::fromStdString(gates.string())}
        );
        require(
            process.waitForStarted(5000),
            "cannot start external Monitor subprocess"
        );
        require(
            wait_until([&] { return read_round(gates) == 1; }),
            "external Monitor did not enter its initial round"
        );
    }
    ~Child() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    }
    QProcess process;
};

} // namespace onedrive::test::gui_monitor
