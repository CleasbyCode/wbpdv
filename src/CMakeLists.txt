cmake_minimum_required(VERSION 3.20)

project(wbpdv LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "wbpdv requires Linux.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "wbpdv requires GCC 14 or newer.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "wbpdv requires Clang 18 or newer with a compatible C++23 standard library.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
  "#include <format>\n#include <print>\nint main() { std::println(\"{}\", std::format(\"{}\", 1)); }"
  WBPDV_HAS_REQUIRED_CXX23_LIBRARY
)
if(NOT WBPDV_HAS_REQUIRED_CXX23_LIBRARY)
  message(FATAL_ERROR "The selected compiler and standard library do not provide the required C++23 <print>/<format> support.")
endif()

set(WBPDV_BUILD_MODE "release" CACHE STRING "wbpdv build mode")
set_property(CACHE WBPDV_BUILD_MODE PROPERTY STRINGS release sanitize)
if(NOT WBPDV_BUILD_MODE MATCHES "^(release|sanitize)$")
  message(FATAL_ERROR "WBPDV_BUILD_MODE must be release or sanitize.")
endif()

set(WBPDV_FORTIFY_LEVEL "3" CACHE STRING "_FORTIFY_SOURCE level for release builds")
set_property(CACHE WBPDV_FORTIFY_LEVEL PROPERTY STRINGS 2 3)
if(NOT WBPDV_FORTIFY_LEVEL MATCHES "^[23]$")
  message(FATAL_ERROR "WBPDV_FORTIFY_LEVEL must be 2 or 3.")
endif()

# libwebp must come from pkg-config. This deliberately avoids guessing a
# dependency location (especially a predictable writable path under /tmp).
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(LIBDEFLATE REQUIRED IMPORTED_TARGET libdeflate)
find_package(ZLIB REQUIRED)

add_executable(wbpdv
  args.cpp
  base64.cpp
  compression.cpp
  conceal.cpp
  encryption.cpp
  image.cpp
  io_utils.cpp
  main.cpp
  recover.cpp
)

target_compile_options(wbpdv PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wformat
  -Wformat-security
)

target_link_libraries(wbpdv PRIVATE
  PkgConfig::WEBP
  PkgConfig::SODIUM
  PkgConfig::LIBDEFLATE
  ZLIB::ZLIB
)

if(WBPDV_BUILD_MODE STREQUAL "release")
  target_compile_options(wbpdv PRIVATE
    -O3
    # This project is built locally: native tuning may make the resulting
    # binary unsuitable for a different or older CPU.
    -march=native
    -pipe
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection=full
    -fPIE
  )
  target_compile_definitions(wbpdv PRIVATE
    NDEBUG
    _GLIBCXX_ASSERTIONS
    "_FORTIFY_SOURCE=${WBPDV_FORTIFY_LEVEL}"
  )
  target_link_options(wbpdv PRIVATE
    -s
    -pie
    -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code
  )

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(wbpdv PRIVATE -fuse-linker-plugin)
    target_link_options(wbpdv PRIVATE -fuse-linker-plugin)
  endif()

  include(CheckIPOSupported)
  check_ipo_supported(RESULT WBPDV_IPO_SUPPORTED OUTPUT WBPDV_IPO_ERROR LANGUAGES CXX)
  if(NOT WBPDV_IPO_SUPPORTED)
    message(FATAL_ERROR "The selected compiler does not support LTO: ${WBPDV_IPO_ERROR}")
  endif()
  set_property(TARGET wbpdv PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  target_compile_options(wbpdv PRIVATE
    -O1
    -g3
    -fno-omit-frame-pointer
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -fstack-protector-strong
  )
  target_compile_definitions(wbpdv PRIVATE _GLIBCXX_ASSERTIONS)
  target_link_options(wbpdv PRIVATE -fsanitize=address,undefined)
endif()
