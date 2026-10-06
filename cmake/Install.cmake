install(TARGETS onedrive-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/onedrive-cpp.toml
    DESTINATION ${ONEDRIVE_SYSTEM_CONFIG_DIR}
)
install(FILES packaging/systemd/onedrive-cpp.service
    DESTINATION lib/systemd/user
)
install(FILES README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
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
