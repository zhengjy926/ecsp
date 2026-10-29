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
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

add_library(ecsp_core STATIC
  src/ecsp/core/crc.c
  src/ecsp/core/bytes.c
  src/ecsp/core/frame.c
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

add_executable(test_slave test/unit/test_slave.c)
target_link_libraries(test_slave PRIVATE ecsp_core)
ecsp_enable_warnings(test_slave)

add_test(NAME frame COMMAND test_frame)
add_test(NAME slave COMMAND test_slave)

# Vector/spec cross-check (PowerShell); runs under ctest as well.
find_program(POWERSHELL_EXECUTABLE powershell)
if(POWERSHELL_EXECUTABLE)
  add_test(NAME check-vectors
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-vectors.ps1)
else()
  message(WARNING "powershell not found; skipping check-vectors test")
endif()
