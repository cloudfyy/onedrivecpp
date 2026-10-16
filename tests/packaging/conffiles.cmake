if(NOT IS_DIRECTORY "${PROJECT_BUILD_DIR}" OR NOT DEFINED SYSTEM_CONFIG_DIR)
    message(FATAL_ERROR "PROJECT_BUILD_DIR and SYSTEM_CONFIG_DIR are required")
endif()

include("${PROJECT_BUILD_DIR}/CPackConfig.cmake")
cmake_path(
    ABSOLUTE_PATH SYSTEM_CONFIG_DIR
    BASE_DIRECTORY "${CPACK_PACKAGING_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE EXPECTED_CONFIG_DIR
)
set(EXPECTED_CONFIG_FILE "${EXPECTED_CONFIG_DIR}/onedrive-cpp.toml")
set(STAGING_DIR "${PROJECT_BUILD_DIR}/packaging-conffiles-test")
file(REMOVE_RECURSE "${STAGING_DIR}")
execute_process(
    COMMAND "${CPACK_EXECUTABLE}"
        --config "${PROJECT_BUILD_DIR}/CPackConfig.cmake"
        -G DEB -B "${STAGING_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
)
file(GLOB PACKAGES "${STAGING_DIR}/*.deb")
list(LENGTH PACKAGES PACKAGE_COUNT)
if(NOT PACKAGE_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one DEB, found ${PACKAGE_COUNT}")
endif()
list(GET PACKAGES 0 PACKAGE)
execute_process(
    COMMAND "${DPKG_DEB_EXECUTABLE}" --control "${PACKAGE}" "${STAGING_DIR}/control"
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT EXISTS "${STAGING_DIR}/control/conffiles")
    message(FATAL_ERROR "DEB is missing conffiles metadata")
endif()
file(READ "${STAGING_DIR}/control/conffiles" CONFFILES)
if(NOT CONFFILES STREQUAL "${EXPECTED_CONFIG_FILE}\n")
    message(FATAL_ERROR "Unexpected conffiles metadata: ${CONFFILES}")
endif()
execute_process(
    COMMAND "${DPKG_DEB_EXECUTABLE}" --contents "${PACKAGE}"
    OUTPUT_VARIABLE CONTENTS
    COMMAND_ERROR_IS_FATAL ANY
)
string(FIND "${CONTENTS}" " .${EXPECTED_CONFIG_FILE}\n" CONFIG_POSITION)
if(CONFIG_POSITION EQUAL -1)
    message(FATAL_ERROR "Declared conffile is not present in the DEB")
endif()
file(REMOVE_RECURSE "${STAGING_DIR}")
