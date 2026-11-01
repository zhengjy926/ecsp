cmake_minimum_required(VERSION 3.16)

project(ecsp LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Emit compile_commands.json for clangd (F12 / IntelliSense).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every target compiles warning-free (parent ticket #1, "编译零警告").
function(ecsp_enable_warnings target)
  if(MSVC)
    # /utf-8: sources contain UTF-8 comments; code page 936 otherwise errors
    # under /WX (C4819).
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

add_library(ecsp_core STATIC
  src/ecsp/core/crc.c
  src/ecsp/core/bytes.c
  src/ecsp/core/frame.c
  src/ecsp/core/cmds.c
  src/ecsp/slave/txn_cache.c
  src/ecsp/slave/session.c
  src/ecsp/slave/long_run.c
  src/ecsp/slave/engine.c
  src/ecsp/host/link.c
)
target_include_directories(ecsp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
ecsp_enable_warnings(ecsp_core)

enable_testing()

add_executable(test_vectors test/unit/test_vectors.c)
target_link_libraries(test_vectors PRIVATE ecsp_core)
target_include_directories(test_vectors PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test/vectors
)
ecsp_enable_warnings(test_vectors)

add_test(NAME vectors COMMAND test_vectors)

add_executable(test_frame test/unit/test_frame.c)
target_link_libraries(test_frame PRIVATE ecsp_core)
ecsp_enable_warnings(test_frame)

add_executable(test_cmds test/unit/test_cmds.c)
target_link_libraries(test_cmds PRIVATE ecsp_core)
ecsp_enable_warnings(test_cmds)

add_executable(test_slave test/unit/test_slave.c)
target_link_libraries(test_slave PRIVATE ecsp_core)
ecsp_enable_warnings(test_slave)

add_executable(test_pipeline test/unit/test_pipeline.c)
target_link_libraries(test_pipeline PRIVATE ecsp_core)
ecsp_enable_warnings(test_pipeline)

add_executable(test_txn_cache test/unit/test_txn_cache.c)
target_link_libraries(test_txn_cache PRIVATE ecsp_core)
ecsp_enable_warnings(test_txn_cache)

add_executable(test_session test/unit/test_session.c)
target_link_libraries(test_session PRIVATE ecsp_core)
ecsp_enable_warnings(test_session)

add_executable(test_long_run test/unit/test_long_run.c)
target_link_libraries(test_long_run PRIVATE ecsp_core)
ecsp_enable_warnings(test_long_run)

add_executable(test_upload test/unit/test_upload.c)
target_link_libraries(test_upload PRIVATE ecsp_core)
ecsp_enable_warnings(test_upload)

add_executable(test_host test/unit/test_host.c)
target_link_libraries(test_host PRIVATE ecsp_core)
ecsp_enable_warnings(test_host)

# Same sources as ecsp_core, with only 0x07's ceiling loosened. The shared
# 50 ms default is untouched, which is what test_reset_ceiling observes.
get_target_property(ECSP_CORE_SOURCES ecsp_core SOURCES)
add_library(ecsp_core_loose_reset STATIC ${ECSP_CORE_SOURCES})
target_include_directories(ecsp_core_loose_reset PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(ecsp_core_loose_reset PRIVATE ECSP_RESP_RESET_MS=200u)
ecsp_enable_warnings(ecsp_core_loose_reset)

add_executable(test_reset_ceiling test/unit/test_reset_ceiling.c)
target_link_libraries(test_reset_ceiling PRIVATE ecsp_core_loose_reset)
ecsp_enable_warnings(test_reset_ceiling)

add_test(NAME frame COMMAND test_frame)
add_test(NAME cmds COMMAND test_cmds)
add_test(NAME slave COMMAND test_slave)
add_test(NAME pipeline COMMAND test_pipeline)
add_test(NAME txn_cache COMMAND test_txn_cache)
add_test(NAME session COMMAND test_session)
add_test(NAME long_run COMMAND test_long_run)
add_test(NAME upload COMMAND test_upload)
add_test(NAME host COMMAND test_host)
add_test(NAME reset_ceiling COMMAND test_reset_ceiling)

add_executable(test_two_instance test/integ/two_instance.c)
target_link_libraries(test_two_instance PRIVATE ecsp_core)
target_include_directories(test_two_instance PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test/unit
  ${CMAKE_CURRENT_SOURCE_DIR}/test/vectors
)
ecsp_enable_warnings(test_two_instance)

add_test(NAME two_instance COMMAND test_two_instance)

# Board command table (ticket #15). It is not part of ecsp_core: the protocol
# library has no 0x30-0x33 semantics. The test is the same virtual-link seam.
add_library(f429_board STATIC
  board/f429/bringup.c
)
target_link_libraries(f429_board PUBLIC ecsp_core)
target_include_directories(f429_board PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/board/f429
)
ecsp_enable_warnings(f429_board)

add_executable(test_board_cmds test/integ/board_cmds.c)
target_link_libraries(test_board_cmds PRIVATE f429_board)
target_include_directories(test_board_cmds PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test/unit
)
ecsp_enable_warnings(test_board_cmds)

add_test(NAME board_cmds COMMAND test_board_cmds)

# Vector/spec cross-check (PowerShell); runs under ctest as well.
find_program(POWERSHELL_EXECUTABLE powershell)
if(POWERSHELL_EXECUTABLE)
  add_test(NAME check-vectors
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-vectors.ps1)
else()
  message(WARNING "powershell not found; skipping check-vectors test")
endif()
