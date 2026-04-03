cmake_minimum_required(VERSION 3.16)

project(hmon LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HMON_ENABLE_CLANG_TIDY "Run clang-tidy globally during C++ compilation" OFF)
option(HMON_PLUGIN_DIR "Directory where plugins are installed" "${CMAKE_INSTALL_LIBDIR}/hmon/plugins")

# ── Core library (libhmoncore.so) ──────────────────────────────────────────

add_library(hmoncore SHARED
  src/core/plugin_manager.cpp
)

target_include_directories(hmoncore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hmoncore PRIVATE dl)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmoncore PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(hmoncore PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
)

# ── Plugins ────────────────────────────────────────────────────────────────

# CPU plugin
add_library(hmon_cpu SHARED
  src/plugins/cpu/cpu_collector.cpp
  src/plugins/cpu/plugin.cpp
)
target_include_directories(hmon_cpu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/cpu
)
set_target_properties(hmon_cpu PROPERTIES
  PREFIX "lib"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmon_cpu PRIVATE -Wall -Wextra -Wpedantic)
endif()

# GPU plugin
add_library(hmon_gpu SHARED
  src/plugins/gpu/gpu_collector.cpp
  src/plugins/gpu/plugin.cpp
)
target_include_directories(hmon_gpu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/gpu
)
set_target_properties(hmon_gpu PROPERTIES
  PREFIX "lib"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmon_gpu PRIVATE -Wall -Wextra -Wpedantic)
endif()

# System plugin (RAM, Disk, Network)
add_library(hmon_system SHARED
  src/plugins/system/system_collector.cpp
  src/plugins/system/plugin.cpp
)
target_include_directories(hmon_system PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/system
)
set_target_properties(hmon_system PROPERTIES
  PREFIX "lib"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmon_system PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Process plugin
add_library(hmon_process SHARED
  src/plugins/process/process_collector.cpp
  src/plugins/process/plugin.cpp
)
target_include_directories(hmon_process PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/process
)
set_target_properties(hmon_process PROPERTIES
  PREFIX "lib"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmon_process PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Main executable ───────────────────────────────────────────────────────

find_package(Curses REQUIRED)

add_executable(hmon src/main.cpp)
target_include_directories(hmon PRIVATE
  ${CURSES_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(hmon PRIVATE
  HMON_PLUGIN_DIR="${CMAKE_INSTALL_FULL_LIBDIR}/hmon/plugins"
)

find_library(NCURSESW_LIBRARY NAMES ncursesw)
if(NCURSESW_LIBRARY)
  target_link_libraries(hmon PRIVATE ${NCURSESW_LIBRARY})
else()
  target_link_libraries(hmon PRIVATE ${CURSES_LIBRARIES})
endif()

target_link_libraries(hmon PRIVATE hmoncore dl)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hmon PRIVATE -Wall -Wextra -Wpedantic)
endif()

# RPATH so the binary finds libhmoncore.so at runtime
set_target_properties(hmon PROPERTIES
  INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}"
  BUILD_RPATH "${CMAKE_BINARY_DIR}"
)

# ── Install rules ──────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS hmon
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS hmoncore
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/hmon
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# Install plugins to the plugin directory
install(TARGETS hmon_cpu hmon_gpu hmon_system hmon_process
  LIBRARY DESTINATION "${HMON_PLUGIN_DIR}"
)

# ── clang-tidy ─────────────────────────────────────────────────────────────

find_program(CLANG_TIDY_EXE NAMES clang-tidy)

if(HMON_ENABLE_CLANG_TIDY)
  if(NOT CLANG_TIDY_EXE)
    message(FATAL_ERROR "HMON_ENABLE_CLANG_TIDY is ON, but clang-tidy was not found in PATH.")
  endif()
  set(CMAKE_CXX_CLANG_TIDY
      ${CLANG_TIDY_EXE};
      --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy)
endif()

if(CLANG_TIDY_EXE)
  set(ALL_SOURCES
    src/main.cpp
    src/core/plugin_manager.cpp
    src/plugins/cpu/cpu_collector.cpp src/plugins/cpu/plugin.cpp
    src/plugins/gpu/gpu_collector.cpp src/plugins/gpu/plugin.cpp
    src/plugins/system/system_collector.cpp src/plugins/system/plugin.cpp
    src/plugins/process/process_collector.cpp src/plugins/process/plugin.cpp
  )
  add_custom_target(lint
    COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
            ${ALL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-tidy lint checks"
    VERBATIM
  )
else()
  add_custom_target(lint
    COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found. Install clang-tidy to enable lint checks."
    VERBATIM
  )
endif()
