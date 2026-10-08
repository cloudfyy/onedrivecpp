include(CMakePushCheckState)

function(onedrive_check_fmt)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES spdlog::spdlog)
    set(FMT_PROBE [=[
        #include <fmt/format.h>
        #include <spdlog/spdlog.h>
        int main() {
            auto text = fmt::format(FMT_STRING("{:02}"), 7);
            spdlog::info("{}", text);
            return text != "07";
        }
    ]=])
    # Recheck after system header updates, even in an existing build tree.
    unset(ONEDRIVE_SYSTEM_FMT_COMPILES CACHE)
    check_cxx_source_compiles(
        "${FMT_PROBE}" ONEDRIVE_SYSTEM_FMT_COMPILES
    )
    if(NOT ONEDRIVE_SYSTEM_FMT_COMPILES)
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
           NOT TARGET fmt::fmt OR
           NOT fmt_VERSION VERSION_GREATER_EQUAL 10 OR
           NOT fmt_VERSION VERSION_LESS 11)
            message(FATAL_ERROR "System fmt/spdlog failed the compile check")
        endif()

        get_target_property(FMT_INCLUDE_DIRS fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
        find_path(FMT_INCLUDE_DIR fmt/core.h
            PATHS ${FMT_INCLUDE_DIRS} NO_DEFAULT_PATH NO_CACHE REQUIRED
        )
        cmake_path(SET FMT_HEADER_DIR NORMALIZE "${FMT_INCLUDE_DIR}/fmt")
        file(READ "${FMT_HEADER_DIR}/core.h" FMT_CORE)
        set(FMT_OLD "detail::parse_format_string<true>(str_, checker(s));")
        set(FMT_NEW "detail::parse_format_string<true>(str_, checker(str_));")
        string(FIND "${FMT_CORE}" "${FMT_OLD}" FMT_PATCH_POSITION)
        if(FMT_PATCH_POSITION EQUAL -1)
            message(FATAL_ERROR
                "System fmt failed the compile check, but its header does not "
                "match the supported fmt 10 compatibility patch"
            )
        endif()

        # Backport the shared-view check from fmt commit 6797f0c (issue #4807).
        # Copy all headers because fmt uses relative includes within this directory.
        set(FMT_OVERLAY "${CMAKE_CURRENT_BINARY_DIR}/fmt-compat")
        file(GLOB FMT_HEADERS CONFIGURE_DEPENDS "${FMT_HEADER_DIR}/*.h")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FMT_HEADERS})
        foreach(FMT_HEADER IN LISTS FMT_HEADERS)
            get_filename_component(FMT_HEADER_NAME "${FMT_HEADER}" NAME)
            if(NOT FMT_HEADER_NAME STREQUAL "core.h")
                configure_file("${FMT_HEADER}" "${FMT_OVERLAY}/fmt/${FMT_HEADER_NAME}" COPYONLY)
            endif()
        endforeach()
        string(REPLACE "${FMT_OLD}" "${FMT_NEW}" FMT_CORE "${FMT_CORE}")
        file(CONFIGURE OUTPUT "${FMT_OVERLAY}/fmt/core.h" CONTENT "${FMT_CORE}" @ONLY)
        set(CMAKE_REQUIRED_INCLUDES "${FMT_OVERLAY}")
        unset(ONEDRIVE_PATCHED_FMT_COMPILES CACHE)
        check_cxx_source_compiles(
            "${FMT_PROBE}" ONEDRIVE_PATCHED_FMT_COMPILES
        )
        if(NOT ONEDRIVE_PATCHED_FMT_COMPILES)
            message(FATAL_ERROR "The fmt 10 compatibility patch failed verification")
        endif()
        target_include_directories(fmt::fmt BEFORE INTERFACE "${FMT_OVERLAY}")
        message(STATUS
            "Using build-local fmt 10 header compatibility patch; "
            "system fmt/spdlog libraries remain dynamically linked"
        )
    endif()

    unset(ONEDRIVE_FMT_ACCEPTS_INVALID_FORMAT CACHE)
    check_cxx_source_compiles([=[
        #include <fmt/format.h>
        int main() {
            auto text = fmt::format(FMT_STRING("{:d}"), "not an integer");
            return text.empty();
        }
    ]=] ONEDRIVE_FMT_ACCEPTS_INVALID_FORMAT)
    if(ONEDRIVE_FMT_ACCEPTS_INVALID_FORMAT)
        message(FATAL_ERROR "fmt must reject invalid format strings at compile time")
    endif()
    unset(ONEDRIVE_FMT_ACCEPTS_INVALID_LITERAL CACHE)
    check_cxx_source_compiles([=[
        #include <fmt/format.h>
        int main() {
            auto text = fmt::format("{:d}", "not an integer");
            return text.empty();
        }
    ]=] ONEDRIVE_FMT_ACCEPTS_INVALID_LITERAL)
    if(ONEDRIVE_FMT_ACCEPTS_INVALID_LITERAL)
        message(FATAL_ERROR "fmt must reject invalid format literals at compile time")
    endif()
    cmake_pop_check_state()
endfunction()

onedrive_check_fmt()
