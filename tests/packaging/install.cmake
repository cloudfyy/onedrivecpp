if(NOT IS_DIRECTORY "${PROJECT_BUILD_DIR}" OR NOT DEFINED EXPECT_GUI)
    message(FATAL_ERROR "PROJECT_BUILD_DIR and EXPECT_GUI are required")
endif()

set(STAGING_DIR "${PROJECT_BUILD_DIR}/packaging-test-root")
file(REMOVE_RECURSE "${STAGING_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${STAGING_DIR}"
        "${CMAKE_COMMAND}" --install "${PROJECT_BUILD_DIR}" --prefix /usr
    RESULT_VARIABLE INSTALL_RESULT
    OUTPUT_VARIABLE INSTALL_OUTPUT
    ERROR_VARIABLE INSTALL_ERROR
)
if(NOT INSTALL_RESULT EQUAL 0)
    message(FATAL_ERROR "Staged installation failed:\n${INSTALL_OUTPUT}\n${INSTALL_ERROR}")
endif()

foreach(INSTALLED_FILE IN ITEMS
    usr/bin/onedrive-cpp
    usr/lib/systemd/user/onedrive-cpp.service
)
    if(NOT EXISTS "${STAGING_DIR}/${INSTALLED_FILE}")
        message(FATAL_ERROR "Missing installed file: ${INSTALLED_FILE}")
    endif()
endforeach()

foreach(GUI_FILE IN ITEMS
    usr/bin/onedrive-cpp-gui
    usr/share/applications/onedrive-cpp-gui.desktop
    usr/share/icons/hicolor/scalable/apps/onedrive-cpp.svg
    usr/share/icons/hicolor/512x512/apps/onedrive-cpp.png
)
    if(EXPECT_GUI AND NOT EXISTS "${STAGING_DIR}/${GUI_FILE}")
        message(FATAL_ERROR "Missing installed GUI file: ${GUI_FILE}")
    elseif(NOT EXPECT_GUI AND EXISTS "${STAGING_DIR}/${GUI_FILE}")
        message(FATAL_ERROR "Unexpected GUI file in CLI-only installation: ${GUI_FILE}")
    endif()
endforeach()

if(EXPECT_GUI)
    find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
    if(DESKTOP_FILE_VALIDATE)
        execute_process(
            COMMAND "${DESKTOP_FILE_VALIDATE}"
                "${STAGING_DIR}/usr/share/applications/onedrive-cpp-gui.desktop"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endif()
file(REMOVE_RECURSE "${STAGING_DIR}")
