cmake_minimum_required(VERSION 3.16)

project(hmon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HMON_ENABLE_CLANG_TIDY "Run clang-tidy globally during C++ compilation" OFF)

set(HMON_SOURCES
  src/main.cpp
  src/metrics/cpu.cpp
  src/metrics/gpu.cpp
  src/metrics/system.cpp
  src/metrics/sensors_fallback.cpp
)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

add_executable(hmon ${HMON_SOURCES})
target_include_directories(hmon PRIVATE
  ${CURSES_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
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
  add_custom_target(lint
    COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
            ${HMON_SOURCES}
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
