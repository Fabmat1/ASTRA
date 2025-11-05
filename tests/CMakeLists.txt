cmake_minimum_required(VERSION 3.16)

# Find Qt Test module
find_package(Qt6 REQUIRED COMPONENTS Core Test)

# Enable testing
enable_testing()

# Define test executable
add_executable(astra_tests
    test_main.cpp
    test_star.cpp
    test_project.cpp
)

# Link test libraries
target_link_libraries(astra_tests PRIVATE
    Qt6::Core
    Qt6::Test
)

# Include directories
target_include_directories(astra_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Add tests
add_test(NAME astra_tests COMMAND astra_tests)