include(CheckCXXSourceCompiles)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

check_cxx_source_compiles(
    "
    #include <linux/openat2.h>
    #include <sys/syscall.h>

    int main() {
        open_how how{};
        how.resolve = RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS;
        return static_cast<int>(SYS_openat2 == 0);
    }
    "
    ONEDRIVE_HAS_OPENAT2_API
)
if(NOT ONEDRIVE_HAS_OPENAT2_API)
    message(
        FATAL_ERROR
        "onedrive-cpp requires Linux headers exposing the openat2 API "
        "(Linux 5.6 or newer)"
    )
endif()

option(
    ONEDRIVE_ENABLE_CLANG_TIDY
    "Run Clang-Tidy checks while compiling"
    OFF
)
option(
    ONEDRIVE_ENABLE_GRAPH_E2E
    "Enable live Microsoft Graph end-to-end tests"
    OFF
)
if(ONEDRIVE_ENABLE_CLANG_TIDY)
    find_program(
        ONEDRIVE_CLANG_TIDY
        NAMES clang-tidy-20 clang-tidy
        REQUIRED
    )
    set(
        CMAKE_CXX_CLANG_TIDY
        "${ONEDRIVE_CLANG_TIDY};--config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy"
    )
endif()
configure_file(
    include/onedrive/version.hpp.in
    generated/onedrive/version.hpp
    @ONLY
)
configure_file(
    docs/onedrive-cpp.1.in
    generated/onedrive-cpp.1
    @ONLY
)
set(
    ONEDRIVE_SYSTEM_CONFIG_DIR
    "/etc/onedrive-cpp"
    CACHE PATH
    "System-wide onedrive-cpp configuration directory"
)

