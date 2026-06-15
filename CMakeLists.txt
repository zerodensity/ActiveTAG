cmake_minimum_required(VERSION 3.20)
project(ActiveTAGConfigurator LANGUAGES CXX)

add_executable(ActiveTAGConfigurator WIN32
    src/main.cpp
    src/active_tag.cpp
    src/active_tag.hpp
    src/serial_port.cpp
    src/serial_port.hpp
)

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
