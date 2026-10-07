include(${PROJECT_SOURCE_DIR}/cmake/TestHelpers.cmake)

find_program(BASH_EXECUTABLE bash REQUIRED)

add_onedrive_test(
    account_state_tests
    SOURCES tests/account/state.cpp
    INCLUDE_DIRS tests
)
add_onedrive_test(
    atomic_file_tests
    SOURCES tests/util/atomic.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    path_security_tests
    SOURCES tests/util/path_security.cpp
    INCLUDE_DIRS tests
)
add_onedrive_test(
    ascii_tests
    SOURCES tests/util/ascii.cpp
    INCLUDE_DIRS src tests
)
foreach(CONFIG_TEST IN ITEMS base proxy valid schema)
    add_onedrive_test(
        config_${CONFIG_TEST}_tests
        SOURCES tests/config/${CONFIG_TEST}.cpp
        INCLUDE_DIRS tests tests/config
    )
endforeach()
add_onedrive_test(
    console_tests
    SOURCES tests/cli/console.cpp
    INCLUDE_DIRS tests
)
add_onedrive_test(
    terminal_tests
    SOURCES tests/cli/terminal.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    tui_tests
    SOURCES tests/cli/tui.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    metrics_status_tests
    SOURCES tests/metrics/status.cpp
    INCLUDE_DIRS tests
)
add_onedrive_test(
    logging_security_tests
    SOURCES tests/logging/security.cpp
    INCLUDE_DIRS tests
    LIBRARIES spdlog::spdlog
)

add_test(
    NAME bash_completion_tests
    COMMAND
        ${BASH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/cli/completion.sh
        ${PROJECT_SOURCE_DIR}/packaging/completions/onedrive-cpp.bash
)

add_library(
    db_test_support STATIC
    ${PROJECT_SOURCE_DIR}/tests/db/support.cpp
)
target_include_directories(
    db_test_support
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/tests/db
)
target_link_libraries(db_test_support PRIVATE onedrive_core)

foreach(DB_TEST IN ITEMS state journal migrate security)
    add_onedrive_test(
        db_${DB_TEST}_tests
        SOURCES tests/db/${DB_TEST}.cpp
        INCLUDE_DIRS tests tests/db
        LIBRARIES db_test_support
    )
endforeach()
add_onedrive_test(
    db_sqlite_tests
    SOURCES tests/db/sqlite.cpp
    INCLUDE_DIRS src tests
    LIBRARIES SQLite3::SQLite3
)
add_onedrive_test(
    device_auth_tests
    SOURCES tests/auth/device.cpp
    INCLUDE_DIRS tests
)
foreach(HTTP_TEST IN ITEMS req redirect download cancel reuse)
    add_onedrive_test(
        http_${HTTP_TEST}_tests
        SOURCES tests/http/${HTTP_TEST}.cpp
        INCLUDE_DIRS src tests tests/http
        LIBRARIES spdlog::spdlog
    )
endforeach()
add_onedrive_test(
    transfer_rate_limiter_tests
    SOURCES tests/transfer/rate.cpp
    INCLUDE_DIRS tests
)
foreach(PROXY_TEST IN ITEMS socks basic transfer)
    add_onedrive_test(
        proxy_${PROXY_TEST}_tests
        SOURCES tests/proxy/${PROXY_TEST}.cpp
        INCLUDE_DIRS tests tests/proxy
    )
endforeach()
foreach(
    GRAPH_TEST
    IN ITEMS drive delta discovery upload download retry notification
)
    add_onedrive_test(
        graph_${GRAPH_TEST}_tests
        SOURCES tests/graph/${GRAPH_TEST}.cpp
        INCLUDE_DIRS tests tests/graph
    )
endforeach()
foreach(APP_TEST IN ITEMS args doctor flow)
    add_onedrive_test(
        app_${APP_TEST}_tests
        SOURCES tests/app/${APP_TEST}.cpp
        INCLUDE_DIRS tests tests/app
    )
endforeach()
foreach(MONITOR_TEST IN ITEMS state notify socket remote local timing stop)
    add_onedrive_test(
        monitor_${MONITOR_TEST}_tests
        SOURCES tests/monitor/${MONITOR_TEST}.cpp
        INCLUDE_DIRS src tests tests/monitor
    )
endforeach()
foreach(PREFLIGHT_TEST IN ITEMS state sync invalid)
    add_onedrive_test(
        preflight_${PREFLIGHT_TEST}_tests
        SOURCES tests/preflight/${PREFLIGHT_TEST}.cpp
        INCLUDE_DIRS src tests tests/preflight
    )
endforeach()
foreach(SYNC_TEST IN ITEMS base delete move download upload mode)
    add_onedrive_test(
        sync_${SYNC_TEST}_tests
        SOURCES tests/sync/${SYNC_TEST}.cpp
        INCLUDE_DIRS src tests tests/sync
    )
endforeach()
add_onedrive_test(
    local_filesystem_tests
    SOURCES tests/fs/local.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    filesystem_metadata_tests
    SOURCES tests/fs/metadata.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    sync_plan_tests
    SOURCES tests/sync/plan.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    sync_capability_tests
    SOURCES tests/sync/capability.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    selective_sync_tests
    SOURCES tests/sync/filter.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    item_operation_coordinator_tests
    SOURCES tests/sync/ops.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    typestate_tests
    SOURCES tests/sync/type.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    download_space_coordinator_tests
    SOURCES tests/download/space.cpp
    INCLUDE_DIRS src tests
)
add_onedrive_test(
    download_progress_tests
    SOURCES tests/download/progress.cpp
    INCLUDE_DIRS src tests
)
foreach(DOWNLOAD_TEST IN ITEMS single safe txn relaxed resume recover)
    add_onedrive_test(
        download_${DOWNLOAD_TEST}_tests
        SOURCES tests/download/${DOWNLOAD_TEST}.cpp
        INCLUDE_DIRS src tests tests/download
    )
endforeach()
add_onedrive_test(
    download_integrity_tests
    SOURCES tests/download/integrity.cpp
    INCLUDE_DIRS src tests
)

if(ONEDRIVE_ENABLE_GRAPH_E2E)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(STDBUF_EXECUTABLE stdbuf REQUIRED)
    add_test(
        NAME graph_sync_e2e_self_test
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/graph_sync_e2e.py
            --self-test
    )
    add_test(
        NAME graph_sync_e2e
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/graph_sync_e2e.py
            --client $<TARGET_FILE:onedrive-cpp>
            --work-root ${PROJECT_BINARY_DIR}/graph-e2e
    )
    set(ONEDRIVE_BOUNDARY_E2E_TESTS)
    foreach(
        ONEDRIVE_BOUNDARY_SCENARIO
        IN ITEMS network storage permission crash monitor
    )
        set(
            ONEDRIVE_BOUNDARY_TEST
            "system_boundary_${ONEDRIVE_BOUNDARY_SCENARIO}_e2e"
        )
        add_test(
            NAME ${ONEDRIVE_BOUNDARY_TEST}
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/graph_sync_e2e.py
                --client $<TARGET_FILE:onedrive-cpp>
                --work-root
                ${PROJECT_BINARY_DIR}/system-boundary-e2e
                --boundary ${ONEDRIVE_BOUNDARY_SCENARIO}
        )
        list(
            APPEND
            ONEDRIVE_BOUNDARY_E2E_TESTS
            ${ONEDRIVE_BOUNDARY_TEST}
        )
    endforeach()
    set_tests_properties(
        graph_sync_e2e_self_test
        graph_sync_e2e
        PROPERTIES LABELS "e2e;graph"
    )
    set_tests_properties(
        graph_sync_e2e
        ${ONEDRIVE_BOUNDARY_E2E_TESTS}
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 2400
    )
    set_tests_properties(
        ${ONEDRIVE_BOUNDARY_E2E_TESTS}
        PROPERTIES LABELS "e2e;graph;boundary"
    )
endif()
