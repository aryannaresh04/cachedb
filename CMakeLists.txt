cmake_minimum_required(VERSION 3.16)
project(cachedb CXX)

# PROJECT.md 3: C++17, no compiler extensions -- CMAKE_CXX_EXTENSIONS OFF is
# what turns -std=gnu++17 into -std=c++17, so a GNU-only construct fails here
# rather than in a reviewer's build.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# PROJECT.md 3: debug builds run under ASan/UBSan. Default on so the sanitized
# build is the one you get by accident; -DCACHEDB_SANITIZE=OFF for benchmarking
# (sanitizers distort latency badly) or on hosts where ASan will not start.
option(CACHEDB_SANITIZE "Build with AddressSanitizer + UBSan" ON)

add_compile_options(-Wall -Wextra -Werror)

if(CACHEDB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# Vendored single-header test framework. SYSTEM on the include directory stops
# -Werror from failing the build on warnings in third-party code we do not own.
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# The engine, as a library so tests link the same objects the server does
# rather than recompiling sources with their own flags. Grows one file per
# component as M1-M4 land.
add_library(cachedb_core
  src/resp.cpp
)
target_include_directories(cachedb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()
add_subdirectory(tests)
