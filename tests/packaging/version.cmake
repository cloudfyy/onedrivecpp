if(NOT IS_DIRECTORY "${PROJECT_BUILD_DIR}")
    message(FATAL_ERROR "PROJECT_BUILD_DIR is required")
endif()
set(SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(STAGING_DIR "${PROJECT_BUILD_DIR}/packaging-version-test")
file(REMOVE_RECURSE "${STAGING_DIR}")

function(check_result NAME EXPECTED_ERROR RESULT OUTPUT)
    if(EXPECTED_ERROR STREQUAL "")
        if(NOT RESULT EQUAL 0)
            message(FATAL_ERROR "${NAME} failed:\n${OUTPUT}")
        endif()
    else()
        string(FIND "${OUTPUT}" "${EXPECTED_ERROR}" ERROR_POSITION)
        if(RESULT EQUAL 0 OR ERROR_POSITION EQUAL -1)
            message(FATAL_ERROR "${NAME} did not fail as expected:\n${OUTPUT}")
        endif()
    endif()
endfunction()

function(configure_case NAME VERSION SUITE EXPECTED_ERROR)
    set(SOURCE "${STAGING_DIR}/${NAME}/source")
    set(BUILD "${STAGING_DIR}/${NAME}/build")
    file(MAKE_DIRECTORY "${SOURCE}/docs" "${SOURCE}/debian")
    foreach(METADATA IN ITEMS LICENSE debian/control)
        configure_file("${SOURCE_ROOT}/${METADATA}" "${SOURCE}/${METADATA}" COPYONLY)
    endforeach()
    file(WRITE "${SOURCE}/vcpkg.json" "{\"version-semver\":\"1.0.1\"}\n")
    file(WRITE "${SOURCE}/debian/changelog"
        "onedrive-cpp (${VERSION}) ${SUITE}; urgency=medium\n"
    )
    foreach(MANUAL IN ITEMS onedrive-cpp.1.in onedrive-cpp.zh_CN.1.in)
        file(WRITE "${SOURCE}/docs/${MANUAL}" "@ONEDRIVE_PACKAGE_VERSION@\n")
    endforeach()
    file(WRITE "${SOURCE}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.28)\n"
        "set(TEST_PROJECT_VERSION 1.0.1 CACHE STRING \"Test application version\")\n"
        "project(onedrive-cpp VERSION \${TEST_PROJECT_VERSION} DESCRIPTION \"Version test\" LANGUAGES NONE)\n"
        "set(ONEDRIVE_SYSTEM_CONFIG_DIR etc/onedrive-cpp)\n"
        "include(\"${SOURCE_ROOT}/cmake/PackageVersion.cmake\")\n"
        "install(FILES vcpkg.json DESTINATION share/onedrive-cpp)\n"
        "include(\"${SOURCE_ROOT}/cmake/Packaging.cmake\")\n"
    )
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BUILD}"
            -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu26.04 ${ARGN}
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR
    )
    check_result("${NAME}" "${EXPECTED_ERROR}" "${RESULT}" "${OUTPUT}\n${ERROR}")
    set(LAST_BUILD "${BUILD}" PARENT_SCOPE)
    set(LAST_SOURCE "${SOURCE}" PARENT_SCOPE)
endfunction()

function(package_case NAME EXPECTED_ERROR)
    execute_process(
        COMMAND "${CPACK_EXECUTABLE}" --config "${LAST_BUILD}/CPackConfig.cmake"
            -G DEB -B "${STAGING_DIR}/packages-${NAME}" ${ARGN}
        RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR
    )
    check_result("${NAME}" "${EXPECTED_ERROR}" "${RESULT}" "${OUTPUT}\n${ERROR}")
endfunction()

configure_case(default "1.0.1-2~ubuntu24.04" noble "")
include("${LAST_BUILD}/CPackConfig.cmake")
if(NOT CPACK_DEBIAN_PACKAGE_RELEASE STREQUAL "2~ubuntu26.04")
    message(FATAL_ERROR "Default revision was not taken from debian/changelog")
endif()
package_case(runtime-revision "" -D CPACK_DEBIAN_PACKAGE_RELEASE=4~ubuntu26.04)
file(WRITE "${LAST_SOURCE}/debian/changelog" "onedrive-cpp (1.0.1-3~ubuntu24.04) noble; urgency=medium\n")
package_case(stale-revision "Changelog revision changed")
configure_case(override "1.0.1-2~ubuntu24.04" noble "" -DONEDRIVE_PACKAGE_REVISION=3)
package_case(override "")
file(GLOB PACKAGES "${STAGING_DIR}/packages-override/*.deb")
list(LENGTH PACKAGES PACKAGE_COUNT)
if(NOT PACKAGE_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one version-test DEB, found ${PACKAGE_COUNT}")
endif()
list(GET PACKAGES 0 PACKAGE)
execute_process(
    COMMAND "${DPKG_DEB_EXECUTABLE}" --field "${PACKAGE}" Version
    OUTPUT_VARIABLE ACTUAL_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT ACTUAL_VERSION STREQUAL "1.0.1-3~ubuntu26.04")
    message(FATAL_ERROR "Unexpected DEB revision: ${ACTUAL_VERSION}")
endif()
package_case(package-version "does not match vcpkg.json" -D CPACK_PACKAGE_VERSION=9.9.9)
package_case(debian-version "CPACK_DEBIAN_PACKAGE_VERSION does not match"
    -D CPACK_DEBIAN_PACKAGE_VERSION=9.9.9
)
package_case(release "Invalid CPACK_DEBIAN_PACKAGE_RELEASE"
    -D CPACK_DEBIAN_PACKAGE_RELEASE=bad-revision~ubuntu26.04
)
package_case(epoch "Package epochs are not used" -D CPACK_DEBIAN_PACKAGE_EPOCH=1)
file(WRITE "${LAST_SOURCE}/vcpkg.json" "{\"version-semver\":\"1.0.2\"}\n")
package_case(stale-manifest "does not match vcpkg.json")
package_case(stale-config-override "does not match vcpkg.json" -D CPACK_PACKAGE_VERSION=1.0.2)
file(WRITE "${LAST_SOURCE}/vcpkg.json" "{\"version-semver\":\"1.0.1\"}\n")
file(WRITE "${LAST_SOURCE}/debian/changelog" "onedrive-cpp (1.0.0-2~ubuntu24.04) noble; urgency=medium\n")
package_case(stale-changelog "does not match vcpkg.json")

configure_case(complex "1.0.1-2ubuntu1+fix~rc1~ubuntu24.04" noble "")
include("${LAST_BUILD}/CPackConfig.cmake")
if(NOT CPACK_DEBIAN_PACKAGE_RELEASE STREQUAL "2ubuntu1+fix~rc1~ubuntu26.04")
    message(FATAL_ERROR "Legal Debian revision was not preserved")
endif()
configure_case(upstream "1.0.0-1~ubuntu24.04" noble "does not match vcpkg.json")
configure_case(malformed "1.0.1" noble "Invalid Debian changelog version")
configure_case(header "1.0.1-1~ubuntu24.04" "noble other" "Invalid debian/changelog header")
configure_case(project "1.0.1-1~ubuntu24.04" noble "does not match vcpkg.json"
    -DTEST_PROJECT_VERSION=1.0.0
)
configure_case(revision "1.0.1-1~ubuntu24.04" noble "Invalid Debian package revision"
    -DONEDRIVE_PACKAGE_REVISION=bad-revision
)
configure_case(distribution "1.0.1-1~ubuntu24.04" noble "invalid package-version"
    -DONEDRIVE_PACKAGE_DISTRIBUTION=ubuntu/26.04
)

file(STRINGS "/etc/os-release" OS_RELEASE REGEX "^(ID|VERSION_ID|VERSION_CODENAME)=")
foreach(LINE IN LISTS OS_RELEASE)
    if(LINE MATCHES "^([A-Z_]+)=\"?([^\"]+)\"?$")
        set(HOST_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
if(DEFINED HOST_ID AND DEFINED HOST_VERSION_ID AND DEFINED HOST_VERSION_CODENAME)
    set(HOST_DISTRIBUTION "${HOST_ID}${HOST_VERSION_ID}")
    set(NATIVE_FLAGS
        -DONEDRIVE_DEBIAN_PACKAGE_BUILD=ON
        "-DONEDRIVE_PACKAGE_DISTRIBUTION=${HOST_DISTRIBUTION}"
    )
    configure_case(native "1.0.1-2~${HOST_DISTRIBUTION}" "${HOST_VERSION_CODENAME}" "" ${NATIVE_FLAGS})
    configure_case(native-version "1.0.1-2~different0.00" "${HOST_VERSION_CODENAME}"
        "does not match" ${NATIVE_FLAGS}
    )
    configure_case(native-suite "1.0.1-2~${HOST_DISTRIBUTION}" wrong-suite
        "Changelog suite wrong-suite does not match" ${NATIVE_FLAGS}
    )
    configure_case(native-revision "1.0.1-2~${HOST_DISTRIBUTION}" "${HOST_VERSION_CODENAME}"
        "does not match" ${NATIVE_FLAGS} -DONEDRIVE_PACKAGE_REVISION=3
    )
    configure_case(native-host "1.0.1-2~different0.00" "${HOST_VERSION_CODENAME}"
        "does not match" ${NATIVE_FLAGS} -DONEDRIVE_PACKAGE_DISTRIBUTION=different0.00
    )
else()
    message(STATUS "Skipping native-target tests: os-release lacks a version or codename")
endif()
file(REMOVE_RECURSE "${STAGING_DIR}")
