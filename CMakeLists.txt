cmake_minimum_required(VERSION 3.25)

project(
    onedrive-cpp
    VERSION 0.1.0
    DESCRIPTION "A modern C++ OneDrive synchronization client scaffold"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(onedrive_core
    src/app/application.cpp
    src/config/config.cpp
    src/graph/graph_client.cpp
    src/monitor/monitor.cpp
    src/storage/item_database.cpp
    src/sync/sync_engine.cpp
)

target_include_directories(onedrive_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_options(onedrive_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(onedrive-cpp src/main.cpp)
target_link_libraries(onedrive-cpp PRIVATE onedrive_core)

install(TARGETS onedrive-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/onedrive-cpp.conf
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/onedrive-cpp
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
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "onedrive-cpp maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
