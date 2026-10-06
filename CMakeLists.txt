cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" ONEDRIVE_VCPKG_MANIFEST)
string(JSON ONEDRIVE_VERSION GET "${ONEDRIVE_VCPKG_MANIFEST}" version-semver)

project(
    onedrive-cpp
    VERSION "${ONEDRIVE_VERSION}"
    DESCRIPTION "A modern C++ OneDrive synchronization client"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)

include(cmake/Dependencies.cmake)
include(cmake/CompilerChecks.cmake)
include(cmake/CoreTarget.cmake)
include(cmake/Install.cmake)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(cmake/Packaging.cmake)
