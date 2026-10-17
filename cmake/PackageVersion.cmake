set(ONEDRIVE_PACKAGE_DISTRIBUTION "" CACHE STRING
    "Distribution suffix for DEB packages, for example ubuntu24.04"
)
set(ONEDRIVE_PACKAGE_REVISION "" CACHE STRING
    "Debian revision without the distribution suffix; empty uses debian/changelog"
)
option(ONEDRIVE_DEBIAN_PACKAGE_BUILD "Validate the native Debian package target" OFF)

file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" ONEDRIVE_VERSION_MANIFEST)
string(JSON ONEDRIVE_MANIFEST_VERSION GET "${ONEDRIVE_VERSION_MANIFEST}" version-semver)
if(NOT PROJECT_VERSION STREQUAL ONEDRIVE_MANIFEST_VERSION)
    message(FATAL_ERROR
        "Application/package version ${PROJECT_VERSION} does not match "
        "vcpkg.json version ${ONEDRIVE_MANIFEST_VERSION}; reconfigure before packaging"
    )
endif()
file(READ "${PROJECT_SOURCE_DIR}/debian/changelog" ONEDRIVE_CHANGELOG)
string(REGEX MATCH "^[^\r\n]+" ONEDRIVE_CHANGELOG_HEADER "${ONEDRIVE_CHANGELOG}")
if(NOT ONEDRIVE_CHANGELOG_HEADER MATCHES "^onedrive-cpp \\(([^)]+)\\) ([A-Za-z0-9.+~-]+);")
    message(FATAL_ERROR "Invalid debian/changelog header: ${ONEDRIVE_CHANGELOG_HEADER}")
endif()
set(ONEDRIVE_CHANGELOG_VERSION "${CMAKE_MATCH_1}")
set(ONEDRIVE_CHANGELOG_SUITE "${CMAKE_MATCH_2}")
if(NOT ONEDRIVE_CHANGELOG_VERSION MATCHES "^(.+)-([A-Za-z0-9.+~]+)~([A-Za-z0-9][A-Za-z0-9.+]*)$")
    message(FATAL_ERROR "Invalid Debian changelog version: ${ONEDRIVE_CHANGELOG_VERSION}")
endif()
set(ONEDRIVE_CHANGELOG_UPSTREAM "${CMAKE_MATCH_1}")
set(ONEDRIVE_CHANGELOG_REVISION "${CMAKE_MATCH_2}")
if(NOT ONEDRIVE_CHANGELOG_UPSTREAM STREQUAL ONEDRIVE_MANIFEST_VERSION)
    message(FATAL_ERROR
        "debian/changelog upstream version ${ONEDRIVE_CHANGELOG_UPSTREAM} "
        "does not match vcpkg.json version ${ONEDRIVE_MANIFEST_VERSION}"
    )
endif()
set(ONEDRIVE_PACKAGE_REVISION_FROM_CHANGELOG OFF)
if(ONEDRIVE_PACKAGE_REVISION STREQUAL "")
    set(ONEDRIVE_PACKAGE_REVISION_FROM_CHANGELOG ON)
    set(ONEDRIVE_PACKAGE_REVISION "${ONEDRIVE_CHANGELOG_REVISION}")
endif()
if(NOT ONEDRIVE_PACKAGE_REVISION MATCHES "^[A-Za-z0-9.+~]+$")
    message(FATAL_ERROR "Invalid Debian package revision: ${ONEDRIVE_PACKAGE_REVISION}")
endif()

if(ONEDRIVE_PACKAGE_DISTRIBUTION STREQUAL "" OR ONEDRIVE_DEBIAN_PACKAGE_BUILD)
    if(NOT EXISTS "/etc/os-release")
        message(FATAL_ERROR "Cannot detect the DEB build distribution: /etc/os-release is missing")
    endif()
    file(STRINGS "/etc/os-release" ONEDRIVE_OS_RELEASE REGEX "^(ID|VERSION_ID|VERSION_CODENAME)=")
    foreach(ONEDRIVE_OS_RELEASE_LINE IN LISTS ONEDRIVE_OS_RELEASE)
        if(ONEDRIVE_OS_RELEASE_LINE MATCHES "^ID=\"?([^\"]+)\"?$")
            set(ONEDRIVE_DISTRIBUTION_ID "${CMAKE_MATCH_1}")
        elseif(ONEDRIVE_OS_RELEASE_LINE MATCHES "^VERSION_ID=\"?([^\"]+)\"?$")
            set(ONEDRIVE_DISTRIBUTION_VERSION "${CMAKE_MATCH_1}")
        elseif(ONEDRIVE_OS_RELEASE_LINE MATCHES "^VERSION_CODENAME=\"?([^\"]+)\"?$")
            set(ONEDRIVE_DISTRIBUTION_CODENAME "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT DEFINED ONEDRIVE_DISTRIBUTION_ID OR NOT DEFINED ONEDRIVE_DISTRIBUTION_VERSION)
        message(FATAL_ERROR "Cannot detect ID and VERSION_ID from /etc/os-release")
    endif()
    set(ONEDRIVE_BUILD_DISTRIBUTION "${ONEDRIVE_DISTRIBUTION_ID}${ONEDRIVE_DISTRIBUTION_VERSION}")
    if(ONEDRIVE_PACKAGE_DISTRIBUTION STREQUAL "")
        set(ONEDRIVE_PACKAGE_DISTRIBUTION "${ONEDRIVE_BUILD_DISTRIBUTION}")
    endif()
endif()
if(NOT ONEDRIVE_PACKAGE_DISTRIBUTION MATCHES "^[A-Za-z0-9][A-Za-z0-9.+]*$")
    message(FATAL_ERROR
        "ONEDRIVE_PACKAGE_DISTRIBUTION contains invalid package-version "
        "characters: ${ONEDRIVE_PACKAGE_DISTRIBUTION}"
    )
endif()
set(ONEDRIVE_PACKAGE_VERSION
    "${PROJECT_VERSION}-${ONEDRIVE_PACKAGE_REVISION}~${ONEDRIVE_PACKAGE_DISTRIBUTION}"
)
if(ONEDRIVE_DEBIAN_PACKAGE_BUILD)
    if(NOT ONEDRIVE_PACKAGE_DISTRIBUTION STREQUAL ONEDRIVE_BUILD_DISTRIBUTION)
        message(FATAL_ERROR
            "Package distribution ${ONEDRIVE_PACKAGE_DISTRIBUTION} does not match "
            "build environment ${ONEDRIVE_BUILD_DISTRIBUTION}"
        )
    endif()
    if(NOT ONEDRIVE_PACKAGE_VERSION STREQUAL ONEDRIVE_CHANGELOG_VERSION)
        message(FATAL_ERROR
            "Package version ${ONEDRIVE_PACKAGE_VERSION} does not match "
            "debian/changelog version ${ONEDRIVE_CHANGELOG_VERSION}"
        )
    endif()
    if(NOT DEFINED ONEDRIVE_DISTRIBUTION_CODENAME OR
        NOT ONEDRIVE_CHANGELOG_SUITE STREQUAL ONEDRIVE_DISTRIBUTION_CODENAME)
        message(FATAL_ERROR
            "Changelog suite ${ONEDRIVE_CHANGELOG_SUITE} does not match "
            "VERSION_CODENAME in /etc/os-release"
        )
    endif()
endif()
message(STATUS "DEB package version: ${ONEDRIVE_PACKAGE_VERSION}")
