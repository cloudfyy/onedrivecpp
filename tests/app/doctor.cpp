#include "support.hpp"

namespace {

using namespace onedrive::test::app;

int test_doctor() {
    CliFixture fixture;
    auto& temporary_directory = fixture.temporary_directory;
    const auto& config_path = fixture.config_path;
    const auto& state_path = fixture.state_path;
    const auto& sync_path = fixture.sync_path;
    const auto& log_path = fixture.log_path;
    auto& runtime_factory = fixture.runtime_factory;
    {
        onedrive::storage::ItemDatabase database{
            state_path / "doctor-fixture", onedrive::test::test_drive_identity()
        };
        database.open();
    }
    const auto healthy_diagnostics = run_application(
        runtime_factory,
        {
            "onedrive-cpp",
            "doctor",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (healthy_diagnostics.exit_code != 0 ||
        !healthy_diagnostics.standard_output.contains(
            R"("event":"database_integrity_passed")"
        ) ||
        !healthy_diagnostics.standard_output.contains(
            R"("status":"healthy")"
        )) {
        return fail("doctor did not report a healthy state database");
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
            "doctor",
            "--config",
            config_path.string(),
            "--output",
            "json",
        }
    );
    if (unhealthy_diagnostics.exit_code != 1 ||
        !unhealthy_diagnostics.standard_error.contains(
            R"("event":"database_integrity_failed")"
        ) ||
        !unhealthy_diagnostics.standard_output.contains(
            R"("status":"unhealthy")"
        )) {
        return fail("doctor did not reject a corrupt state database");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_doctor();
}
