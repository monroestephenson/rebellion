cmake_minimum_required(VERSION 3.16)
project(rebellion
    VERSION 0.1.0
    DESCRIPTION "A Firecrawl-style web scraper in C"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(REBELLION_BUILD_TESTS "Build the test suite" ON)
option(REBELLION_BUILD_CLI "Build the rebellion CLI" ON)
option(REBELLION_WERROR "Treat warnings as errors" OFF)
option(REBELLION_HARDEN "Enable hardening compile/link flags" ON)
option(REBELLION_FUZZ "Build libFuzzer targets (needs clang)" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# strcasestr / nanosleep / clock_gettime live behind feature-test macros.
add_compile_definitions(_GNU_SOURCE)

# ---- dependencies ----------------------------------------------------------
find_package(CURL REQUIRED)

# iconv: on glibc it lives in libc; elsewhere it is a separate library.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES_SAVED "${CMAKE_REQUIRED_LIBRARIES}")
check_c_source_compiles("
#include <iconv.h>
int main(void){ iconv_t c = iconv_open(\"UTF-8\",\"ISO-8859-1\"); iconv_close(c); return 0; }
" ICONV_IN_LIBC)
if(NOT ICONV_IN_LIBC)
    find_library(ICONV_LIB iconv)
endif()

find_package(Threads REQUIRED)

# ---- library ---------------------------------------------------------------
add_library(rebellion
    src/util.c
    src/client.c
    src/url.c
    src/net.c
    src/charset.c
    src/http.c
    src/entities.c
    src/html.c
    src/markdown.c
    src/extract.c
    src/robots.c
    src/browser.c
    src/json.c
    src/scrape.c
    src/crawl.c
)

add_library(rebellion::rebellion ALIAS rebellion)

target_include_directories(rebellion
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rebellion PUBLIC CURL::libcurl Threads::Threads)
if(ICONV_LIB)
    target_link_libraries(rebellion PUBLIC ${ICONV_LIB})
endif()

set_target_properties(rebellion PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET default)

set(REBELLION_WARNINGS -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings)
if(REBELLION_WERROR)
    list(APPEND REBELLION_WARNINGS -Werror)
endif()

# Hardening flags (release-oriented, but safe everywhere).
set(REBELLION_HARDEN_C "")
set(REBELLION_HARDEN_LD "")
if(REBELLION_HARDEN)
    include(CheckCCompilerFlag)
    foreach(flag -fstack-protector-strong -fstack-clash-protection
                 -D_FORTIFY_SOURCE=2 -fPIC)
        string(MAKE_C_IDENTIFIER "HAVE${flag}" _ok)
        check_c_compiler_flag("${flag}" ${_ok})
        if(${_ok})
            list(APPEND REBELLION_HARDEN_C "${flag}")
        endif()
    endforeach()
    if(NOT APPLE)
        list(APPEND REBELLION_HARDEN_LD "-Wl,-z,relro,-z,now")
    endif()
endif()

target_compile_options(rebellion PRIVATE ${REBELLION_WARNINGS} ${REBELLION_HARDEN_C})
target_link_options(rebellion PRIVATE ${REBELLION_HARDEN_LD})

# ---- CLI -------------------------------------------------------------------
if(REBELLION_BUILD_CLI)
    add_executable(rebellion-cli src/main.c)
    set_target_properties(rebellion-cli PROPERTIES OUTPUT_NAME rebellion)
    target_link_libraries(rebellion-cli PRIVATE rebellion)
    target_compile_options(rebellion-cli PRIVATE ${REBELLION_WARNINGS} ${REBELLION_HARDEN_C})
    target_link_options(rebellion-cli PRIVATE ${REBELLION_HARDEN_LD})
endif()

# ---- tests -----------------------------------------------------------------
if(REBELLION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- fuzzers ---------------------------------------------------------------
if(REBELLION_FUZZ)
    add_subdirectory(fuzz)
endif()

# ---- install / packaging ---------------------------------------------------
install(TARGETS rebellion
    EXPORT rebellionTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(REBELLION_BUILD_CLI)
    install(TARGETS rebellion-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES docs/rebellion.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 OPTIONAL)
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# pkg-config (relocatable: prefix is resolved relative to the .pc location)
file(RELATIVE_PATH REBELLION_PC_REL
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    "${CMAKE_INSTALL_PREFIX}")
string(REGEX REPLACE "/+$" "" REBELLION_PC_REL "${REBELLION_PC_REL}")
if(REBELLION_PC_REL STREQUAL "")
    set(REBELLION_PC_REL ".")
endif()
configure_file(cmake/librebellion.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/librebellion.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/librebellion.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# CMake package config (find_package(rebellion))
set(REBELLION_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/rebellion)
install(EXPORT rebellionTargets
    FILE rebellionTargets.cmake
    NAMESPACE rebellion::
    DESTINATION ${REBELLION_CMAKEDIR})
configure_package_config_file(cmake/rebellion-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rebellion-config.cmake
    INSTALL_DESTINATION ${REBELLION_CMAKEDIR})
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rebellion-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rebellion-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rebellion-config-version.cmake
    DESTINATION ${REBELLION_CMAKEDIR})
