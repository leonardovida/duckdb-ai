cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME ai)

# DuckDB's extension distribution supports vcpkg. Dependencies can be added in
# ./vcpkg.json and then used in cmake with find_package.
find_package(CURL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/duckdb_ai_extension.cpp
    src/duckdb_ai_provider.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

if(WIN32)
  # curl.h pulls in windows.h, whose min/max macros break std::min/std::max and
  # std::numeric_limits<T>::max() under MSVC.
  target_compile_definitions(${EXTENSION_NAME} PRIVATE NOMINMAX)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE NOMINMAX)
endif()

if(TARGET CURL::libcurl)
  target_link_libraries(${EXTENSION_NAME} CURL::libcurl)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} CURL::libcurl)
else()
  target_include_directories(${EXTENSION_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
  target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
  target_link_libraries(${EXTENSION_NAME} ${CURL_LIBRARIES})
  target_link_libraries(${LOADABLE_EXTENSION_NAME} ${CURL_LIBRARIES})
endif()

target_link_libraries(${EXTENSION_NAME} duckdb_yyjson)
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_yyjson)
target_link_libraries(${EXTENSION_NAME} duckdb_re2)
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_re2)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# Instrument project-owned extension code without requiring a sanitized DuckDB core.
option(AI_ENABLE_SANITIZERS "Enable address/undefined sanitizers on the loadable AI extension" OFF)
if(AI_ENABLE_SANITIZERS)
  if(NOT EXTENSION_STATIC_BUILD)
    message(FATAL_ERROR "AI_ENABLE_SANITIZERS requires EXTENSION_STATIC_BUILD=1 for DuckDB RTTI linkage")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "AI_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
  target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_libraries(${LOADABLE_EXTENSION_NAME} -fsanitize=address,undefined)
endif()
