include(FetchContent)

include(FetchContent)
include(CheckCXXSourceCompiles)

find_package(CLI11 CONFIG REQUIRED)
set(CURL_USE_STATIC_LIBS OFF)
find_package(CURL REQUIRED)
find_package(ftxui CONFIG QUIET)
if(NOT ftxui_FOUND)
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        ftxui
        URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
        URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
    )
    FetchContent_MakeAvailable(ftxui)
endif()
find_package(Microsoft.GSL CONFIG REQUIRED)
find_package(msft_proxy4 4.1 CONFIG QUIET)
if(NOT msft_proxy4_FOUND)
    FetchContent_Declare(
        msft_proxy4
        URL https://github.com/ngcpp/proxy/releases/download/4.1.0/proxy-4.1.0.tgz
        URL_HASH SHA256=3b96e426df094fdc1da9b2bfb997c04574fe2ab2d022df834f2be2ab8339ddb5
    )
    FetchContent_MakeAvailable(msft_proxy4)
    add_library(onedrive_proxy INTERFACE)
    target_include_directories(
        onedrive_proxy
        INTERFACE "${msft_proxy4_SOURCE_DIR}"
    )
    add_library(msft_proxy4::proxy ALIAS onedrive_proxy)
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
