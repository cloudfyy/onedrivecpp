add_library(onedrive_cli
    src/ui/cli/app/app.cpp
    src/ui/cli/app/args.cpp
    src/ui/cli/app/browser_auth.cpp
    src/ui/cli/app/commands.cpp
    src/ui/cli/app/discover.cpp
    src/ui/cli/app/drive_fields.cpp
    src/ui/cli/app/info.cpp
    src/ui/cli/app/inspection.cpp
    src/ui/cli/app/local_inspection.cpp
    src/ui/cli/app/cleanup.cpp
    src/ui/cli/console.cpp
    src/ui/cli/format.cpp
    src/ui/cli/ftxui_backend.cpp
    src/ui/cli/json_backend.cpp
    src/ui/cli/localization.cpp
    src/ui/cli/message.cpp
    src/ui/cli/terminal.cpp
    src/ui/cli/text_backend.cpp
)

target_include_directories(onedrive_cli
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(onedrive_cli
    PUBLIC
        onedrive_app
    PRIVATE
        CLI11::CLI11
        ftxui::component
        ftxui::dom
        ftxui::screen
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

target_compile_options(onedrive_cli PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(onedrive_core
    src/util/atomic_file.cpp
    src/util/mount.cpp
    src/util/private_file.cpp
    src/account/account_state.cpp
    src/auth/device_auth.cpp
    src/auth/error.cpp
    src/auth/auth_code.cpp
    src/auth/token_store.cpp
    src/ui/common/observer.cpp
    src/config/console.cpp
    src/config/config.cpp
    src/config/network.cpp
    src/config/output.cpp
    src/config/parse.cpp
    src/config/sync_options.cpp
    src/graph/graph_client.cpp
    src/graph/notification.cpp
    src/graph/delta.cpp
    src/graph/delta_hierarchy.cpp
    src/graph/discovery.cpp
    src/graph/download.cpp
    src/graph/drive_ops.cpp
    src/graph/support.cpp
    src/graph/upload.cpp
    src/http/transfer_rate_limiter.cpp
    src/http/callbacks.cpp
    src/http/http_client.cpp
    src/http/proxy.cpp
    src/http/request.cpp
    src/logging/logging.cpp
    src/metrics/metrics.cpp
    src/monitor/monitor.cpp
    src/monitor/control.cpp
    src/monitor/input.cpp
    src/monitor/socket.cpp
    src/util/path_security.cpp
    src/util/remote_time.cpp
    src/util/sha256.cpp
    src/storage/delta.cpp
    src/storage/pending_delta.cpp
    src/storage/item_database.cpp
    src/storage/item_state.cpp
    src/storage/state_reset.cpp
    src/storage/status.cpp
    src/storage/integrity.cpp
    src/storage/migrations.cpp
    src/storage/schema.cpp
    src/storage/downloads.cpp
    src/storage/deletes.cpp
    src/storage/local_moves.cpp
    src/storage/remote_moves.cpp
    src/storage/markers.cpp
    src/storage/uploads.cpp
    src/storage/sqlite.cpp
    src/sync/download/integrity.cpp
    src/sync/download/progress.cpp
    src/sync/download/recovery.cpp
    src/sync/download/installation.cpp
    src/sync/download/space.cpp
    src/sync/download/target.cpp
    src/sync/download/transaction.cpp
    src/sync/filesystem/metadata.cpp
    src/sync/core/item_ops.cpp
    src/sync/core/downloads.cpp
    src/sync/core/local_move.cpp
    src/sync/core/move_schedule.cpp
    src/sync/core/reporting.cpp
    src/sync/core/remote_delete.cpp
    src/sync/filesystem/operations.cpp
    src/sync/upload/orchestration.cpp
    src/sync/upload/directory.cpp
    src/sync/upload/file.cpp
    src/sync/upload/remote_delete.cpp
    src/sync/upload/remote_move.cpp
    src/sync/filesystem/safe_sync_root.cpp
    src/sync/filesystem/safe_backup.cpp
    src/sync/filter/selective.cpp
    src/sync/download/single_file.cpp
    src/sync/download/restore.cpp
    src/sync/core/blocked_policy.cpp
    src/sync/core/plan.cpp
    src/sync/core/delta_plan.cpp
    src/sync/core/engine.cpp
    src/sync/core/preview_cache.cpp
    src/sync/core/preparation.cpp
    src/sync/core/plan_execute.cpp
    src/sync/core/plan_report.cpp
)

target_include_directories(onedrive_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(onedrive_core PRIVATE
    <algorithm>
    <atomic>
    <chrono>
    <cstddef>
    <cstdint>
    <filesystem>
    <functional>
    <memory>
    <mutex>
    <optional>
    <span>
    <string>
    <string_view>
    <system_error>
    <unordered_map>
    <utility>
    <vector>
    <nlohmann/json.hpp>
)

target_link_libraries(onedrive_core
    PUBLIC
        Microsoft.GSL::GSL
        msft_proxy4::proxy
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        spdlog::spdlog
        SQLite::SQLite3
        tomlplusplus::tomlplusplus
)

target_compile_options(onedrive_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(onedrive_app
    src/app/authentication.cpp
    src/app/browser_callback.cpp
    src/app/monitoring.cpp
    src/app/drive_set.cpp
    src/app/synchronization.cpp
    src/app/checks.cpp
    src/app/lock.cpp
    src/app/preflight.cpp
    src/app/factory.cpp
    src/app/queries.cpp
)

target_include_directories(onedrive_app
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(onedrive_app
    PUBLIC
        onedrive_core
    PRIVATE
        spdlog::spdlog
)

target_compile_options(onedrive_app PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(onedrive-cpp src/main.cpp)
target_link_libraries(onedrive-cpp PRIVATE onedrive_cli)

if(ONEDRIVE_BUILD_GUI)
    set(ONEDRIVE_GUI_WINDOW_SOURCES
        ${PROJECT_SOURCE_DIR}/src/ui/gui/autostart.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/autostart.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_account.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_helpers.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_helpers.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_items.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_logs.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_pages.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_settings.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_sync.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/main_window_tray.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/settings_editor.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/settings_editor.hpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/settings_account.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/settings_transport.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/gui/settings_monitor.cpp
    )
    find_package(Qt6 6.8 REQUIRED
        COMPONENTS Core DBus Gui Network Widgets LinguistTools
    )

    add_library(onedrive_gui_localization STATIC src/ui/gui/localization.cpp)
    target_include_directories(onedrive_gui_localization PUBLIC
        ${PROJECT_SOURCE_DIR}/src/ui/gui
    )
    target_link_libraries(onedrive_gui_localization PUBLIC onedrive_core Qt6::Core)
    set(ONEDRIVE_GUI_TRANSLATIONS_DIR
        "${CMAKE_INSTALL_DATADIR}/onedrive-cpp/translations"
    )
    file(RELATIVE_PATH ONEDRIVE_GUI_TRANSLATIONS_RELATIVE_DIR
        "${CMAKE_INSTALL_FULL_BINDIR}"
        "${CMAKE_INSTALL_FULL_DATADIR}/onedrive-cpp/translations"
    )
    target_compile_definitions(onedrive_gui_localization PRIVATE
        ONEDRIVE_GUI_TRANSLATIONS_RELATIVE_DIR="${ONEDRIVE_GUI_TRANSLATIONS_RELATIVE_DIR}"
    )
    file(GLOB ONEDRIVE_TS_FILES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/ui/gui/translations/onedrive_*.ts"
    )
    qt_add_lrelease(onedrive_gui_localization
        LRELEASE_TARGET onedrive_gui_lrelease
        TS_FILES ${ONEDRIVE_TS_FILES}
        QM_FILES_OUTPUT_VARIABLE ONEDRIVE_QM_FILES
        QM_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/translations"
        OPTIONS -nounfinished
    )
    qt_add_lupdate(onedrive_gui_localization
        LUPDATE_TARGET onedrive_gui_lupdate
        SOURCES
            ${PROJECT_SOURCE_DIR}/src/ui/gui/main.cpp
            ${ONEDRIVE_GUI_WINDOW_SOURCES}
            ${PROJECT_SOURCE_DIR}/src/ui/gui/localization.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/gui/login_controller.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/gui/login_controller.hpp
            ${PROJECT_SOURCE_DIR}/src/ui/gui/sync_controller.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/gui/sync_controller.hpp
            ${PROJECT_SOURCE_DIR}/src/ui/gui/monitor_controller.cpp
            ${PROJECT_SOURCE_DIR}/src/ui/gui/monitor_controller.hpp
        TS_FILES ${ONEDRIVE_TS_FILES}
        OPTIONS -no-obsolete
    )
    get_target_property(ONEDRIVE_QMAKE Qt6::qmake IMPORTED_LOCATION)
    execute_process(
        COMMAND "${ONEDRIVE_QMAKE}" -query QT_INSTALL_TRANSLATIONS
        OUTPUT_VARIABLE ONEDRIVE_QT_TRANSLATIONS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set(ONEDRIVE_QT_QM_FILES)
    foreach(ONEDRIVE_TS_FILE IN LISTS ONEDRIVE_TS_FILES)
        get_filename_component(ONEDRIVE_TRANSLATION_NAME "${ONEDRIVE_TS_FILE}" NAME_WE)
        string(REGEX REPLACE "^onedrive_" "" ONEDRIVE_TRANSLATION_LOCALE
            "${ONEDRIVE_TRANSLATION_NAME}"
        )
        string(TOUPPER "${ONEDRIVE_TRANSLATION_LOCALE}" ONEDRIVE_TRANSLATION_KEY)
        find_file(ONEDRIVE_QTBASE_${ONEDRIVE_TRANSLATION_KEY}
            "qtbase_${ONEDRIVE_TRANSLATION_LOCALE}.qm"
            PATHS "${ONEDRIVE_QT_TRANSLATIONS}" NO_DEFAULT_PATH REQUIRED
        )
        list(APPEND ONEDRIVE_QT_QM_FILES
            "${ONEDRIVE_QTBASE_${ONEDRIVE_TRANSLATION_KEY}}"
        )
        configure_file("${ONEDRIVE_QTBASE_${ONEDRIVE_TRANSLATION_KEY}}"
            "${CMAKE_CURRENT_BINARY_DIR}/translations/qtbase_${ONEDRIVE_TRANSLATION_LOCALE}.qm"
            COPYONLY
        )
    endforeach()
    add_dependencies(onedrive_gui_localization onedrive_gui_lrelease)
    set(ONEDRIVE_ICON_RESOURCES
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo.svg"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-16.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-24.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-32.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-48.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-64.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-128.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-256.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo.png"
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-1024.png"
    )
    set_source_files_properties(
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo.svg"
        PROPERTIES QT_RESOURCE_ALIAS onedrive-cpp.svg
    )
    foreach(ONEDRIVE_ICON_SIZE IN ITEMS 16 24 32 48 64 128 256 1024)
        set_source_files_properties(
            "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo-${ONEDRIVE_ICON_SIZE}.png"
            PROPERTIES QT_RESOURCE_ALIAS "onedrive-cpp-${ONEDRIVE_ICON_SIZE}.png"
        )
    endforeach()
    set_source_files_properties(
        "${PROJECT_SOURCE_DIR}/assets/onedrive-cpp-logo.png"
        PROPERTIES QT_RESOURCE_ALIAS onedrive-cpp-512.png
    )
    qt_add_resources(onedrive_gui_localization onedrive_icons
        PREFIX "/icons"
        BASE "${PROJECT_SOURCE_DIR}/assets"
        FILES ${ONEDRIVE_ICON_RESOURCES}
    )

    add_executable(
        onedrive-cpp-gui
        src/ui/gui/configuration.cpp
        src/ui/gui/app_state_view_model.cpp
        src/ui/gui/main.cpp
        ${ONEDRIVE_GUI_WINDOW_SOURCES}
        src/ui/gui/login_controller.cpp
        src/ui/gui/login_controller.hpp
        src/ui/gui/sync_controller.cpp
        src/ui/gui/sync_controller.hpp
        src/ui/gui/monitor_controller.cpp
        src/ui/gui/monitor_controller.hpp
        src/ui/gui/system_appearance.cpp
        src/ui/gui/system_appearance.hpp
    )
    set_target_properties(onedrive-cpp-gui PROPERTIES AUTOMOC ON)
    target_include_directories(onedrive-cpp-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(
        onedrive-cpp-gui
        PRIVATE
            onedrive_app
            Qt6::Core
            Qt6::DBus
            Qt6::Gui
            Qt6::Network
            Qt6::Widgets
            onedrive_gui_localization
            spdlog::spdlog
    )
    target_compile_options(onedrive-cpp-gui PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
    if(ONEDRIVE_ENABLE_CLANG_TIDY)
        set(ONEDRIVE_GUI_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY}")
        list(
            APPEND ONEDRIVE_GUI_CLANG_TIDY
            "--checks=-cppcoreguidelines-owning-memory"
        )
        set_target_properties(
            onedrive-cpp-gui
            PROPERTIES CXX_CLANG_TIDY "${ONEDRIVE_GUI_CLANG_TIDY}"
        )
    endif()
endif()

if(ONEDRIVE_ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "")
endif()
