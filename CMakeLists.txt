cmake_minimum_required(VERSION 3.16)

project(hmon LANGUAGES CXX VERSION 1.0.0)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HMON_ENABLE_CLANG_TIDY "Run clang-tidy globally during C++ compilation" OFF)

# ── Single binary: all sources compiled together ───────────────────────────

find_package(Curses REQUIRED)

set(HMON_SOURCES
  src/main.cpp
  src/core/plugin_manager.cpp
  src/core/static_plugins.cpp
  src/plugins/cpu/cpu_collector.cpp
  src/plugins/cpu/plugin.cpp
  src/plugins/gpu/gpu_collector.cpp
  src/plugins/gpu/plugin.cpp
  src/plugins/system/system_collector.cpp
  src/plugins/system/plugin.cpp
  src/plugins/process/process_collector.cpp
  src/plugins/process/plugin.cpp
  src/plugins/docker/docker_collector.cpp
  src/plugins/docker/plugin.cpp
  src/plugins/ports/ports_collector.cpp
  src/plugins/ports/plugin.cpp
  src/plugins/systemd/systemd_collector.cpp
  src/plugins/systemd/plugin.cpp
  src/plugins/database/database_collector.cpp
  src/plugins/database/plugin.cpp
  src/plugins/webserver/webserver_collector.cpp
  src/plugins/webserver/plugin.cpp
  src/plugins/cron/cron_collector.cpp
  src/plugins/cron/plugin.cpp
)

add_executable(hmon ${HMON_SOURCES})
target_include_directories(hmon PRIVATE
  ${CURSES_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/cpu
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/gpu
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/system
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/process
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/docker
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/ports
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/systemd
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/database
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/webserver
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/cron
)

find_library(NCURSESW_LIBRARY NAMES ncursesw)
if(NCURSESW_LIBRARY)
  target_link_libraries(hmon PRIVATE ${NCURSESW_LIBRARY})
else()
  target_link_libraries(hmon PRIVATE ${CURSES_LIBRARIES})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmon PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Install ────────────────────────────────────────────────────────────────

install(TARGETS hmon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/hmon DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

# ── clang-tidy ─────────────────────────────────────────────────────────────

find_program(CLANG_TIDY_EXE NAMES clang-tidy)
if(CLANG_TIDY_EXE)
  add_custom_target(lint
    COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR}
            --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
            ${HMON_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-tidy"
    VERBATIM
  )
else()
  add_custom_target(lint
    COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found."
    VERBATIM
  )
endif()
