cmake_minimum_required(VERSION 3.16)

project(ecsp LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

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

add_test(NAME frame COMMAND test_frame)
add_test(NAME cmds COMMAND test_cmds)
add_test(NAME slave COMMAND test_slave)
add_test(NAME pipeline COMMAND test_pipeline)
add_test(NAME txn_cache COMMAND test_txn_cache)
add_test(NAME session COMMAND test_session)
add_test(NAME long_run COMMAND test_long_run)

# Vector/spec cross-check (PowerShell); runs under ctest as well.
find_program(POWERSHELL_EXECUTABLE powershell)
if(POWERSHELL_EXECUTABLE)
  add_test(NAME check-vectors
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-vectors.ps1)
else()
  message(WARNING "powershell not found; skipping check-vectors test")
endif()
