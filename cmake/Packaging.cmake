set(CPACK_GENERATOR "DEB")
file(READ "${PROJECT_SOURCE_DIR}/debian/control" ONEDRIVE_DEBIAN_CONTROL)
foreach(ONEDRIVE_METADATA_FIELD IN ITEMS Maintainer Homepage Description)
    string(REGEX MATCH
        "(^|\n)${ONEDRIVE_METADATA_FIELD}: ([^\r\n]+)((\r?\n [^\r\n]*)*)"
        ONEDRIVE_METADATA_MATCH "${ONEDRIVE_DEBIAN_CONTROL}"
    )
    if(ONEDRIVE_METADATA_MATCH STREQUAL "")
        message(FATAL_ERROR "Missing ${ONEDRIVE_METADATA_FIELD} in debian/control")
    endif()
    set(ONEDRIVE_METADATA_${ONEDRIVE_METADATA_FIELD} "${CMAKE_MATCH_2}${CMAKE_MATCH_3}")
    if(ONEDRIVE_METADATA_FIELD STREQUAL "Description")
        set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${CMAKE_MATCH_2}")
        set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${CMAKE_MATCH_3}")
        string(REGEX REPLACE "^\r?\n " "" CPACK_DEBIAN_PACKAGE_DESCRIPTION
            "${CPACK_DEBIAN_PACKAGE_DESCRIPTION}"
        )
        string(REGEX REPLACE "\r?\n " "\n" CPACK_DEBIAN_PACKAGE_DESCRIPTION
            "${CPACK_DEBIAN_PACKAGE_DESCRIPTION}"
        )
        if(CPACK_DEBIAN_PACKAGE_DESCRIPTION STREQUAL "")
            message(FATAL_ERROR "Missing extended Description in debian/control")
        endif()
    endif()
endforeach()
set(CPACK_PACKAGE_CONTACT "${ONEDRIVE_METADATA_Maintainer}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${ONEDRIVE_METADATA_Homepage}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(
    CPACK_DEBIAN_PACKAGE_RELEASE
    "${ONEDRIVE_PACKAGE_REVISION}~${ONEDRIVE_PACKAGE_DISTRIBUTION}"
)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/CheckPackageVersion.cmake.in"
    generated/CheckPackageVersion.cmake
    @ONLY
)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/CheckPackageVersion.cmake")
configure_file(
    docs/onedrive-cpp.1.in
    generated/onedrive-cpp.1
    @ONLY
)
configure_file(
    docs/onedrive-cpp.zh_CN.1.in
    generated/onedrive-cpp.zh_CN.1
    @ONLY
)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
if(NOT DEFINED CPACK_PACKAGING_INSTALL_PREFIX)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
endif()
cmake_path(
    ABSOLUTE_PATH ONEDRIVE_SYSTEM_CONFIG_DIR
    BASE_DIRECTORY "${CPACK_PACKAGING_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE ONEDRIVE_PACKAGE_CONFIG_DIR
)
file(
    CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/conffiles"
    CONTENT "${ONEDRIVE_PACKAGE_CONFIG_DIR}/onedrive-cpp.toml\n"
    @ONLY
)
list(
    APPEND CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_BINARY_DIR}/generated/conffiles"
)
if(ONEDRIVE_BUILD_GUI)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, qt6-wayland")
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
