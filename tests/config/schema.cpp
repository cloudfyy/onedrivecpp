#include "support.hpp"

#include <format>
#include <utility>

namespace {

using namespace onedrive::test::config;

int test_drive_id_validation() {
    const auto path = std::filesystem::path{"/tmp/drive-id-validation.toml"};
    const auto load = [&](std::string_view key, const std::string& literal) {
        return onedrive::config::Config::load_from_string(
            path,
            "config_version = 2\n[sync]\n" + std::string{key} + " = " +
                (key == "drive_ids" ? "['me', " + literal + "]" : literal)
        );
    };
    for (const auto* key : {"drive_id", "drive_ids"}) {
        for (const auto* id :
             {"0123456789ABCDEF", "b!AbC-123_+=z", "opaque:future-id", "a"}) {
            const auto config = load(key, "'" + std::string{id} + "'");
            const auto ids = config.configured_drive_ids();
            if (ids.back() != id ||
                ids.size() !=
                    (std::string_view{key} == "drive_ids" ? 2U : 1U)) {
                return onedrive::test::fail(
                    "opaque Drive IDs were rejected or normalized"
                );
            }
        }
        for (const auto& [literal, expected] :
             {std::pair{R"("drive-\u00e9")", "drive-\xc3\xa9"},
              std::pair{R"("drive-\u00a1")", "drive-\xc2\xa1"}}) {
            if (load(key, literal).configured_drive_ids().back() != expected) {
                return onedrive::test::fail(
                    "non-control UTF-8 Drive ID was changed"
                );
            }
        }
        std::vector<std::string> invalid{
            "''",
            "' '",
            "' leading'",
            "'trailing '",
            "'two words'",
            "'https://contoso.sharepoint.com/sites/team'",
            "'HTTP://example.test/drive'",
            "'HtTpS:example.test/drive'",
            "'ftp://example.test/drive'",
            "'//contoso.sharepoint.com/sites/team'",
        };
        for (unsigned code = 0; code <= 0x9f; ++code) {
            if (code <= 0x1f || code >= 0x7f) {
                invalid.push_back(std::format(R"("a\u{:04x}b")", code));
            }
        }
        for (const auto& literal : invalid) {
            if (!onedrive::test::throws_with<std::runtime_error>(
                    [&] { static_cast<void>(load(key, literal)); },
                    "sync." + std::string{key}
                )) {
                return onedrive::test::fail(
                    std::string{key} +
                    " accepted an invalid Drive ID: " + literal
                );
            }
        }
    }
    const auto distinct = onedrive::config::Config::load_from_string(
        path, "config_version = 2\n[sync]\ndrive_ids = ['AbC', 'abc']\n"
    );
    if (distinct.drive_ids != std::vector<std::string>{"AbC", "abc"}) {
        return onedrive::test::fail(
            "Drive ID comparison changed opaque casing"
        );
    }
    return EXIT_SUCCESS;
}

int test_schema() {
    ConfigFixture fixture;
    const auto& path = fixture.path;
    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown TOML configuration key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync]\n"
               << "download_concurrency = 4\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "legacy version 1 download key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "unknown TOML configuration key 'sync.download_concurrency'"
            )) {
            std::filesystem::remove(path);
            std::cerr << "legacy download key reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[transfer]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown transfer key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[download]\n"
               << "unknown = true\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "unknown download key was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "[sync]\n"
               << "drive_id = \"me\"\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "missing TOML configuration version was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 1\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "version 1 TOML configuration was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error& error) {
        if (!std::string{error.what()}.contains(
                "requires config_version = 2"
            )) {
            std::filesystem::remove(path);
            std::cerr << "version 1 configuration reported wrong error\n";
            return EXIT_FAILURE;
        }
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[auth]\n"
               << "scopes = [\"Files.Read\", 42]\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "non-string authentication scope was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }

    {
        std::ofstream output{path};
        output << "config_version = 2\n"
               << "[sync\n";
    }
    try {
        static_cast<void>(onedrive::config::Config::load(path));
        std::filesystem::remove(path);
        std::cerr << "malformed TOML configuration was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::runtime_error&) {
    }
    std::filesystem::remove(path);
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    const auto config_path =
        std::filesystem::path{"/tmp/multi-drive-config.toml"};
    const auto multi = onedrive::config::Config::load_from_string(
        config_path,
        "config_version = 2\n[sync]\ndrive_ids = ['me', 'b!library']\n"
    );
    if (multi.configured_drive_ids() !=
            std::vector<std::string>{"me", "b!library"} ||
        multi.for_drive("b!library").drive_id != "b!library" ||
        !multi.for_drive("b!library").drive_ids.empty()) {
        return onedrive::test::fail(
            "multi-drive configuration lost its targets"
        );
    }
    for (const auto* selection :
         {"",
          "drive_id = 'me'",
          "drive_id = 'b!library'",
          "drive_ids = ['b!library']"}) {
        const auto single = onedrive::config::Config::load_from_string(
            config_path, std::string{"config_version = 2\n[sync]\n"} + selection
        );
        const auto expected =
            std::string{selection}.contains("library") ? "b!library" : "me";
        if (single.drive_id != expected ||
            single.configured_drive_ids() !=
                std::vector<std::string>{expected}) {
            return onedrive::test::fail(
                "legacy/default/single-entry Drive selection changed"
            );
        }
    }
    const auto selected = multi.for_drive("b!library");
    if (multi.drive_ids.size() != 2 ||
        selected.configured_drive_ids() !=
            std::vector<std::string>{"b!library"} ||
        selected.sync_data_directory != multi.sync_data_directory ||
        selected.state_directory != multi.state_directory ||
        selected.auth_scope != multi.auth_scope) {
        return onedrive::test::fail(
            "selecting a Drive mutated or discarded shared configuration"
        );
    }
    for (const auto* invalid :
         {"drive_ids = []",
          "drive_ids = 'me'",
          "drive_ids = [42]",
          "drive_ids = ['']",
          "drive_ids = ['me', 'me']",
          "drive_id = 'me'\ndrive_ids = ['other']"}) {
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<void>(
                        onedrive::config::Config::load_from_string(
                            config_path,
                            std::string{"config_version = 2\n[sync]\n"} +
                                invalid
                        )
                    );
                },
                "sync.drive_ids"
            )) {
            return onedrive::test::fail("invalid Drive selection was accepted");
        }
    }
    const auto path = std::filesystem::path{"/tmp/file-history-config.toml"};
    if (onedrive::config::Config::load_from_string(path, "config_version = 2\n")
                .file_history_limit != 10'000 ||
        onedrive::config::Config::load_from_string(
            path, "config_version = 2\n[history]\nmaximum_entries = 3\n"
        )
                .file_history_limit != 3) {
        return onedrive::test::fail(
            "file history default or custom limit was not loaded"
        );
    }
    for (const auto* invalid : {"0", "-1", "\"10000\"", "true", "1.5", "1.0"}) {
        if (!onedrive::test::throws_with<std::runtime_error>(
                [&] {
                    static_cast<
                        void>(onedrive::config::Config::load_from_string(
                        path,
                        std::string{
                            "config_version = 2\n[history]\nmaximum_entries = "
                        } + invalid
                    ));
                },
                "history.maximum_entries"
            )) {
            return onedrive::test::fail(
                std::string{"invalid file history limit was accepted: "} +
                invalid
            );
        }
    }
    if (!onedrive::test::throws_with<std::runtime_error>(
            [&] {
                static_cast<void>(onedrive::config::Config::load_from_string(
                    path, "config_version = 2\n[history]\nunknown = 1\n"
                ));
            },
            "history.unknown"
        )) {
        return onedrive::test::fail("unknown history setting was accepted");
    }
    if (test_drive_id_validation() != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return test_schema();
}
