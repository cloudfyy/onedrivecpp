if(NOT IS_DIRECTORY "${PROJECT_BUILD_DIR}" OR NOT DEFINED CXX_COMPILER)
    message(FATAL_ERROR "PROJECT_BUILD_DIR and CXX_COMPILER are required")
endif()
set(SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(STAGING_DIR "${PROJECT_BUILD_DIR}/packaging-offline-test")
file(REMOVE_RECURSE "${STAGING_DIR}")
file(MAKE_DIRECTORY "${STAGING_DIR}/source/packaging")
configure_file(
    "${SOURCE_ROOT}/packaging/dependencies.json"
    "${STAGING_DIR}/source/packaging/dependencies.json"
    COPYONLY
)
file(WRITE "${STAGING_DIR}/source/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.28)\n"
    "project(OfflineInputTest LANGUAGES CXX)\n"
    "include(GNUInstallDirs)\n"
    "include(\"${SOURCE_ROOT}/cmake/Dependencies.cmake\")\n"
)
foreach(SCENARIO IN ITEMS missing corrupt)
    if(SCENARIO STREQUAL "missing")
        set(EXPECTED_ERROR "Missing offline dependency")
    else()
        file(READ "${SOURCE_ROOT}/packaging/dependencies.json" MANIFEST)
        string(JSON ARCHIVE GET "${MANIFEST}" ftxui archive)
        file(WRITE "${STAGING_DIR}/source/third_party/${ARCHIVE}" "corrupted archive")
        set(EXPECTED_ERROR "SHA256 mismatch")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${STAGING_DIR}/source" -B "${STAGING_DIR}/${SCENARIO}"
            "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
            -DONEDRIVE_OFFLINE_BUILD=ON
        RESULT_VARIABLE RESULT
        OUTPUT_VARIABLE OUTPUT
        ERROR_VARIABLE ERROR
    )
    string(FIND "${OUTPUT}\n${ERROR}" "${EXPECTED_ERROR}" ERROR_POSITION)
    if(RESULT EQUAL 0 OR ERROR_POSITION EQUAL -1)
        message(FATAL_ERROR
            "${SCENARIO} archive did not fail as expected:\n${OUTPUT}\n${ERROR}"
        )
    endif()
endforeach()
file(REMOVE_RECURSE "${STAGING_DIR}")
