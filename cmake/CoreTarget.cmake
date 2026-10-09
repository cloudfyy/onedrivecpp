add_library(onedrive_cli
    src/ui/cli/app/app.cpp
    src/ui/cli/app/args.cpp
    src/ui/cli/app/commands.cpp
    src/ui/cli/app/discover.cpp
    src/ui/cli/app/drive_fields.cpp
    src/ui/cli/app/info.cpp
    src/ui/cli/console.cpp
    src/ui/cli/format.cpp
    src/ui/cli/ftxui_backend.cpp
    src/ui/cli/json_backend.cpp
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
    src/monitor/input.cpp
    src/monitor/socket.cpp
    src/util/path_security.cpp
    src/util/remote_time.cpp
    src/util/sha256.cpp
    src/storage/delta.cpp
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
    src/sync/download/space.cpp
    src/sync/download/target.cpp
    src/sync/download/transaction.cpp
    src/sync/filesystem/metadata.cpp
    src/sync/core/item_ops.cpp
    src/sync/core/downloads.cpp
    src/sync/core/local_move.cpp
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
    src/sync/core/plan.cpp
    src/sync/core/delta_plan.cpp
    src/sync/core/engine.cpp
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
    src/app/monitoring.cpp
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
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core DBus Gui Network Widgets)

    add_executable(
        onedrive-cpp-gui
        src/ui/gui/configuration.cpp
        src/ui/gui/app_state_view_model.cpp
        src/ui/gui/main.cpp
        src/ui/gui/main_window.cpp
        src/ui/gui/login_controller.cpp
        src/ui/gui/login_controller.hpp
        src/ui/gui/system_appearance.cpp
        src/ui/gui/system_appearance.hpp
    )
    set_target_properties(onedrive-cpp-gui PROPERTIES AUTOMOC ON)
    target_include_directories(onedrive-cpp-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
    )
    target_compile_options(onedrive-cpp-gui PRIVATE
        $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endif()

if(ONEDRIVE_ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "")
endif()
