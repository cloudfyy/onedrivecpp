set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "onedrive-cpp maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
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
