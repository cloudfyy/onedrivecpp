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

find_package(CLI11 CONFIG REQUIRED)
set(CURL_USE_STATIC_LIBS OFF)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
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
    src/app/application.cpp
    src/app/runtime_factory.cpp
    src/auth/device_auth.cpp
    src/auth/token_store.cpp
    src/config/config.cpp
    src/graph/graph_client.cpp
    src/http/http_client.cpp
    src/logging/logging.cpp
    src/metrics/metrics.cpp
    src/monitor/monitor.cpp
    src/storage/item_database.cpp
    src/sync/download_recovery.cpp
    src/sync/download_transaction.cpp
    src/sync/filesystem_metadata.cpp
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
    PRIVATE
        CLI11::CLI11
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        spdlog::spdlog
        SQLite3::SQLite3
)

target_compile_options(onedrive_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(onedrive-cpp src/main.cpp)
target_link_libraries(onedrive-cpp PRIVATE onedrive_core)

install(TARGETS onedrive-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/onedrive-cpp.conf
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

if(BUILD_TESTING)
    add_executable(config_tests tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE onedrive_core)
    add_test(NAME config_tests COMMAND config_tests)

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

    add_executable(sync_plan_tests tests/sync_plan_tests.cpp)
    target_include_directories(sync_plan_tests PRIVATE src/sync)
    target_link_libraries(sync_plan_tests PRIVATE onedrive_core)
    add_test(NAME sync_plan_tests COMMAND sync_plan_tests)

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
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "onedrive-cpp maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
