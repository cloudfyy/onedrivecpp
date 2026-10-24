#pragma once

#include "app/support.hpp"
#include "app/preflight.hpp"
#include "app/lock.hpp"
#include "localization.hpp"
#include "main_window.hpp"
#include "sync_controller.hpp"
#include "onedrive/app/synchronization.hpp"
#include "onedrive/app/queries.hpp"
#include "onedrive/storage/item_database.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTreeWidget>
#include <QGroupBox>

#include <spdlog/spdlog.h>

#include <atomic>
#include <array>
#include <limits>
#include <thread>
#include <tuple>

#include "gui/support.hpp"

namespace onedrive::test::gui_sync {

namespace app = onedrive::app;

using gui_support::require;
using gui_support::wait_until;

using namespace onedrive;

struct Fixture {
    test::TemporaryDirectory root;
    std::filesystem::path file{root.path() / "config.toml"};
    config::Config config;
    account::DriveIdentity identity{test::test_drive_identity("me")};
    std::filesystem::path token;

    explicit Fixture(bool dry_run = false) {
        test::write_file(
            file,
            "config_version = 2\n[sync]\ndata_directory = \"" +
                (root.path() / "sync").string() +
                "\"\nupload = false\ndry_run = " +
                (dry_run ? "true" : "false") + "\n[state]\ndirectory = \"" +
                (root.path() / "state").string() +
                "\"\n[auth]\napplication_id = \"test-client\"\n"
        );
        config = config::Config::load(file);
        std::filesystem::create_directories(config.state_directory);
        std::filesystem::permissions(
            config.state_directory, std::filesystem::perms::owner_all
        );
        token = account::AccountState::activate(
                    config.state_directory, identity, "saved-token"
                )
                    .token_directory /
                "refresh_token";
        std::filesystem::create_directories(config.sync_data_directory);
        test::write_file(
            config.sync_data_directory / "keep.txt", "keep this file"
        );
    }
};

inline QPushButton* button(QWidget& window, const char* name) {
    auto* result = window.findChild<QPushButton*>(name);
    require(result != nullptr, "missing synchronization control");
    return result;
}

} // namespace onedrive::test::gui_sync
