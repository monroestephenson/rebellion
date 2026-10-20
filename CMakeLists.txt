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
option(REBELLION_WERROR "Treat warnings as errors" OFF)

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

target_include_directories(rebellion
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rebellion PUBLIC CURL::libcurl Threads::Threads)
if(ICONV_LIB)
    target_link_libraries(rebellion PUBLIC ${ICONV_LIB})
endif()

set(REBELLION_WARNINGS -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings)
if(REBELLION_WERROR)
    list(APPEND REBELLION_WARNINGS -Werror)
endif()
target_compile_options(rebellion PRIVATE ${REBELLION_WARNINGS})

# ---- CLI -------------------------------------------------------------------
add_executable(rebellion-cli src/main.c)
set_target_properties(rebellion-cli PROPERTIES OUTPUT_NAME rebellion)
target_link_libraries(rebellion-cli PRIVATE rebellion)
target_compile_options(rebellion-cli PRIVATE ${REBELLION_WARNINGS})

# ---- tests -----------------------------------------------------------------
if(REBELLION_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS rebellion rebellion-cli
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
