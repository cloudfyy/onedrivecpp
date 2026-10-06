add_library(onedrive_core
    src/util/atomic_file.cpp
    src/account/account_state.cpp
    src/app/application.cpp
    src/app/runtime_preflight.cpp
    src/app/runtime_factory.cpp
    src/auth/device_auth.cpp
    src/auth/token_store.cpp
    src/cli/console.cpp
    src/config/config.cpp
    src/config/network.cpp
    src/config/parse.cpp
    src/config/sync_options.cpp
    src/graph/graph_client.cpp
    src/graph/delta.cpp
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
    src/util/path_security.cpp
    src/util/remote_time.cpp
    src/util/sha256.cpp
    src/storage/delta.cpp
    src/storage/item_database.cpp
    src/storage/item_state.cpp
    src/storage/state_reset.cpp
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

target_link_libraries(onedrive_core
    PUBLIC
        Microsoft.GSL::GSL
        msft_proxy4::proxy
    PRIVATE
        CLI11::CLI11
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        spdlog::spdlog
        SQLite3::SQLite3
        tomlplusplus::tomlplusplus
)

target_compile_options(onedrive_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(onedrive-cpp src/main.cpp)
target_link_libraries(onedrive-cpp PRIVATE onedrive_core)

if(ONEDRIVE_ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "")
endif()

