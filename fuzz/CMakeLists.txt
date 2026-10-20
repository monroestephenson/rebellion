# Fuzz targets. Build with clang and fuzzer instrumentation, e.g.:
#
#   cmake -S . -B build-fuzz -DREBELLION_FUZZ=ON -DREBELLION_BUILD_TESTS=OFF \
#     -DCMAKE_C_COMPILER=clang \
#     -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -g -O1"
#   cmake --build build-fuzz -j
#   ./build-fuzz/fuzz/fuzz_html -max_total_time=60 corpus/
#
# The library picks up coverage/sanitizer instrumentation from CMAKE_C_FLAGS;
# the target below just adds the libFuzzer driver.

foreach(target fuzz_html fuzz_json)
    add_executable(${target} ${target}.c)
    target_link_libraries(${target} PRIVATE rebellion)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
