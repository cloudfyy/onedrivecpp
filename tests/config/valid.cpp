#include "support.hpp"

namespace {

using namespace onedrive::test::config;

int test_valid() {
    ConfigFixture fixture;
    const auto& path = fixture.path;
    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "mode = \"sideways\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid synchronization mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("sync.mode")) {
            std::filesystem::remove(path);
            std::cerr << "invalid synchronization mode reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "delete_policy = \"destroy\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid deletion policy was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("sync.delete_policy")) {
            std::filesystem::remove(path);
            std::cerr << "invalid deletion policy reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "mode = \"upload_only\"\n"
               << "upload = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "ambiguous synchronization mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "must not both be configured"
            )) {
            std::filesystem::remove(path);
            std::cerr << "ambiguous synchronization mode reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "upload = false\n";
    }
    const auto legacy = onedrive::config::Config::load(path);
    if (legacy.sync_mode != onedrive::sync::SyncMode::download_only ||
        legacy.delete_policy != onedrive::sync::DeletePolicy::propagate) {
        std::filesystem::remove(path);
        std::cerr << "legacy upload setting was not mapped safely\n";
        return EXIT_FAILURE;
    }
    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "upload = true\n";
    }
    const auto legacy_upload = onedrive::config::Config::load(path);
    if (legacy_upload.sync_mode !=
        onedrive::sync::SyncMode::bidirectional) {
        std::filesystem::remove(path);
        std::cerr << "legacy enabled upload setting was not mapped safely\n";
        return EXIT_FAILURE;
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[console]\n"
               << "color = \"sometimes\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid console color was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("console.color")) {
            std::filesystem::remove(path);
            std::cerr << "invalid console color reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[console]\n"
               << "ui = \"graphical\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid console UI was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("console.ui")) {
            std::filesystem::remove(path);
            std::cerr << "invalid console UI reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[logging]\n"
               << "level = \"verbose\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid logging level was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("logging.level")) {
            std::filesystem::remove(path);
            std::cerr << "invalid logging level reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[logging]\n"
               << "file = \"\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "empty logging file was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "logging.file must not be empty"
            )) {
            std::filesystem::remove(path);
            std::cerr << "empty logging file reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "local_conflict = \"overwrite\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unsafe local conflict mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "dotfiles = \"ignore\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid dotfile policy was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "maximum_file_size_bytes = -1\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "negative maximum file size was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "data_mount_point = \"\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "empty sync mount point was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "sync.data_mount_point must not be empty"
            )) {
            std::filesystem::remove(path);
            std::cerr << "empty sync mount point reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "mount_point = \"/tmp\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "removed sync.mount_point key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "unknown TOML configuration key 'sync.mount_point'"
            )) {
            std::filesystem::remove(path);
            std::cerr << "removed sync.mount_point reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "directory = \"/tmp/old-OneDrive\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "removed sync.directory key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "unknown TOML configuration key 'sync.directory'"
            )) {
            std::filesystem::remove(path);
            std::cerr << "removed sync.directory reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "sync_root_files = \"yes\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "non-boolean root-file policy was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "validation = \"unsafe\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download validation mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "sync_list = \"\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "empty selective sync path was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "ip_version = \"5\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download IP version was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("transfer.ip_version")) {
            std::filesystem::remove(path);
            std::cerr << "invalid download IP version reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "order = \"fastest\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid transfer order was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("transfer.order")) {
            std::filesystem::remove(path);
            std::cerr << "invalid transfer order reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "maximum_retries = -1\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "negative download retry count was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("non-negative integer")) {
            std::filesystem::remove(path);
            std::cerr << "negative download retry count reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "checkpoint_interval_bytes = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download checkpoint interval was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "permissions = \"shared\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid synchronization permissions were accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[graph.throttle]\n"
               << "maximum_retries = \"invalid\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid throttle retry count was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "http_version = \"3\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download HTTP version was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "operation_timeout_seconds = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download operation timeout was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "stall_minimum_bytes_per_second = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download stall minimum was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "chunk_threshold_bytes = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero download chunk threshold was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[upload]\n"
               << "concurrency = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero upload concurrency was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[upload]\n"
               << "chunk_size_bytes = 1048576\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unaligned upload chunk size was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[monitor]\n"
               << "poll_interval_seconds = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero monitor poll interval was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[monitor]\n"
               << "settle_delay_milliseconds = 60001\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "excessive monitor settle delay was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[monitor]\n"
               << "websocket_connect_timeout_seconds = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "zero WebSocket connect timeout was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[monitor]\n"
               << "websocket_initial_backoff_seconds = 30\n"
               << "websocket_maximum_backoff_seconds = 10\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "inverted WebSocket backoff range was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[monitor]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown monitor key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "concurrency = 17\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "excessive download concurrency was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "concurrency = 0\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid download concurrency was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[filesystem]\n"
               << "metadata = \"unsupported\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid filesystem metadata mode was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[graph.throttle]\n"
               << "initial_delay_seconds = 10\n"
               << "maximum_delay_seconds = 5\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid throttle delay range was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_valid();
}
