if(
    NOT IS_DIRECTORY "${PROJECT_BUILD_DIR}" OR
    NOT DEFINED EXPECT_GUI OR NOT DEFINED INSTALL_BINDIR
)
    message(FATAL_ERROR "PROJECT_BUILD_DIR, EXPECT_GUI and INSTALL_BINDIR are required")
endif()

set(STAGING_DIR "${PROJECT_BUILD_DIR}/packaging-test-root")
include("${CMAKE_CURRENT_LIST_DIR}/service.cmake")
foreach(PREFIX IN ITEMS /usr /usr/local "/opt/onedrive cpp%test$dollar")
    file(REMOVE_RECURSE "${STAGING_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${STAGING_DIR}"
            "${CMAKE_COMMAND}" --install "${PROJECT_BUILD_DIR}" --prefix "${PREFIX}"
        RESULT_VARIABLE INSTALL_RESULT
        OUTPUT_VARIABLE INSTALL_OUTPUT
        ERROR_VARIABLE INSTALL_ERROR
    )
    if(NOT INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR "Staged installation failed:\n${INSTALL_OUTPUT}\n${INSTALL_ERROR}")
    endif()

    set(EXPECTED_BINDIR "${INSTALL_BINDIR}")
    cmake_path(ABSOLUTE_PATH EXPECTED_BINDIR BASE_DIRECTORY "${PREFIX}" NORMALIZE)
    if(NOT EXISTS "${STAGING_DIR}${EXPECTED_BINDIR}/onedrive-cpp")
        message(FATAL_ERROR "Missing installed executable: ${EXPECTED_BINDIR}/onedrive-cpp")
    endif()
    check_service(
        "${STAGING_DIR}${PREFIX}/lib/systemd/user/onedrive-cpp.service"
        "${EXPECTED_BINDIR}/onedrive-cpp"
    )
    file(STRINGS "${PROJECT_BUILD_DIR}/install_manifest.txt" INSTALL_MANIFEST)
    list(FIND INSTALL_MANIFEST "${PREFIX}/lib/systemd/user/onedrive-cpp.service" SERVICE_INDEX)
    if(SERVICE_INDEX EQUAL -1)
        message(FATAL_ERROR "Installed service is missing from the install manifest")
    endif()

    foreach(GUI_FILE IN ITEMS
        "${EXPECTED_BINDIR}/onedrive-cpp-gui"
        "${PREFIX}/share/applications/onedrive-cpp-gui.desktop"
        "${PREFIX}/share/icons/hicolor/scalable/apps/onedrive-cpp.svg"
        "${PREFIX}/share/icons/hicolor/512x512/apps/onedrive-cpp.png"
    )
        if(EXPECT_GUI AND NOT EXISTS "${STAGING_DIR}${GUI_FILE}")
            message(FATAL_ERROR "Missing installed GUI file: ${GUI_FILE}")
        elseif(NOT EXPECT_GUI AND EXISTS "${STAGING_DIR}${GUI_FILE}")
            message(FATAL_ERROR "Unexpected GUI file in CLI-only installation: ${GUI_FILE}")
        endif()
    endforeach()

    if(EXPECT_GUI)
        find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
        if(DESKTOP_FILE_VALIDATE)
            execute_process(
                COMMAND "${DESKTOP_FILE_VALIDATE}"
                    "${STAGING_DIR}${PREFIX}/share/applications/onedrive-cpp-gui.desktop"
                COMMAND_ERROR_IS_FATAL ANY
            )
        endif()
    endif()
endforeach()

find_program(SYSTEMD_ANALYZE_EXECUTABLE systemd-analyze)
if(SYSTEMD_ANALYZE_EXECUTABLE AND NOT IS_ABSOLUTE "${INSTALL_BINDIR}")
    set(PARSER_PREFIX "${STAGING_DIR}/parser/onedrive cpp%test$dollar")
    file(COPY "${STAGING_DIR}${EXPECTED_BINDIR}/onedrive-cpp"
        DESTINATION "${PARSER_PREFIX}/${INSTALL_BINDIR}"
    )
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=DESTDIR
            "${CMAKE_COMMAND}" "-DCMAKE_INSTALL_PREFIX=${PARSER_PREFIX}"
            -P "${PROJECT_BUILD_DIR}/generated/install-service.cmake"
        COMMAND_ERROR_IS_FATAL ANY
    )
    file(MAKE_DIRECTORY "${STAGING_DIR}/runtime")
    file(CHMOD "${STAGING_DIR}/runtime" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "XDG_RUNTIME_DIR=${STAGING_DIR}/runtime"
            "${SYSTEMD_ANALYZE_EXECUTABLE}" --user --man=no --generators=no
            --recursive-errors=no verify "${PARSER_PREFIX}/lib/systemd/user/onedrive-cpp.service"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()
file(REMOVE_RECURSE "${STAGING_DIR}")
