cmake_minimum_required(VERSION 3.15)
project(tb_overlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

add_compile_options(/utf-8 /W3)

# ===== Static library: tb_icon_lib =====
# Link this into any project that needs to set/clear taskbar overlay icons
# or control taskbar grouping via AppUserModelID.
add_library(tb_icon_lib STATIC
    src/tb_icon.cpp
)

target_include_directories(tb_icon_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(tb_icon_lib PUBLIC
    ole32
    gdi32
    gdiplus
    user32
    shell32
    psapi
    propsys
)

# ===== CLI executable: tb_icon =====
add_executable(tb_icon
    src/main.cpp
)

target_link_libraries(tb_icon PRIVATE
    tb_icon_lib
)
