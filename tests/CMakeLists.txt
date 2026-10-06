include(${PROJECT_SOURCE_DIR}/cmake/TestHelpers.cmake)

find_program(BASH_EXECUTABLE bash REQUIRED)

add_onedrive_test(
    account_state_tests
    SOURCES tests/account_state_tests.cpp
)
add_onedrive_test(
    atomic_file_tests
    SOURCES tests/atomic_file_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    ascii_tests
    SOURCES tests/ascii_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(config_tests SOURCES tests/config_tests.cpp)
add_onedrive_test(console_tests SOURCES tests/console_tests.cpp)

add_test(
    NAME bash_completion_tests
    COMMAND
        ${BASH_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/bash_completion_tests.sh
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
    device_auth_tests
    SOURCES tests/device_auth_tests.cpp
)
foreach(HTTP_TEST IN ITEMS req redirect download cancel reuse)
    add_onedrive_test(
        http_${HTTP_TEST}_tests
        SOURCES tests/http/${HTTP_TEST}.cpp
        INCLUDE_DIRS tests tests/http
        LIBRARIES spdlog::spdlog
    )
endforeach()
add_onedrive_test(
    transfer_rate_limiter_tests
    SOURCES tests/transfer_rate_limiter_tests.cpp
)
add_onedrive_test(
    proxy_client_tests
    SOURCES tests/proxy_client_tests.cpp
)
add_onedrive_test(
    graph_client_tests
    SOURCES tests/graph_client_tests.cpp
)
add_onedrive_test(
    application_cli_tests
    SOURCES tests/application_cli_tests.cpp
)
add_onedrive_test(
    monitor_tests
    SOURCES tests/monitor_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    runtime_preflight_tests
    SOURCES tests/runtime_preflight_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    sync_engine_tests
    SOURCES tests/sync_engine_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    local_filesystem_tests
    SOURCES tests/local_filesystem_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    filesystem_metadata_tests
    SOURCES tests/filesystem_metadata_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    sync_plan_tests
    SOURCES tests/sync_plan_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    selective_sync_tests
    SOURCES tests/selective_sync_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    item_operation_coordinator_tests
    SOURCES tests/item_operation_coordinator_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    typestate_tests
    SOURCES tests/typestate_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    download_space_coordinator_tests
    SOURCES tests/download_space_coordinator_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    download_progress_tests
    SOURCES tests/download_progress_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    download_components_tests
    SOURCES tests/download_components_tests.cpp
    INCLUDE_DIRS src
)
add_onedrive_test(
    download_integrity_tests
    SOURCES tests/download_integrity_tests.cpp
    INCLUDE_DIRS src
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
