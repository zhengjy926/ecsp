cmake_minimum_required(VERSION 3.16)

project(ecsp LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(ecsp_core STATIC
  src/ecsp/core/crc.c
  src/ecsp/core/bytes.c
  src/ecsp/core/frame.c
)
target_include_directories(ecsp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
  target_compile_options(ecsp_core PRIVATE /W4 /WX)
else()
  target_compile_options(ecsp_core PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()

add_executable(test_vectors test/unit/test_vectors.c)
target_link_libraries(test_vectors PRIVATE ecsp_core)
target_include_directories(test_vectors PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/test/vectors
)
if(MSVC)
  target_compile_options(test_vectors PRIVATE /W4 /WX)
else()
  target_compile_options(test_vectors PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME vectors COMMAND test_vectors)

# Vector/spec cross-check (PowerShell); runs under ctest as well.
find_program(POWERSHELL_EXECUTABLE powershell)
if(POWERSHELL_EXECUTABLE)
  add_test(NAME check-vectors
    COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-vectors.ps1)
else()
  message(WARNING "powershell not found; skipping check-vectors test")
endif()
