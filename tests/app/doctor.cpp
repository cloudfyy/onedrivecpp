#include "support.hpp"
#include "onedrive/exit_code.hpp"
#include "onedrive/logging/logging.hpp"

#include <nlohmann/json.hpp>

namespace {

namespace exit_code = onedrive::exit_code;

using namespace onedrive::test::app;

int test_doctor() {
    CliFixture fixture;
    auto& temporary_directory = fixture.temporary_directory;
    const auto& config_path = fixture.config_path;
    const auto& state_path = fixture.state_path;
    const auto& sync_path = fixture.sync_path;
    const auto& log_path = fixture.log_path;
    auto& runtime_factory = fixture.runtime_factory;
    const auto empty_diagnostics = run_application(
        runtime_factory,
        {"onedrive-cpp", "inspect", "health", "--config",
         config_path.string(), "--output", "json"}
    );
    const auto empty_message =
        nlohmann::json::parse(empty_diagnostics.standard_output);
    if (empty_diagnostics.exit_code != exit_code::success ||
        empty_message.at("event") != "database_integrity" ||
        empty_message.at("level") != "info" ||
        empty_message.at("message") !=
            "No synchronization state databases were found.") {
        return fail("empty health inspection did not use the shared info level");
    }
    {
        const onedrive::logging::Session logging{{}};
        onedrive::storage::ItemDatabase database{
            state_path / "doctor-fixture", onedrive::test::test_drive_identity()
        };
        database.open();
    }
    const auto healthy_diagnostics = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "health",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (healthy_diagnostics.exit_code != exit_code::success ||
        !healthy_diagnostics.standard_output.contains(
            R"("event":"database_integrity_passed")"
        ) ||
        !healthy_diagnostics.standard_output.contains(
            R"("status":"healthy")"
        ) ||
        !healthy_diagnostics.standard_output.contains(
            R"("schema_version":"28")"
        ) ||
        !healthy_diagnostics.standard_output.contains(
            R"("latest_schema_version":"28")"
        )) {
        return fail("inspect health did not report a healthy state database");
    }
    {
        std::ofstream corrupt{
            state_path / "doctor-fixture/items.sqlite3",
            std::ios::binary | std::ios::trunc
        };
        corrupt << "not a SQLite database";
    }
    const auto unhealthy_diagnostics = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "inspect",
            "health",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (unhealthy_diagnostics.exit_code != exit_code::failure ||
        !unhealthy_diagnostics.standard_error.contains(
            R"("event":"database_integrity_failed")"
        ) ||
        !unhealthy_diagnostics.standard_output.contains(
            R"("status":"unhealthy")"
        ) ||
        !unhealthy_diagnostics.standard_output.contains(
            R"("schema_version":"unknown")"
        ) ||
        !unhealthy_diagnostics.standard_output.contains(
            R"("latest_schema_version":"28")"
        )) {
        return fail("inspect health did not reject a corrupt state database");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_doctor();
}
