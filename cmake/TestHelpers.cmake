function(add_onedrive_test target)
    cmake_parse_arguments(
        TEST
        ""
        ""
        "SOURCES;INCLUDE_DIRS;LIBRARIES"
        ${ARGN}
    )
    if(NOT TEST_SOURCES)
        message(FATAL_ERROR "${target} requires at least one source")
    endif()

    set(TEST_RESOLVED_SOURCES)
    foreach(TEST_SOURCE IN LISTS TEST_SOURCES)
        if(IS_ABSOLUTE "${TEST_SOURCE}")
            list(APPEND TEST_RESOLVED_SOURCES "${TEST_SOURCE}")
        else()
            list(APPEND TEST_RESOLVED_SOURCES
                "${PROJECT_SOURCE_DIR}/${TEST_SOURCE}"
            )
        endif()
    endforeach()

    add_executable(${target} ${TEST_RESOLVED_SOURCES})
    set_target_properties(
        ${target}
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    )
    target_link_libraries(
        ${target}
        PRIVATE
            onedrive_core
            ${TEST_LIBRARIES}
    )
    if(TEST_INCLUDE_DIRS)
        set(TEST_RESOLVED_INCLUDE_DIRS)
        foreach(TEST_INCLUDE_DIR IN LISTS TEST_INCLUDE_DIRS)
            if(IS_ABSOLUTE "${TEST_INCLUDE_DIR}")
                list(APPEND TEST_RESOLVED_INCLUDE_DIRS "${TEST_INCLUDE_DIR}")
            else()
                list(APPEND TEST_RESOLVED_INCLUDE_DIRS
                    "${PROJECT_SOURCE_DIR}/${TEST_INCLUDE_DIR}"
                )
            endif()
        endforeach()
        target_include_directories(
            ${target}
            PRIVATE ${TEST_RESOLVED_INCLUDE_DIRS}
        )
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()
