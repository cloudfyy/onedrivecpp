#include "support.hpp"

namespace {

using namespace onedrive::test::preflight;

int test_state() {
    using onedrive::app::detail::Operation;
    using onedrive::app::detail::RuntimePreflight;

    TemporaryDirectory temporary;
    const auto config = config_for(temporary);
    {
        const RuntimePreflight first{config, Operation::logout};
        const auto state_permissions =
            std::filesystem::status(config.state_directory).permissions();
        const auto lock_permissions =
            std::filesystem::status(
                config.state_directory / "onedrive-cpp.lock"
            )
                .permissions();
        if ((state_permissions & std::filesystem::perms::all) !=
                std::filesystem::perms::owner_all ||
            (lock_permissions & std::filesystem::perms::all) !=
                (std::filesystem::perms::owner_read |
                 std::filesystem::perms::owner_write)) {
            return fail("preflight did not secure state paths");
        }
        if (!throws_with(
                [&] {
                    const RuntimePreflight second{config, Operation::logout};
                },
                "another onedrive-cpp process"
            )) {
            return fail("concurrent state-directory use was accepted");
        }
    }
    const RuntimePreflight after_release{config, Operation::logout};
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return test_state();
}
