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
    if (store.load_refresh_token() !=
        std::optional<std::string>{"refresh-secret"}) {
        return fail("refresh token ASCII whitespace was not trimmed");
    }
    if (!store.remove_refresh_token() || store.remove_refresh_token()) {
        return fail("refresh token removal did not report presence accurately");
    }
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
    return test_token_store_boundaries();
}
