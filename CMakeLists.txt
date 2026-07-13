cmake_minimum_required(VERSION 3.20)

project(rsync_assistant LANGUAGES CXX)

include(FetchContent)
include(ProcessorCount)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v6.1.9
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
  # Some package managers provide a config package with this spelling.
  set(RSYNC_ASSISTANT_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  # Older CMake FindSQLite3 modules use this spelling.
  set(RSYNC_ASSISTANT_SQLITE_TARGET SQLite::SQLite3)
else()
  # Older CMake FindSQLite3 modules only populate variables.  Keep the project
  # buildable on distribution CMake packages that predate the imported target.
  add_library(rsync_assistant_sqlite INTERFACE)
  target_include_directories(rsync_assistant_sqlite INTERFACE
    "${SQLite3_INCLUDE_DIRS}" "${SQLite3_INCLUDE_DIR}")
  target_link_libraries(rsync_assistant_sqlite INTERFACE "${SQLite3_LIBRARIES}" "${SQLite3_LIBRARY}")
  set(RSYNC_ASSISTANT_SQLITE_TARGET rsync_assistant_sqlite)
endif()

set(RSYNC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rsync")
set(RSYNC_BUILD_DIR "${CMAKE_BINARY_DIR}/private-rsync")
set(RSYNC_BINARY "${RSYNC_BUILD_DIR}/rsync")
find_program(RSYNC_ASSISTANT_SYSTEM_RSYNC NAMES rsync)
find_program(RSYNC_ASSISTANT_MAKE NAMES gmake make REQUIRED)
find_program(RSYNC_ASSISTANT_SSH NAMES ssh REQUIRED)
find_program(RSYNC_ASSISTANT_SCP NAMES scp REQUIRED)
find_program(RSYNC_ASSISTANT_FD NAMES fd)
find_program(RSYNC_ASSISTANT_RG NAMES rg)
if(APPLE)
  set(RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC_DEFAULT OFF)
else()
  set(RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC_DEFAULT ON)
endif()
option(RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC
       "Build the pinned rsync submodule instead of using the system rsync"
       ${RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC_DEFAULT})
ProcessorCount(RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS)
if(NOT RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS OR RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS LESS 1)
  set(RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS 1)
endif()
if(RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS GREATER 16)
  set(RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS 16)
endif()
set(RSYNC_ASSISTANT_BUILD_JOBS "${RSYNC_ASSISTANT_DEFAULT_BUILD_JOBS}" CACHE STRING
    "Internal parallel job count for bundled rsync")
if(NOT RSYNC_ASSISTANT_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "RSYNC_ASSISTANT_BUILD_JOBS must be a positive integer")
endif()

if(NOT RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC AND NOT RSYNC_ASSISTANT_SYSTEM_RSYNC)
  message(FATAL_ERROR "No system rsync was found. Enable RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC or install rsync.")
endif()
if(RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC AND NOT EXISTS "${RSYNC_SOURCE_DIR}/configure" AND NOT RSYNC_ASSISTANT_SYSTEM_RSYNC)
  message(FATAL_ERROR "Bundled rsync source is unavailable and no system rsync was found. Initialize third_party/rsync or install rsync.")
endif()

if(RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC AND EXISTS "${RSYNC_SOURCE_DIR}/configure")
  add_custom_command(
    OUTPUT "${RSYNC_BINARY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RSYNC_BUILD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${RSYNC_BUILD_DIR}"
            "${RSYNC_SOURCE_DIR}/configure" --with-included-popt --disable-md2man
    COMMAND "${CMAKE_COMMAND}" -E chdir "${RSYNC_BUILD_DIR}"
            "${RSYNC_ASSISTANT_MAKE}" "-j${RSYNC_ASSISTANT_BUILD_JOBS}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${RSYNC_SOURCE_DIR}/configure"
    COMMENT "Building pinned private rsync"
    VERBATIM
  )
  add_custom_target(private-rsync ALL DEPENDS "${RSYNC_BINARY}")
else()
  message(STATUS "Skipping bundled rsync build; runtime will fall back to system rsync only when no bundled binary exists.")
endif()

add_library(rsync_assistant_core
  src/process_runner.cpp
  src/rsync_locator.cpp
  src/project_profile.cpp
  src/endpoint.cpp
  src/directory_scanner.cpp
  src/selection_manifest.cpp
  src/benchmark_cache.cpp
  src/endpoint_benchmark.cpp
  src/settings.cpp
  src/task_control_socket.cpp
  src/task_control_service.cpp
  src/transport_selector.cpp
)

target_include_directories(rsync_assistant_core PUBLIC include)
target_link_libraries(rsync_assistant_core PUBLIC ${RSYNC_ASSISTANT_SQLITE_TARGET})

add_executable(rsync-assistant app/main.cpp)
target_link_libraries(rsync-assistant PRIVATE rsync_assistant_core)
target_link_libraries(rsync-assistant PRIVATE ftxui::component ftxui::dom ftxui::screen)
if(TARGET private-rsync)
  add_dependencies(rsync-assistant private-rsync)
endif()
target_compile_definitions(rsync-assistant PRIVATE
  RSYNC_ASSISTANT_BUNDLED_RSYNC_PATH="${RSYNC_BINARY}"
  RSYNC_ASSISTANT_SYSTEM_RSYNC_PATH="${RSYNC_ASSISTANT_SYSTEM_RSYNC}"
  RSYNC_ASSISTANT_SSH_PATH="${RSYNC_ASSISTANT_SSH}"
  RSYNC_ASSISTANT_SCP_PATH="${RSYNC_ASSISTANT_SCP}"
  RSYNC_ASSISTANT_FD_PATH="${RSYNC_ASSISTANT_FD}"
  RSYNC_ASSISTANT_RG_PATH="${RSYNC_ASSISTANT_RG}")
target_compile_definitions(rsync_assistant_core PUBLIC
  RSYNC_ASSISTANT_BUNDLED_RSYNC_PATH="${RSYNC_BINARY}"
  RSYNC_ASSISTANT_SYSTEM_RSYNC_PATH="${RSYNC_ASSISTANT_SYSTEM_RSYNC}"
  RSYNC_ASSISTANT_SSH_PATH="${RSYNC_ASSISTANT_SSH}"
  RSYNC_ASSISTANT_SCP_PATH="${RSYNC_ASSISTANT_SCP}"
  RSYNC_ASSISTANT_FD_PATH="${RSYNC_ASSISTANT_FD}"
  RSYNC_ASSISTANT_RG_PATH="${RSYNC_ASSISTANT_RG}")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  enable_testing()
  foreach(test_name IN ITEMS task_control_service task_control_socket process_runner settings project_profile endpoint directory_scanner transport_selector benchmark_cache endpoint_benchmark)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_name}_test.cpp")
      add_executable(${test_name}_test "tests/${test_name}_test.cpp")
      target_link_libraries(${test_name}_test PRIVATE rsync_assistant_core)
      add_test(NAME ${test_name}_test COMMAND ${test_name}_test)
    endif()
  endforeach()
endif()
