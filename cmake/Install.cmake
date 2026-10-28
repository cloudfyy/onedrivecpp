install(TARGETS onedrive-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(ONEDRIVE_BUILD_GUI)
    install(TARGETS onedrive-cpp-gui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES ${ONEDRIVE_QM_FILES} ${ONEDRIVE_QT_QM_FILES}
        DESTINATION "${ONEDRIVE_GUI_TRANSLATIONS_DIR}"
    )
    install(FILES packaging/desktop/onedrive-cpp-gui.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES assets/onedrive-cpp-logo.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME onedrive-cpp.svg
    )
    foreach(ONEDRIVE_ICON_SIZE IN ITEMS 16 24 32 48 64 128 256 1024)
        install(FILES "assets/onedrive-cpp-logo-${ONEDRIVE_ICON_SIZE}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${ONEDRIVE_ICON_SIZE}x${ONEDRIVE_ICON_SIZE}/apps"
            RENAME onedrive-cpp.png
        )
    endforeach()
    install(FILES assets/onedrive-cpp-logo.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME onedrive-cpp.png
    )
    foreach(ONEDRIVE_HIDPI_PAIR IN ITEMS
        "16:assets/onedrive-cpp-logo-32.png"
        "24:assets/onedrive-cpp-logo-48.png"
        "32:assets/onedrive-cpp-logo-64.png"
        "64:assets/onedrive-cpp-logo-128.png"
        "128:assets/onedrive-cpp-logo-256.png"
        "256:assets/onedrive-cpp-logo.png"
        "512:assets/onedrive-cpp-logo-1024.png"
    )
        string(REPLACE ":" ";" ONEDRIVE_HIDPI_SPEC "${ONEDRIVE_HIDPI_PAIR}")
        list(GET ONEDRIVE_HIDPI_SPEC 0 ONEDRIVE_HIDPI_BASE)
        list(GET ONEDRIVE_HIDPI_SPEC 1 ONEDRIVE_HIDPI_SOURCE)
        install(FILES "${ONEDRIVE_HIDPI_SOURCE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${ONEDRIVE_HIDPI_BASE}x${ONEDRIVE_HIDPI_BASE}@2/apps"
            RENAME onedrive-cpp.png
        )
    endforeach()
endif()
install(FILES config/onedrive-cpp.toml
    DESTINATION ${ONEDRIVE_SYSTEM_CONFIG_DIR}
)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/install-service.cmake.in"
    generated/install-service.cmake
    @ONLY
)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/generated/install-service.cmake")
install(FILES LICENSE debian/copyright README.md README.zh-CN.md TERMS.md PRIVACY.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(
    FILES
        assets/onedrive-cpp-logo.png
        assets/onedrive-cpp-logo-1024.png
        assets/onedrive-cpp-logo.svg
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/assets
)
install(
    FILES
        docs/architecture.md
        docs/architecture.zh-CN.md
        docs/authentication.md
        docs/authentication.zh-CN.md
        docs/configuration.md
        docs/configuration.zh-CN.md
        docs/development.md
        docs/development.zh-CN.md
        docs/project.md
        docs/project.zh-CN.md
        docs/synchronization.md
        docs/synchronization.zh-CN.md
        docs/usage.md
        docs/usage.zh-CN.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/onedrive-cpp.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/onedrive-cpp.zh_CN.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/zh_CN/man1
    RENAME onedrive-cpp.1
)
install(
    FILES packaging/completions/onedrive-cpp.bash
    DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
    RENAME onedrive-cpp
)
install(
    FILES packaging/completions/_onedrive-cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/vendor-completions
)
