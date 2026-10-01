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

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
configure_file(
    include/onedrive/version.hpp.in
    generated/onedrive/version.hpp
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
    src/auth/device_auth.cpp
    src/auth/token_store.cpp
    src/config/config.cpp
    src/graph/graph_client.cpp
    src/http/http_client.cpp
    src/monitor/monitor.cpp
    src/storage/item_database.cpp
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
        CURL::libcurl
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
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
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "onedrive-cpp maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
