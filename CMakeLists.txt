cmake_minimum_required(VERSION 3.16)
project(ASTRA VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Charts Sql)
qt_standard_project_setup()

# Set automatic MOC, UIC and RCC for Qt
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source files
set(SOURCES
    src/main.cpp
    src/controllers/ApplicationController.cpp
    src/controllers/ProjectController.cpp
    src/models/Project.cpp
    src/models/Star.cpp
    src/models/Photometry.cpp
    src/models/Spectrum.cpp
    src/views/MainWindow.cpp
    src/views/ProjectSelectionView.cpp
    src/views/ProjectView.cpp
    src/views/StarDetailView.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/EditProjectDialog.cpp
    src/utils/DatabaseManager.cpp
)

# Header files
set(HEADERS
    src/controllers/ApplicationController.h
    src/controllers/ProjectController.h
    src/models/Project.h
    src/models/Star.h
    src/models/Photometry.h
    src/models/Spectrum.h
    src/views/MainWindow.h
    src/views/ProjectSelectionView.h
    src/views/ProjectView.h
    src/views/StarDetailView.h
    src/dialogs/NewProjectDialog.h
    src/dialogs/EditProjectDialog.h
    src/utils/DatabaseManager.h
)

# Create executable
qt_add_executable(ASTRA
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

# Link libraries
target_link_libraries(ASTRA PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Charts
    Qt6::Sql
)

# Include directories
target_include_directories(ASTRA PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Platform-specific settings
if(WIN32)
    set_target_properties(ASTRA PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
elseif(APPLE)
    set_target_properties(ASTRA PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    )
endif()

# Install targets
install(TARGETS ASTRA
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Testing
enable_testing()
add_subdirectory(tests)