cmake_minimum_required(VERSION 3.20)
project(ActiveTAGConfigurator LANGUAGES CXX)

file(READ "${CMAKE_SOURCE_DIR}/version.json" ACTIVETAG_VERSION_JSON)
string(JSON ACTIVETAG_VERSION_MAJOR GET "${ACTIVETAG_VERSION_JSON}" major)
string(JSON ACTIVETAG_VERSION_MINOR GET "${ACTIVETAG_VERSION_JSON}" minor)
string(JSON ACTIVETAG_VERSION_PATCH GET "${ACTIVETAG_VERSION_JSON}" patch)
set(ACTIVETAG_DISPLAY_VERSION
    "v${ACTIVETAG_VERSION_MAJOR}.${ACTIVETAG_VERSION_MINOR}.${ACTIVETAG_VERSION_PATCH}")

execute_process(
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/Generate-Icon.ps1"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/Prepare-Build.ps1"
    COMMAND_ERROR_IS_FATAL ANY
)

add_executable(ActiveTAGConfigurator WIN32
    src/main.cpp
    src/active_tag.cpp
    src/active_tag.hpp
    src/serial_port.cpp
    src/serial_port.hpp
    src/generated/app.rc
)

set_target_properties(ActiveTAGConfigurator PROPERTIES
    OUTPUT_NAME "ActiveTAG-Configurator-${ACTIVETAG_DISPLAY_VERSION}")

target_compile_features(ActiveTAGConfigurator PRIVATE cxx_std_20)
target_compile_definitions(ActiveTAGConfigurator PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(ActiveTAGConfigurator PRIVATE third_party)
target_link_libraries(ActiveTAGConfigurator PRIVATE
    user32 gdi32 comctl32 comdlg32 advapi32)

if(MSVC)
    set_property(TARGET ActiveTAGConfigurator PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(ActiveTAGConfigurator PRIVATE /W4 /permissive- /EHsc)
endif()
