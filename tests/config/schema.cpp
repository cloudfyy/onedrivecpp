#include "support.hpp"

namespace {

using namespace onedrive::test::config;

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
    return test_schema();
}
