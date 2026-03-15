cmake_minimum_required(VERSION 3.16)

# Find Qt Test module
find_package(Qt6 REQUIRED COMPONENTS Core Test Sql)

# Enable testing
enable_testing()

# Utility sources needed by models (DataStore is now a dependency of Spectrum/Photometry)
set(UTIL_SOURCES
    ${CMAKE_SOURCE_DIR}/src/utils/DataStore.cpp
)

# Source files needed for tests
set(MODEL_SOURCES
    ${CMAKE_SOURCE_DIR}/src/models/Star.cpp
    ${CMAKE_SOURCE_DIR}/src/models/Project.cpp
    ${CMAKE_SOURCE_DIR}/src/models/Photometry.cpp
    ${CMAKE_SOURCE_DIR}/src/models/Spectrum.cpp
    ${CMAKE_SOURCE_DIR}/src/models/RadialVelocity.cpp
    ${CMAKE_SOURCE_DIR}/src/models/Instrument.cpp
    ${UTIL_SOURCES}
)

# Test for Star model
qt_add_executable(test_star
    test_star.cpp
    ${MODEL_SOURCES}
)
target_link_libraries(test_star PRIVATE
    Qt6::Core
    Qt6::Test
)
target_include_directories(test_star PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_star COMMAND test_star)

# Test for Project model
qt_add_executable(test_project
    test_project.cpp
    ${MODEL_SOURCES}
)
target_link_libraries(test_project PRIVATE
    Qt6::Core
    Qt6::Test
)
target_include_directories(test_project PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_project COMMAND test_project)
