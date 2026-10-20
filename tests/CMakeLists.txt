add_executable(rebellion_tests
    test_main.c
    test_markdown.c
    test_charset.c
    test_entities.c
    test_url.c
    test_robots.c
    test_html.c
    test_proxy.c
    test_integration.c
)
target_link_libraries(rebellion_tests PRIVATE rebellion)
target_include_directories(rebellion_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include)
target_compile_options(rebellion_tests PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND rebellion_tests)
