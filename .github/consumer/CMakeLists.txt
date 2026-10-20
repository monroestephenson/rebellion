cmake_minimum_required(VERSION 3.16)
project(consumer C)

# Verifies that an installed rebellion is consumable via find_package.
find_package(rebellion REQUIRED)

add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE rebellion::rebellion)
