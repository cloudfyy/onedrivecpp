#include "support/progress.hpp"
#include "onedrive/auth/token_store.hpp"
#include "support/common.hpp"

namespace {

using onedrive::test::fail;
using onedrive::test::throws_with;

int test_token_store_boundaries() {
    onedrive::test::TemporaryDirectory temporary;
    onedrive::auth::FileTokenStore store{temporary.path() / "state"};
    if (store.load_refresh_token() || store.remove_refresh_token()) {
        return fail("missing refresh token was reported as present");
    }
    if (!throws_with<std::runtime_error>(
            [&] { store.save_refresh_token(""); },
            "refusing to persist an empty refresh token"
        ) ||
        std::filesystem::exists(store.path())) {
        return fail("empty token save did not fail without creating a file");
    }
    store.save_refresh_token(" \t\r\n ");
    if (store.load_refresh_token()) {
        return fail("whitespace-only refresh token was not treated as empty");
    }
    store.save_refresh_token(" \t refresh-secret \r\n ");
    if ((std::filesystem::status(store.path().parent_path()).permissions() &
         std::filesystem::perms::mask) != std::filesystem::perms::owner_all) {
        return fail("refresh token parent directory was not secured to 0700");
    }
    if (store.load_refresh_token() !=
        std::optional<std::string>{"refresh-secret"}) {
        return fail("refresh token ASCII whitespace was not trimmed");
    }
    const auto hard_link = temporary.path() / "hard-linked-token";
    std::filesystem::create_hard_link(store.path(), hard_link);
    if (!throws_with<std::runtime_error>(
            [&] { static_cast<void>(store.load_refresh_token()); },
            "exactly one hard link"
        )) {
        return fail("refresh token reader accepted a hard-linked file");
    }
    std::filesystem::remove(hard_link);
    const auto symlinked_dir = temporary.path() / "symlinked-state";
    std::filesystem::create_symlink(store.path().parent_path(), symlinked_dir);
    onedrive::auth::FileTokenStore symlinked_store{symlinked_dir};
    if (!throws_with<std::runtime_error>(
            [&] { symlinked_store.save_refresh_token("secret"); },
            "symlink"
        )) {
        return fail("refresh token writer accepted a symlinked directory");
    }
    if (!store.remove_refresh_token() || store.remove_refresh_token()) {
        return fail("refresh token removal did not report presence accurately");
    }
    std::filesystem::create_symlink(
        temporary.path() / "absent-token", store.path()
    );
    if (!throws_with<std::runtime_error>(
            [&] { static_cast<void>(store.load_refresh_token()); }
        )) {
        return fail("dangling refresh token link was reported as missing");
    }
    std::filesystem::remove(store.path());
    std::filesystem::create_directory(store.path());
    if (!throws_with<std::runtime_error>(
            [&] { static_cast<void>(store.load_refresh_token()); },
            "not a regular file"
        )) {
        return fail("refresh token reader accepted a directory");
    }
    onedrive::test::write_file(store.path() / "child", "preserved");
    if (!throws_with<std::runtime_error>(
            [&] { static_cast<void>(store.remove_refresh_token()); },
            "cannot remove refresh token file"
        ) ||
        onedrive::test::read_file(store.path() / "child") != "preserved") {
        return fail("failed refresh token removal did not preserve contents");
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    onedrive::test::ScenarioProgress progress{1};
    return progress.finish(
        progress.run("token_store_boundaries", test_token_store_boundaries)
    );
}
