cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" ONEDRIVE_VCPKG_MANIFEST)
string(JSON ONEDRIVE_VERSION GET "${ONEDRIVE_VCPKG_MANIFEST}" version-semver)

project(
    onedrive-cpp
    VERSION "${ONEDRIVE_VERSION}"
    DESCRIPTION "A modern C++ OneDrive synchronization client scaffold"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)
include(FetchContent)
include(CheckCXXSourceCompiles)

find_package(CLI11 CONFIG REQUIRED)
set(CURL_USE_STATIC_LIBS OFF)
find_package(CURL REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(msft_proxy4 4.1 CONFIG QUIET)
if(NOT msft_proxy4_FOUND)
    FetchContent_Declare(
        msft_proxy4
        URL https://github.com/ngcpp/proxy/releases/download/4.1.0/proxy-4.1.0.tgz
        URL_HASH SHA256=3b96e426df094fdc1da9b2bfb997c04574fe2ab2d022df834f2be2ab8339ddb5
    )
    FetchContent_MakeAvailable(msft_proxy4)
    add_library(onedrive_proxy INTERFACE)
    target_include_directories(
        onedrive_proxy
        INTERFACE "${msft_proxy4_SOURCE_DIR}"
    )
    add_library(msft_proxy4::proxy ALIAS onedrive_proxy)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

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

add_library(onedrive_core
    src/util/atomic_file.cpp
    src/account/account_state.cpp
    src/app/application.cpp
    src/app/runtime_preflight.cpp
    src/app/runtime_factory.cpp
    src/auth/device_auth.cpp
    src/auth/token_store.cpp
    src/cli/console.cpp
    src/config/config.cpp
    src/graph/graph_client.cpp
    src/http/transfer_rate_limiter.cpp
    src/http/http_client.cpp
    src/logging/logging.cpp
    src/metrics/metrics.cpp
    src/monitor/monitor.cpp
    src/util/path_security.cpp
    src/util/remote_time.cpp
    src/util/sha256.cpp
    src/storage/item_database.cpp
    src/sync/download/integrity.cpp
    src/sync/download/progress.cpp
    src/sync/download/recovery.cpp
    src/sync/download/space_coordinator.cpp
    src/sync/download/target.cpp
    src/sync/download/transaction.cpp
    src/sync/filesystem/metadata.cpp
    src/sync/core/item_operation_coordinator.cpp
    src/sync/filesystem/local.cpp
    src/sync/upload/local.cpp
    src/sync/filesystem/safe_sync_root.cpp
    src/sync/filesystem/safe_backup.cpp
    src/sync/filter/selective.cpp
    src/sync/download/single_file.cpp
    src/sync/core/plan.cpp
    src/sync/core/engine.cpp
)

target_include_directories(onedrive_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(onedrive_core
    PUBLIC
        Microsoft.GSL::GSL
        msft_proxy4::proxy
    PRIVATE
        CLI11::CLI11
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        spdlog::spdlog
        SQLite3::SQLite3
        tomlplusplus::tomlplusplus
)

target_compile_options(onedrive_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(onedrive-cpp src/main.cpp)
target_link_libraries(onedrive-cpp PRIVATE onedrive_core)

install(TARGETS onedrive-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/onedrive-cpp.toml
    DESTINATION ${ONEDRIVE_SYSTEM_CONFIG_DIR}
)
install(FILES packaging/systemd/onedrive-cpp.service
    DESTINATION lib/systemd/user
)
install(FILES README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/onedrive-cpp.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
install(
    FILES packaging/completions/onedrive-cpp.bash
    DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
    RENAME onedrive-cpp
)
install(
    FILES packaging/completions/_onedrive-cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/vendor-completions
)

if(ONEDRIVE_ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "")
endif()

if(BUILD_TESTING)
    find_program(BASH_EXECUTABLE bash REQUIRED)

    function(add_onedrive_test target)
        cmake_parse_arguments(
            TEST
            ""
            ""
            "SOURCES;INCLUDE_DIRS;LIBRARIES"
            ${ARGN}
        )
        if(NOT TEST_SOURCES)
            message(FATAL_ERROR "${target} requires at least one source")
        endif()
        add_executable(${target} ${TEST_SOURCES})
        target_link_libraries(
            ${target}
            PRIVATE
                onedrive_core
                ${TEST_LIBRARIES}
        )
        if(TEST_INCLUDE_DIRS)
            target_include_directories(
                ${target}
                PRIVATE ${TEST_INCLUDE_DIRS}
            )
        endif()
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    add_onedrive_test(
        account_state_tests
        SOURCES tests/account_state_tests.cpp
    )
    add_onedrive_test(
        atomic_file_tests
        SOURCES tests/atomic_file_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        ascii_tests
        SOURCES tests/ascii_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(config_tests SOURCES tests/config_tests.cpp)
    add_onedrive_test(console_tests SOURCES tests/console_tests.cpp)

    add_test(
        NAME bash_completion_tests
        COMMAND
            ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bash_completion_tests.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/completions/onedrive-cpp.bash
    )

    add_onedrive_test(
        item_database_tests
        SOURCES tests/item_database_tests.cpp
    )
    add_onedrive_test(
        device_auth_tests
        SOURCES tests/device_auth_tests.cpp
    )
    add_onedrive_test(
        http_client_tests
        SOURCES tests/http_client_tests.cpp
        LIBRARIES spdlog::spdlog
    )
    add_onedrive_test(
        transfer_rate_limiter_tests
        SOURCES tests/transfer_rate_limiter_tests.cpp
    )
    add_onedrive_test(
        proxy_client_tests
        SOURCES tests/proxy_client_tests.cpp
    )
    add_onedrive_test(
        graph_client_tests
        SOURCES tests/graph_client_tests.cpp
    )
    add_onedrive_test(
        application_cli_tests
        SOURCES tests/application_cli_tests.cpp
    )
    add_onedrive_test(
        monitor_tests
        SOURCES tests/monitor_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        runtime_preflight_tests
        SOURCES tests/runtime_preflight_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        sync_engine_tests
        SOURCES tests/sync_engine_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        local_filesystem_tests
        SOURCES tests/local_filesystem_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        filesystem_metadata_tests
        SOURCES tests/filesystem_metadata_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        sync_plan_tests
        SOURCES tests/sync_plan_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        selective_sync_tests
        SOURCES tests/selective_sync_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        item_operation_coordinator_tests
        SOURCES tests/item_operation_coordinator_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        typestate_tests
        SOURCES tests/typestate_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        download_space_coordinator_tests
        SOURCES tests/download_space_coordinator_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        download_progress_tests
        SOURCES tests/download_progress_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        download_components_tests
        SOURCES tests/download_components_tests.cpp
        INCLUDE_DIRS src
    )
    add_onedrive_test(
        download_integrity_tests
        SOURCES tests/download_integrity_tests.cpp
        INCLUDE_DIRS src
    )

    if(ONEDRIVE_ENABLE_GRAPH_E2E)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        find_program(STDBUF_EXECUTABLE stdbuf REQUIRED)
        add_test(
            NAME graph_sync_e2e_self_test
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_sync_e2e.py
                --self-test
        )
        add_test(
            NAME graph_sync_e2e
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_sync_e2e.py
                --client $<TARGET_FILE:onedrive-cpp>
                --work-root ${CMAKE_CURRENT_BINARY_DIR}/graph-e2e
        )
        set(ONEDRIVE_BOUNDARY_E2E_TESTS)
        foreach(
            ONEDRIVE_BOUNDARY_SCENARIO
            IN ITEMS network storage permission crash monitor
        )
            set(
                ONEDRIVE_BOUNDARY_TEST
                "system_boundary_${ONEDRIVE_BOUNDARY_SCENARIO}_e2e"
            )
            add_test(
                NAME ${ONEDRIVE_BOUNDARY_TEST}
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_sync_e2e.py
                    --client $<TARGET_FILE:onedrive-cpp>
                    --work-root
                    ${CMAKE_CURRENT_BINARY_DIR}/system-boundary-e2e
                    --boundary ${ONEDRIVE_BOUNDARY_SCENARIO}
            )
            list(
                APPEND
                ONEDRIVE_BOUNDARY_E2E_TESTS
                ${ONEDRIVE_BOUNDARY_TEST}
            )
        endforeach()
        set_tests_properties(
            graph_sync_e2e_self_test
            graph_sync_e2e
            PROPERTIES LABELS "e2e;graph"
        )
        set_tests_properties(
            graph_sync_e2e
            ${ONEDRIVE_BOUNDARY_E2E_TESTS}
            PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 2400
        )
        set_tests_properties(
            ${ONEDRIVE_BOUNDARY_E2E_TESTS}
            PROPERTIES LABELS "e2e;graph;boundary"
        )
    endif()
endif()

set(
    ONEDRIVE_PACKAGE_DISTRIBUTION
    ""
    CACHE STRING
    "Distribution suffix for DEB packages, for example ubuntu24.04"
)
if(ONEDRIVE_PACKAGE_DISTRIBUTION STREQUAL "")
    if(NOT EXISTS "/etc/os-release")
        message(
            FATAL_ERROR
            "Cannot detect the DEB distribution; set "
            "ONEDRIVE_PACKAGE_DISTRIBUTION explicitly"
        )
    endif()
    file(
        STRINGS "/etc/os-release"
        ONEDRIVE_OS_RELEASE
        REGEX "^(ID|VERSION_ID)="
    )
    foreach(ONEDRIVE_OS_RELEASE_LINE IN LISTS ONEDRIVE_OS_RELEASE)
        if(ONEDRIVE_OS_RELEASE_LINE MATCHES "^ID=\"?([^\"]+)\"?$")
            set(ONEDRIVE_DISTRIBUTION_ID "${CMAKE_MATCH_1}")
        elseif(
            ONEDRIVE_OS_RELEASE_LINE
            MATCHES "^VERSION_ID=\"?([^\"]+)\"?$"
        )
            set(ONEDRIVE_DISTRIBUTION_VERSION "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(
        NOT DEFINED ONEDRIVE_DISTRIBUTION_ID OR
        NOT DEFINED ONEDRIVE_DISTRIBUTION_VERSION
    )
        message(
            FATAL_ERROR
            "Cannot detect ID and VERSION_ID from /etc/os-release; set "
            "ONEDRIVE_PACKAGE_DISTRIBUTION explicitly"
        )
    endif()
    set(
        ONEDRIVE_PACKAGE_DISTRIBUTION
        "${ONEDRIVE_DISTRIBUTION_ID}${ONEDRIVE_DISTRIBUTION_VERSION}"
    )
endif()
if(
    NOT ONEDRIVE_PACKAGE_DISTRIBUTION
    MATCHES "^[A-Za-z0-9][A-Za-z0-9.+]*$"
)
    message(
        FATAL_ERROR
        "ONEDRIVE_PACKAGE_DISTRIBUTION contains invalid package-version "
        "characters: ${ONEDRIVE_PACKAGE_DISTRIBUTION}"
    )
endif()
message(
    STATUS
    "DEB package distribution: ${ONEDRIVE_PACKAGE_DISTRIBUTION}"
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "onedrive-cpp maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(
    CPACK_DEBIAN_PACKAGE_RELEASE
    "1~${ONEDRIVE_PACKAGE_DISTRIBUTION}"
)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
