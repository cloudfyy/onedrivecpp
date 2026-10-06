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
set(
    ONEDRIVE_PACKAGE_VERSION
    "${CPACK_PACKAGE_VERSION}-${CPACK_DEBIAN_PACKAGE_RELEASE}"
)
configure_file(
    docs/onedrive-cpp.1.in
    generated/onedrive-cpp.1
    @ONLY
)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
