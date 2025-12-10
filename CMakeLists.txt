# Set version requirement
cmake_minimum_required(VERSION 3.16)

# Define project
project(zip-test
    VERSION 0.0.1
    LANGUAGES C CXX)

# Export compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set default to Debug
if("${CMAKE_BUILD_TYPE}" STREQUAL "")
    set(CMAKE_BUILD_TYPE Debug)
endif()
message("Build type: ${CMAKE_BUILD_TYPE}")

# Create list of all source files
set (SOURCE_FILES
    "src/main.cpp"
)

# Enforce shared build
set(BUILD_SHARED_LIBS OFF)

# Disable all options of libarchive
set(ENABLE_TEST OFF)
set(ENABLE_LIBB2 OFF)
set(ENABLE_LZ4 OFF)
set(ENABLE_LZO OFF)
set(ENABLE_LZMA OFF)
set(ENABLE_ZSTD OFF)
set(ENABLE_BZip2 OFF)
set(ENABLE_LIBXML2 OFF)
set(ENABLE_EXPAT OFF)
set(ENABLE_PCREPOSIX OFF)
set(ENABLE_PCRE2POSIX OFF)
set(ENABLE_LIBGCC OFF)
# Only enable ZLIB support
set(ENABLE_ZLIB ON)

# Enforce static linkage of zlib
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)

# Add libarchive dependency
add_subdirectory(deps/libarchive EXCLUDE_FROM_ALL)

# Link executable
add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PRIVATE -static archive_static ZLIB::ZLIB)
