cmake_minimum_required(VERSION 3.20)

project(rsync_assistant LANGUAGES CXX)

include(FetchContent)
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

set(RSYNC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rsync")
set(RSYNC_BUILD_DIR "${CMAKE_BINARY_DIR}/private-rsync")
set(RSYNC_BINARY "${RSYNC_BUILD_DIR}/rsync")
find_program(RSYNC_ASSISTANT_SYSTEM_RSYNC NAMES rsync REQUIRED)
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

if(RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC AND EXISTS "${RSYNC_SOURCE_DIR}/configure")
  add_custom_command(
    OUTPUT "${RSYNC_BINARY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${RSYNC_BUILD_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${RSYNC_BUILD_DIR}"
            "${RSYNC_SOURCE_DIR}/configure" --with-included-popt
    COMMAND "${CMAKE_COMMAND}" -E chdir "${RSYNC_BUILD_DIR}"
            "${CMAKE_MAKE_PROGRAM}"
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
  src/settings.cpp
  src/task_control_socket.cpp
  src/task_control_service.cpp
)

target_include_directories(rsync_assistant_core PUBLIC include)
target_link_libraries(rsync_assistant_core PUBLIC SQLite3::SQLite3)

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

enable_testing()

add_executable(task_control_service_test
  tests/task_control_service_test.cpp
)
target_link_libraries(task_control_service_test PRIVATE rsync_assistant_core)
add_test(NAME task_control_service_test COMMAND task_control_service_test)

add_executable(task_control_socket_test
  tests/task_control_socket_test.cpp
)
target_link_libraries(task_control_socket_test PRIVATE rsync_assistant_core)
add_test(NAME task_control_socket_test COMMAND task_control_socket_test)

add_executable(process_runner_test tests/process_runner_test.cpp)
target_link_libraries(process_runner_test PRIVATE rsync_assistant_core)
add_test(NAME process_runner_test COMMAND process_runner_test)

add_executable(settings_test tests/settings_test.cpp)
target_link_libraries(settings_test PRIVATE rsync_assistant_core)
add_test(NAME settings_test COMMAND settings_test)

add_executable(project_profile_test tests/project_profile_test.cpp)
target_link_libraries(project_profile_test PRIVATE rsync_assistant_core)
add_test(NAME project_profile_test COMMAND project_profile_test)

add_executable(endpoint_test tests/endpoint_test.cpp)
target_link_libraries(endpoint_test PRIVATE rsync_assistant_core)
add_test(NAME endpoint_test COMMAND endpoint_test)
