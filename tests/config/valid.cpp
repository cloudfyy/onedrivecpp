#include "support/progress.hpp"
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
               << "[console]\n"
               << "theme = \"rainbow\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid console theme was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("console.theme")) {
            std::filesystem::remove(path);
            std::cerr << "invalid console theme reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[console]\n"
               << "language = \"french\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "invalid console language was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains("console.language")) {
            std::filesystem::remove(path);
            std::cerr << "invalid console language reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    for (const auto& [raw, expected] :
         std::initializer_list<std::pair<const char*, onedrive::config::Language>>{
             {"auto", onedrive::config::Language::automatic},
             {"AUTO", onedrive::config::Language::automatic},
             {"en", onedrive::config::Language::english},
             {"EN", onedrive::config::Language::english},
             {"en_US", onedrive::config::Language::english},
             {"en-US", onedrive::config::Language::english},
             {"zh_CN", onedrive::config::Language::simplified_chinese},
             {"zh-CN", onedrive::config::Language::simplified_chinese},
             {"zh_Hans", onedrive::config::Language::simplified_chinese},
             {"zh-Hans", onedrive::config::Language::simplified_chinese},
             {"zh", onedrive::config::Language::simplified_chinese},
             {"ZH", onedrive::config::Language::simplified_chinese},
         }) {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[console]\n"
               << "language = \"" << raw << "\"\n";
        output.close();
        const auto loaded = onedrive::config::Config::load(path);
        if (loaded.console_language != expected) {
            std::filesystem::remove(path);
            std::cerr << "console.language was not parsed as expected\n";
            return EXIT_FAILURE;
        }
    }
    if (onedrive::config::resolve_locale_language("zh_CN.UTF-8") !=
            onedrive::config::Language::simplified_chinese ||
        onedrive::config::resolve_locale_language("zh_CN.UTF-8@euro") !=
            onedrive::config::Language::simplified_chinese ||
        onedrive::config::resolve_locale_language("zh_SG.UTF-8") !=
            onedrive::config::Language::simplified_chinese ||
        onedrive::config::resolve_locale_language("zh-Hans-CN") !=
            onedrive::config::Language::simplified_chinese ||
        onedrive::config::resolve_locale_language("zh_Hans_SG.UTF-8") !=
            onedrive::config::Language::simplified_chinese ||
        onedrive::config::resolve_locale_language("zh_CN:en_US") !=
            onedrive::config::Language::simplified_chinese ||
        onedrive::config::resolve_locale_language("en_US:zh_CN") !=
            onedrive::config::Language::english ||
        onedrive::config::resolve_locale_language("zh_TW.UTF-8") !=
            onedrive::config::Language::english ||
        onedrive::config::resolve_locale_language("zh_HK.UTF-8") !=
            onedrive::config::Language::english ||
        onedrive::config::resolve_locale_language("zh_Hant_TW") !=
            onedrive::config::Language::english ||
        onedrive::config::resolve_locale_language("fr_FR.UTF-8") !=
            onedrive::config::Language::english ||
        onedrive::config::resolve_locale_language("C.UTF-8") !=
            onedrive::config::Language::english ||
        onedrive::config::resolve_locale_language("en_US.UTF-8") !=
            onedrive::config::Language::english ||
        onedrive::config::language_name(onedrive::config::Language::automatic) !=
            "auto" ||
        onedrive::config::language_name(onedrive::config::Language::english) !=
            "en" ||
        onedrive::config::language_name(
            onedrive::config::Language::simplified_chinese
        ) != "zh_CN" ||
        onedrive::config::language_name(
            static_cast<onedrive::config::Language>(99)
        ) != "auto") {
        std::filesystem::remove(path);
        std::cerr << "locale language resolution returned the wrong language\n";
        return EXIT_FAILURE;
    }
    {
        struct EnvRestore {
            std::string name;
            std::optional<std::string> value;
            explicit EnvRestore(const char* var) : name{var} {
                if (const char* current = std::getenv(var)) {
                    value = current;
                }
            }
            ~EnvRestore() {
                if (value.has_value()) {
                    ::setenv(name.c_str(), value->c_str(), 1);
                } else {
                    ::unsetenv(name.c_str());
                }
            }
        };
        const EnvRestore restore_override{"ONEDRIVE_CPP_LANG"};
        const EnvRestore restore_lc_all{"LC_ALL"};
        const EnvRestore restore_lc_messages{"LC_MESSAGES"};
        const EnvRestore restore_lang{"LANG"};
        const EnvRestore restore_language{"LANGUAGE"};

        ::unsetenv("ONEDRIVE_CPP_LANG");
        ::unsetenv("LC_ALL");
        ::unsetenv("LC_MESSAGES");
        ::unsetenv("LANG");
        ::unsetenv("LANGUAGE");
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::english) {
            std::filesystem::remove(path);
            std::cerr << "empty locale environment did not default to English\n";
            return EXIT_FAILURE;
        }
        ::setenv("LANGUAGE", "zh_CN:en_US", 1);
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::simplified_chinese ||
            onedrive::config::resolve_language(
                onedrive::config::Language::english
            ) != onedrive::config::Language::english) {
            std::filesystem::remove(path);
            std::cerr << "LANGUAGE fallback or explicit override failed\n";
            return EXIT_FAILURE;
        }
        ::setenv("LANG", "en_US.UTF-8", 1);
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::english) {
            std::filesystem::remove(path);
            std::cerr << "LANG did not take precedence over LANGUAGE\n";
            return EXIT_FAILURE;
        }
        ::setenv("LC_MESSAGES", "zh_CN.UTF-8", 1);
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::simplified_chinese) {
            std::filesystem::remove(path);
            std::cerr << "LC_MESSAGES did not take precedence over LANG\n";
            return EXIT_FAILURE;
        }
        ::setenv("LC_ALL", "en_GB.UTF-8", 1);
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::english) {
            std::filesystem::remove(path);
            std::cerr << "LC_ALL did not take precedence over LC_MESSAGES\n";
            return EXIT_FAILURE;
        }
        ::setenv("ONEDRIVE_CPP_LANG", "zh_CN.UTF-8", 1);
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::simplified_chinese) {
            std::filesystem::remove(path);
            std::cerr << "ONEDRIVE_CPP_LANG locale string did not override LC_ALL\n";
            return EXIT_FAILURE;
        }
        ::setenv("ONEDRIVE_CPP_LANG", "auto", 1);
        if (onedrive::config::resolve_language(
                onedrive::config::Language::automatic
            ) != onedrive::config::Language::english) {
            std::filesystem::remove(path);
            std::cerr << "ONEDRIVE_CPP_LANG=auto did not fall through to LC_ALL\n";
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
    onedrive::test::ScenarioProgress progress{1};
    return progress.finish(progress.run("valid", test_valid));
}
