include(FetchContent)
include(CheckCXXSourceCompiles)

option(ONEDRIVE_OFFLINE_BUILD
    "Build pinned dependencies only from verified third_party archives" OFF
)
file(READ "${PROJECT_SOURCE_DIR}/packaging/dependencies.json" ONEDRIVE_DEPENDENCIES)

function(onedrive_declare_dependency NAME)
    foreach(FIELD IN ITEMS archive url sha256)
        string(JSON ${FIELD} GET "${ONEDRIVE_DEPENDENCIES}" "${NAME}" "${FIELD}")
    endforeach()
    set(ARCHIVE "${PROJECT_SOURCE_DIR}/third_party/${archive}")
    if(EXISTS "${ARCHIVE}")
        file(SHA256 "${ARCHIVE}" ACTUAL_HASH)
        if(NOT ACTUAL_HASH STREQUAL sha256)
            message(FATAL_ERROR "${NAME} archive SHA256 mismatch: ${ARCHIVE}")
        endif()
        set(url "${ARCHIVE}")
    elseif(ONEDRIVE_OFFLINE_BUILD)
        message(FATAL_ERROR
            "Missing offline dependency: ${ARCHIVE}. Use the prepared release "
            "source archive from tools/prepare_source.py."
        )
    endif()
    if(ONEDRIVE_OFFLINE_BUILD)
        string(TOUPPER "${NAME}" UPPER_NAME)
        if(FETCHCONTENT_SOURCE_DIR_${UPPER_NAME})
            message(FATAL_ERROR
                "Offline builds cannot use FETCHCONTENT_SOURCE_DIR_${UPPER_NAME}; "
                "use the verified third_party archives."
            )
        endif()
    endif()
    FetchContent_Declare(${NAME}
        URL "${url}"
        URL_HASH "SHA256=${sha256}"
        ${ARGN}
    )
endfunction()

find_package(CLI11 CONFIG REQUIRED)
set(CURL_USE_STATIC_LIBS OFF)
find_package(CURL REQUIRED)
if(NOT ONEDRIVE_OFFLINE_BUILD)
    find_package(ftxui 7.0.3...<8 CONFIG QUIET)
endif()
if(ONEDRIVE_OFFLINE_BUILD OR NOT ftxui_FOUND)
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
    onedrive_declare_dependency(ftxui)
    FetchContent_MakeAvailable(ftxui)
    set_target_properties(component PROPERTIES EXCLUDE_FROM_ALL TRUE)
    install(FILES "${ftxui_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME FTXUI-LICENSE
    )
endif()
find_package(Microsoft.GSL CONFIG REQUIRED)
if(NOT ONEDRIVE_OFFLINE_BUILD)
    find_package(msft_proxy4 4.1...<5 CONFIG QUIET)
endif()
if(ONEDRIVE_OFFLINE_BUILD OR NOT msft_proxy4_FOUND)
    onedrive_declare_dependency(msft_proxy4 SOURCE_SUBDIR include)
    FetchContent_MakeAvailable(msft_proxy4)
    add_library(onedrive_proxy INTERFACE)
    target_include_directories(
        onedrive_proxy
        SYSTEM INTERFACE "${msft_proxy4_SOURCE_DIR}/include"
    )
    add_library(msft_proxy4::proxy ALIAS onedrive_proxy)
    install(FILES "${msft_proxy4_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME Proxy-LICENSE
    )
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
