cmake_minimum_required(VERSION 3.16)
project(ASTRA VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Charts Sql Network)
qt_standard_project_setup()

# Set automatic MOC, UIC and RCC for Qt
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Export compile commands for IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find optional CCfits for FITS file support
# First try to find CCfits using pkg-config
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(CCFITS ccfits)
endif()

# If pkg-config didn't work, try to find CCfits manually
if(NOT CCFITS_FOUND)
    find_path(CCFITS_INCLUDE_DIR CCfits/CCfits.h
        HINTS
        /usr/include
        /usr/local/include
        /opt/local/include
        ${CMAKE_PREFIX_PATH}/include
    )
    
    find_library(CCFITS_LIBRARY
        NAMES CCfits ccfits
        HINTS
        /usr/lib
        /usr/local/lib
        /opt/local/lib
        ${CMAKE_PREFIX_PATH}/lib
    )
    
    find_library(CFITSIO_LIBRARY
        NAMES cfitsio
        HINTS
        /usr/lib
        /usr/local/lib
        /opt/local/lib
        ${CMAKE_PREFIX_PATH}/lib
    )
    
    if(CCFITS_INCLUDE_DIR AND CCFITS_LIBRARY AND CFITSIO_LIBRARY)
        set(CCFITS_FOUND TRUE)
        set(CCFITS_INCLUDE_DIRS ${CCFITS_INCLUDE_DIR})
        set(CCFITS_LIBRARIES ${CCFITS_LIBRARY} ${CFITSIO_LIBRARY})
        message(STATUS "Found CCfits: ${CCFITS_INCLUDE_DIRS}")
    endif()
endif()

if(CCFITS_FOUND)
    message(STATUS "CCfits support enabled")
else()
    message(WARNING "CCfits not found - FITS file support will be disabled")
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/controllers/ApplicationController.cpp
    src/controllers/ProjectController.cpp
    src/models/Project.cpp
    src/models/Star.cpp
    src/models/Photometry.cpp
    src/models/Spectrum.cpp
    src/models/Instrument.cpp
    src/models/RadialVelocity.cpp
    src/views/MainWindow.cpp
    src/views/ProjectSelectionView.cpp
    src/views/ProjectView.cpp
    src/views/StarDetailView.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/EditProjectDialog.cpp
    src/utils/StarImportWizard.cpp
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
    src/models/Instrument.h
    src/models/RadialVelocity.h
    src/views/MainWindow.h
    src/views/ProjectSelectionView.h
    src/views/ProjectView.h
    src/views/StarDetailView.h
    src/dialogs/NewProjectDialog.h
    src/dialogs/EditProjectDialog.h
    src/utils/StarImportWizard.h
    src/utils/DatabaseManager.h
)

# Create executable
qt_add_executable(ASTRA
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

# Set up include directories
target_include_directories(ASTRA PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link libraries
target_link_libraries(ASTRA PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Charts
    Qt6::Sql
    Qt6::Network
)

# Add CCfits support if available
if(CCFITS_FOUND)
    target_compile_definitions(ASTRA PRIVATE HAVE_CCFITS)
    target_include_directories(ASTRA PRIVATE ${CCFITS_INCLUDE_DIRS})
    target_link_libraries(ASTRA PRIVATE ${CCFITS_LIBRARIES})
    
    # On some systems, we might need to add additional math libraries
    if(UNIX AND NOT APPLE)
        target_link_libraries(ASTRA PRIVATE m)
    endif()
endif()

# Platform-specific settings
if(WIN32)
    set_target_properties(ASTRA PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    
    # On Windows, you might need to copy CCfits DLLs to the output directory
    if(CCFITS_FOUND AND CCFITS_RUNTIME_LIBRARIES)
        add_custom_command(TARGET ASTRA POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CCFITS_RUNTIME_LIBRARIES}
            $<TARGET_FILE_DIR:ASTRA>
        )
    endif()
elseif(APPLE)
    set_target_properties(ASTRA PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    )
    
    # On macOS, we might need to set the RPATH for CCfits
    if(CCFITS_FOUND)
        set_target_properties(ASTRA PROPERTIES
            INSTALL_RPATH "@executable_path/../Frameworks"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()
endif()

# Install targets
install(TARGETS ASTRA
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Testing
enable_testing()
add_subdirectory(tests)