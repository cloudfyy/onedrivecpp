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
option(
    ONEDRIVE_ENABLE_CLANG_TIDY
    "Run Clang-Tidy checks while compiling"
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
    src/account/account_state.cpp
    src/app/application.cpp
    src/app/runtime_preflight.cpp
    src/app/runtime_factory.cpp
    src/auth/device_auth.cpp
    src/auth/token_store.cpp
    src/cli/console.cpp
    src/config/config.cpp
    src/graph/graph_client.cpp
    src/http/http_client.cpp
    src/logging/logging.cpp
    src/metrics/metrics.cpp
    src/monitor/monitor.cpp
    src/remote_time.cpp
    src/storage/item_database.cpp
    src/sync/download_integrity.cpp
    src/sync/download_recovery.cpp
    src/sync/download_space_coordinator.cpp
    src/sync/download_transaction.cpp
    src/sync/filesystem_metadata.cpp
    src/sync/item_operation_coordinator.cpp
    src/sync/local_filesystem.cpp
    src/sync/sync_plan.cpp
    src/sync/sync_engine.cpp
)

target_include_directories(onedrive_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
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

    add_executable(account_state_tests tests/account_state_tests.cpp)
    target_link_libraries(account_state_tests PRIVATE onedrive_core)
    add_test(NAME account_state_tests COMMAND account_state_tests)

    add_executable(config_tests tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE onedrive_core)
    add_test(NAME config_tests COMMAND config_tests)

    add_executable(console_tests tests/console_tests.cpp)
    target_link_libraries(console_tests PRIVATE onedrive_core)
    add_test(NAME console_tests COMMAND console_tests)

    add_test(
        NAME bash_completion_tests
        COMMAND
            ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/bash_completion_tests.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/packaging/completions/onedrive-cpp.bash
    )

    add_executable(item_database_tests tests/item_database_tests.cpp)
    target_link_libraries(item_database_tests PRIVATE onedrive_core)
    add_test(NAME item_database_tests COMMAND item_database_tests)

    add_executable(device_auth_tests tests/device_auth_tests.cpp)
    target_link_libraries(device_auth_tests PRIVATE onedrive_core)
    add_test(NAME device_auth_tests COMMAND device_auth_tests)

    add_executable(http_client_tests tests/http_client_tests.cpp)
    target_link_libraries(http_client_tests PRIVATE onedrive_core)
    add_test(NAME http_client_tests COMMAND http_client_tests)

    add_executable(graph_client_tests tests/graph_client_tests.cpp)
    target_link_libraries(graph_client_tests PRIVATE onedrive_core)
    add_test(NAME graph_client_tests COMMAND graph_client_tests)

    add_executable(application_cli_tests tests/application_cli_tests.cpp)
    target_link_libraries(application_cli_tests PRIVATE onedrive_core)
    add_test(NAME application_cli_tests COMMAND application_cli_tests)

    add_executable(
        runtime_preflight_tests
        tests/runtime_preflight_tests.cpp
    )
    target_include_directories(
        runtime_preflight_tests
        PRIVATE src/app
    )
    target_link_libraries(runtime_preflight_tests PRIVATE onedrive_core)
    add_test(NAME runtime_preflight_tests COMMAND runtime_preflight_tests)

    add_executable(sync_engine_tests tests/sync_engine_tests.cpp)
    target_link_libraries(sync_engine_tests PRIVATE onedrive_core)
    add_test(NAME sync_engine_tests COMMAND sync_engine_tests)

    add_executable(
        local_filesystem_tests
        tests/local_filesystem_tests.cpp
    )
    target_include_directories(
        local_filesystem_tests
        PRIVATE src/sync
    )
    target_link_libraries(local_filesystem_tests PRIVATE onedrive_core)
    add_test(NAME local_filesystem_tests COMMAND local_filesystem_tests)

    add_executable(
        filesystem_metadata_tests
        tests/filesystem_metadata_tests.cpp
    )
    target_include_directories(
        filesystem_metadata_tests
        PRIVATE src/sync
    )
    target_link_libraries(filesystem_metadata_tests PRIVATE onedrive_core)
    add_test(NAME filesystem_metadata_tests COMMAND filesystem_metadata_tests)

    add_executable(sync_plan_tests tests/sync_plan_tests.cpp)
    target_include_directories(sync_plan_tests PRIVATE src/sync)
    target_link_libraries(sync_plan_tests PRIVATE onedrive_core)
    add_test(NAME sync_plan_tests COMMAND sync_plan_tests)

    add_executable(
        item_operation_coordinator_tests
        tests/item_operation_coordinator_tests.cpp
    )
    target_include_directories(
        item_operation_coordinator_tests
        PRIVATE src/sync
    )
    target_link_libraries(
        item_operation_coordinator_tests
        PRIVATE onedrive_core
    )
    add_test(
        NAME item_operation_coordinator_tests
        COMMAND item_operation_coordinator_tests
    )

    add_executable(
        download_space_coordinator_tests
        tests/download_space_coordinator_tests.cpp
    )
    target_include_directories(
        download_space_coordinator_tests
        PRIVATE src/sync
    )
    target_link_libraries(
        download_space_coordinator_tests
        PRIVATE onedrive_core
    )
    add_test(
        NAME download_space_coordinator_tests
        COMMAND download_space_coordinator_tests
    )

    add_executable(
        download_components_tests
        tests/download_components_tests.cpp
    )
    target_include_directories(
        download_components_tests
        PRIVATE src/sync
    )
    target_link_libraries(download_components_tests PRIVATE onedrive_core)
    add_test(NAME download_components_tests COMMAND download_components_tests)

    add_executable(
        download_integrity_tests
        tests/download_integrity_tests.cpp
    )
    target_include_directories(
        download_integrity_tests
        PRIVATE src/sync
    )
    target_link_libraries(download_integrity_tests PRIVATE onedrive_core)
    add_test(NAME download_integrity_tests COMMAND download_integrity_tests)
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
