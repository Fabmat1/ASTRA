cmake_minimum_required(VERSION 3.16)
project(ASTRA VERSION 0.1.0 LANGUAGES CXX)

# Configurable data directory (set at install/build time)
# Usage: cmake -DASTRA_DATA_DIR=/opt/astra/data ..
# Default: empty string → falls back to QStandardPaths::AppDataLocation at runtime
set(ASTRA_DATA_DIR "" CACHE PATH "Compile-time default for data/db/log storage directory")
message(STATUS "ASTRA data storage location set to: ${ASTRA_DATA_DIR}")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find required Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql Network Concurrent PrintSupport)
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

add_library(qcustomplot STATIC
    src/plotting/qcustomplot.cpp
    src/plotting/qcustomplot.h
)
target_link_libraries(qcustomplot PUBLIC
    Qt6::Core
    Qt6::Widgets
    Qt6::PrintSupport
)
target_include_directories(qcustomplot PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plotting
)
set_target_properties(qcustomplot PROPERTIES AUTOMOC ON)

# Source files
set(SOURCES
    src/main.cpp

    # controllers
    src/controllers/ApplicationController.cpp

    # db
    src/db/DatabaseManager.cpp
    src/db/DBAccess.cpp
    src/db/InstrumentRepository.cpp
    src/db/PhotometryRepository.cpp
    src/db/ProjectRepository.cpp
    src/db/RadialVelocityRepository.cpp
    src/db/SpectrumRepository.cpp
    src/db/StarRepository.cpp

    # dialogs
    src/dialogs/EditProjectDialog.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/SettingsDialog.cpp

    # importWizard
    src/importWizard/GeneralImportPage.cpp
    src/importWizard/ImportStagingArea.cpp
    src/importWizard/LightcurveImportPage.cpp
    src/importWizard/RadialVelocityImportPage.cpp
    src/importWizard/RVPhotometryPages.cpp
    src/importWizard/SEDImportPage.cpp
    src/importWizard/SpectraImportPage.cpp
    src/importWizard/SpectralFitImportPage.cpp
    src/importWizard/StarImportWizard.cpp

    # models
    src/models/BarycentricCorrection.cpp
    src/models/ColumnPreset.cpp
    src/models/Instrument.cpp
    src/models/InstrumentMode.cpp
    src/models/Photometry.cpp
    src/models/Project.cpp
    src/models/RadialVelocity.cpp
    src/models/Spectrum.cpp
    src/models/Star.cpp
    src/models/Time.cpp

    # utils
    src/utils/AppSettings.cpp
    src/utils/BackgroundTaskManager.cpp
    src/utils/CatalogQueryWorkers.cpp
    src/utils/CoastlineData.cpp
    src/utils/CrossRefResolver.cpp
    src/utils/DataStore.cpp
    src/utils/ExtractSED.cpp
    src/utils/Logger.cpp
    src/utils/SpectrumReader.cpp
    src/utils/ThemeManager.cpp

    # views
    src/views/BooleanColumnDelegate.cpp
    src/views/ColumnConfigDialog.cpp
    src/views/InstrumentConfigView.cpp
    src/views/MainWindow.cpp
    src/views/ProjectSelectionView.cpp
    src/views/ProjectView.cpp
    src/views/StarDetailView.cpp
    src/views/StarFilterWidget.cpp

    # views/tools
    src/views/tools/CMDDialog.cpp
    src/views/tools/GalacticOrbitDialog.cpp
    src/views/tools/LightcurveFetchDialog.cpp
    src/views/tools/RVInspectorDialog.cpp
    src/views/tools/SEDFitDialog.cpp
    src/views/tools/SpectraFitDialog.cpp

    # views/panels
    src/views/panels/PanelUtils.cpp
    src/views/panels/DetailPanel.cpp
    src/views/panels/DetailPanelFactory.cpp
    src/views/panels/SummaryPanel.cpp
    src/views/panels/RVPanel.cpp
    src/views/panels/LCPanel.cpp
    src/views/panels/SpectraPanel.cpp
)

# Header files
set(HEADERS
    # controllers
    src/controllers/ApplicationController.h

    # db
    src/db/DatabaseManager.h
    src/db/DBAccess.h
    src/db/InstrumentRepository.h
    src/db/PhotometryRepository.h
    src/db/ProjectRepository.h
    src/db/RadialVelocityRepository.h
    src/db/SpectrumRepository.h
    src/db/StarRepository.h

    # dialogs
    src/dialogs/EditProjectDialog.h
    src/dialogs/NewProjectDialog.h
    src/dialogs/SettingsDialog.h

    # importWizard
    src/importWizard/GeneralImportPage.h
    src/importWizard/ImportStagingArea.h
    src/importWizard/LightcurveImportPage.h
    src/importWizard/RadialVelocityImportPage.h
    src/importWizard/RVPhotometryPages.h
    src/importWizard/SEDImportPage.h
    src/importWizard/SpectraImportPage.h
    src/importWizard/SpectralFitImportPage.h
    src/importWizard/StarImportWizard.h

    # models
    src/models/BarycentricCorrection.h
    src/models/ColumnPreset.h
    src/models/Instrument.h
    src/models/InstrumentMode.h
    src/models/Photometry.h
    src/models/Project.h
    src/models/RadialVelocity.h
    src/models/Spectrum.h
    src/models/Star.h
    src/models/Time.h

    # utils
    src/utils/AppPaths.h
    src/utils/AppSettings.h
    src/utils/BackgroundTaskManager.h
    src/utils/CatalogQueryWorkers.h
    src/utils/CoastlineData.h
    src/utils/CrossRefResolver.h
    src/utils/DataStore.h
    src/utils/ExtractSED.h
    src/utils/Logger.h
    src/utils/SpectrumReader.h
    src/utils/ThemeManager.h

    # views
    src/views/BooleanColumnDelegate.h
    src/views/ColumnConfigDialog.h
    src/views/InstrumentConfigView.h
    src/views/MainWindow.h
    src/views/ProjectSelectionView.h
    src/views/ProjectView.h
    src/views/StarDetailView.h
    src/views/StarFilterWidget.h

    # views/tools
    src/views/tools/CMDDialog.h
    src/views/tools/GalacticOrbitDialog.h
    src/views/tools/LightcurveFetchDialog.h
    src/views/tools/RVInspectorDialog.h
    src/views/tools/SEDFitDialog.h
    src/views/tools/SpectraFitDialog.h

    # views/panels
    src/views/panels/PanelUtils.h
    src/views/panels/DetailPanel.h
    src/views/panels/DetailPanelFactory.h
    src/views/panels/SummaryPanel.h
    src/views/panels/RVPanel.h
    src/views/panels/LCPanel.h
    src/views/panels/SpectraPanel.h

)

# Create executable
qt_add_executable(ASTRA
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

# Pass to source as a preprocessor define
target_compile_definitions(ASTRA PRIVATE
    ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
)

# Profiling option
option(ENABLE_PROFILING "Build with profiling support (debug symbols + frame pointers)" OFF)

if(ENABLE_PROFILING)
    message(STATUS "Profiling enabled: -g -fno-omit-frame-pointer")
    set(PROFILING_FLAGS -O2 -g -fno-omit-frame-pointer)

    target_compile_options(ASTRA PRIVATE ${PROFILING_FLAGS})
    target_compile_options(qcustomplot PRIVATE ${PROFILING_FLAGS})

    # Export all symbol names so perf can resolve them
    target_link_options(ASTRA PRIVATE -rdynamic)
endif()

# Set up include directories
target_include_directories(ASTRA PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link libraries
target_link_libraries(ASTRA PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Sql
    Qt6::Network
    Qt6::Concurrent
    Qt6::PrintSupport
    qcustomplot
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


# ═════════════════════════════════════════════════════════════════════════════
# Tests
# ═════════════════════════════════════════════════════════════════════════════
option(BUILD_TESTING "Build test suite" ON)

if(BUILD_TESTING)
    enable_testing()

    # ── Core library (model sources needed by tests, without GUI) ────────
    # Avoids linking the entire application + all Qt Widgets dependencies.
    add_library(astra_core STATIC
        src/models/BarycentricCorrection.cpp
        src/models/Instrument.cpp
        src/models/Time.cpp
    )
    target_include_directories(astra_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(astra_core PUBLIC
        Qt6::Core
    )
    set_target_properties(astra_core PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        AUTOMOC ON
    )

    # ── Reference data generation ────────────────────────────────────────
    find_package(Python3 COMPONENTS Interpreter)

    set(REFERENCE_JSON "${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_bjd.json")
    set(GENERATE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_reference_bjd.py")

    if(Python3_FOUND)
        # Custom target so you can run:  cmake --build build --target generate_reference
        add_custom_command(
            OUTPUT  "${REFERENCE_JSON}"
            COMMAND "${Python3_EXECUTABLE}" "${GENERATE_SCRIPT}" "${REFERENCE_JSON}"
            DEPENDS "${GENERATE_SCRIPT}"
            COMMENT "Generating BJD reference data with astropy..."
            VERBATIM
        )
        add_custom_target(generate_reference
            DEPENDS "${REFERENCE_JSON}"
        )
    else()
        message(WARNING "Python3 not found – cannot auto-generate reference_bjd.json. "
                        "Run manually: python3 tests/generate_reference_bjd.py")
    endif()

    # ── Barycentric correction test ──────────────────────────────────────
    add_executable(test_barycentric
        tests/test_barycentric.cpp
    )
    target_link_libraries(test_barycentric PRIVATE
        astra_core
        Qt6::Core
    )
    # Tell the test where to find the JSON at compile time
    target_compile_definitions(test_barycentric PRIVATE
        REFERENCE_BJD_PATH="${REFERENCE_JSON}"
    )
    set_target_properties(test_barycentric PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    # Ensure reference data is generated before the test runs
    if(TARGET generate_reference)
        add_dependencies(test_barycentric generate_reference)
    endif()

    # Register with CTest
    add_test(
        NAME    barycentric_correction
        COMMAND test_barycentric "${REFERENCE_JSON}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(barycentric_correction PROPERTIES
        LABELS "models;astronomy"
        TIMEOUT 30
    )

    # ── Convenience target ───────────────────────────────────────────────
    # `cmake --build build --target check` builds and runs all tests
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS test_barycentric
        COMMENT "Running all tests..."
    )
endif()