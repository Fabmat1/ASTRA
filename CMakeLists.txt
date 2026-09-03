cmake_minimum_required(VERSION 3.21)   # PROJECT_IS_TOP_LEVEL needs ≥3.21
project(ASTRA VERSION 0.2.2 LANGUAGES CXX)

# ── Build type (refuse typos like "Relase") ─────────────────────────────────
set(_allowed_build_types Debug Release RelWithDebInfo MinSizeRel)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE IN_LIST _allowed_build_types)
    message(FATAL_ERROR
        "Invalid CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. "
        "Allowed: ${_allowed_build_types}")
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${_allowed_build_types})

# ── Single, project-wide ABI for every target (host + submodules) ───────────
# GAEL requires C++20, so the whole project is C++20.
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # use -std=c++20 (NOT gnu++20) everywhere

# Pick a baseline ISA that covers desktop + laptop. Avoid -march=native.
# x86 only: these tokens are rejected outright by Apple Clang on arm64, so the
# guard keeps the macOS build from having to patch (and thereby dirty) the
# working tree — a dirty tree makes GitVersion.cmake stamp "git-<hash>-dirty"
# instead of the release tag, which in turn disables the updater.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    add_compile_options(
        -mtune=generic
        -march=x86-64-v3    # AVX2/FMA. Use x86-64-v2 if you support older CPUs.
    )
endif()

# Pin Eigen ABI so allocations made in one TU can be safely freed in another.
add_compile_definitions(
    EIGEN_USE_BLAS
    EIGEN_USE_LAPACKE
    EIGEN_MAX_ALIGN_BYTES=32
    EIGEN_DEFAULT_ALIGN_BYTES=32
)

# Configurable data dir
set(ASTRA_DATA_DIR "" CACHE PATH
    "Compile-time default for data/db/log storage directory")
message(STATUS "ASTRA data storage location set to: ${ASTRA_DATA_DIR}")

# Qt
find_package(Qt6 6.10 REQUIRED COMPONENTS
    Core Widgets Sql Network Concurrent PrintSupport Svg)
qt_standard_project_setup()

set(LCQUERY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/lightcurvequery")
set(LCQUERY_SCRIPT "${LCQUERY_DIR}/lightcurvequery.py")
set(LCQUERY_REQS   "${LCQUERY_DIR}/requirements.txt")

if(EXISTS "${LCQUERY_SCRIPT}")
    message(STATUS "lightcurvequery script: ${LCQUERY_SCRIPT}")
    set(ASTRA_HAVE_LCQUERY TRUE)
else()
    message(WARNING
        "lightcurvequery submodule missing - fetch feature will refuse to launch.\n"
        "  git submodule add https://github.com/Fabmat1/lightcurvequery external/lightcurvequery\n"
        "  git submodule update --init --recursive")
    set(ASTRA_HAVE_LCQUERY FALSE)
endif()

# Optional convenience target:  cmake --build build --target lcquery_deps
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND EXISTS "${LCQUERY_REQS}")
    add_custom_target(lcquery_deps
        COMMAND "${Python3_EXECUTABLE}" -m pip install --user -r "${LCQUERY_REQS}"
        WORKING_DIRECTORY "${LCQUERY_DIR}"
        COMMENT "Installing lightcurvequery Python dependencies"
        VERBATIM)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Compile-commands for IDEs and the diff/sanity script
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sane install RPATH so the installed binary doesn't pick up stale system .so's
include(GNUInstallDirs)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# ── lcurve fitting binaries (optional bundling) ─────────────────────────────
# Directory holding prebuilt lcurve_* binaries to ship inside the install tree
# / AppImage (e.g. your lcurve build output). When the binaries are present at
# configure time they are installed under <prefix>/<libexec>/astra/lcurve and
# resolved at runtime relative to the ASTRA binary, so users no longer need to
# build and install lcurve separately. When absent (the default), nothing is
# bundled and lcurve is resolved from the Settings dialog / PATH as before.
set(ASTRA_LCURVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/lcurve" CACHE PATH
    "Directory containing prebuilt lcurve_* binaries to bundle (optional)")

# lcurve_re is the forward-model binary; the fit dialog runs it to preview
# the model for the current initial guess.
set(_lcurve_binaries lcurve_levmarq lcurve_mcmc lcurve_simplex lcurve_re)
set(_lcurve_found_files "")
foreach(_b IN LISTS _lcurve_binaries)
    if(EXISTS "${ASTRA_LCURVE_DIR}/${_b}")
        list(APPEND _lcurve_found_files "${ASTRA_LCURVE_DIR}/${_b}")
    endif()
endforeach()
if(_lcurve_found_files)
    message(STATUS "lcurve binaries to bundle: ${_lcurve_found_files}")
else()
    message(STATUS
        "No lcurve binaries found in '${ASTRA_LCURVE_DIR}' - lcurve will be "
        "resolved from Settings/PATH at runtime. Set -DASTRA_LCURVE_DIR=<dir> "
        "to bundle them.")
endif()

# Path the runtime uses to locate bundled binaries relative to the ASTRA
# executable. Computed from the install layout so it is identical for an
# installed tree and a (relocated) AppImage.
file(RELATIVE_PATH ASTRA_LCURVE_BUNDLE_RELDIR
     "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_LIBEXECDIR}/astra/lcurve")

# ── SEDplusplus photometric SED fitting (bundled submodule) ─────────────────
# C++ replacement for the former ISIS-based SED fitting. The submodule builds
# the `sedfit` executable, which is installed under <libexec>/astra/sedfit and
# resolved at runtime relative to the ASTRA binary (same pattern as lcurve).
# Its filter reference data ships read-only under <datadir>/astra/sedfit.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/SEDplusplus/CMakeLists.txt")
    set(ASTRA_HAVE_SEDFIT TRUE)
else()
    message(WARNING
        "SEDplusplus submodule missing - SED fitting will only work with a "
        "user-installed sedfit binary.\n"
        "  git submodule update --init external/SEDplusplus")
    set(ASTRA_HAVE_SEDFIT FALSE)
endif()

file(RELATIVE_PATH ASTRA_SEDFIT_BUNDLE_RELDIR
     "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_LIBEXECDIR}/astra/sedfit")
file(RELATIVE_PATH ASTRA_SEDFIT_REFDATA_RELDIR
     "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_DATADIR}/astra/sedfit/refdata")

# Same idea for the bundled lightcurvequery Python sources. They are shipped as
# read-only data under <prefix>/share/astra/lightcurvequery; at runtime ASTRA
# unpacks them into a writable per-user dir and builds a venv next to them.
file(RELATIVE_PATH ASTRA_LCQUERY_BUNDLE_RELDIR
     "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_DATADIR}/astra/lightcurvequery")

# And for a bundled ISIS tree under <prefix>/share/astra/isis. ISIS only reads
# its tree, so it runs in place from the (read-only) AppImage; ASTRA points it
# there at runtime via ISIS_SRCDIR / SLSH_PATH / .isisrc. The tree itself is
# produced by the AppImage build (build-appimage.sh), not by `cmake --install`.
file(RELATIVE_PATH ASTRA_ISIS_BUNDLE_RELDIR
     "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_DATADIR}/astra/isis")
# Optional: point a dev build straight at an external ISIS prefix.
set(ASTRA_ISIS_DIR "" CACHE PATH
    "Optional ISIS prefix to resolve at runtime when not bundled (dev only)")

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

# In ASTRA's top-level CMakeLists.txt
# ASTRA_GAEL_SOURCE_DIR lets a developer build against an out-of-tree GAEL
# checkout (e.g. -DASTRA_GAEL_SOURCE_DIR=$HOME/Projects/GAEL) instead of the
# pinned submodule. Relative values are taken from the source root.
# Note: CMake resolves a relative PATH cache value against the working
# directory it was given on, so pass an absolute path when overriding.
set(ASTRA_GAEL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/GAEL" CACHE PATH
    "GAEL source directory (default: the external/GAEL submodule)")
get_filename_component(ASTRA_GAEL_SOURCE_DIR "${ASTRA_GAEL_SOURCE_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${ASTRA_GAEL_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ASTRA_GAEL_SOURCE_DIR does not look like a GAEL checkout: ${ASTRA_GAEL_SOURCE_DIR}")
endif()
set(GAEL_BUILD_REPORT OFF CACHE BOOL "" FORCE)   # no matplotlib/Python dep
add_subdirectory("${ASTRA_GAEL_SOURCE_DIR}" gael-build EXCLUDE_FROM_ALL)

# SEDplusplus submodule – builds the `sedfit` helper executable that ASTRA
# drives via QProcess for photometric SED fitting. Built by default (not
# EXCLUDE_FROM_ALL) so `cmake --build` always produces it alongside ASTRA.
if(ASTRA_HAVE_SEDFIT)
    add_subdirectory(external/SEDplusplus)
endif()


# ── Belt-and-braces: force the final ABI on every linked target ─────────────
# Some submodules unconditionally `set(CMAKE_CXX_STANDARD 17)` in their own
# CMakeLists.txt, which silently overrides our directory-scope setting.
foreach(_t IN ITEMS GAELcore qcustomplot)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
    endif()
endforeach()

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
    src/db/PeriodogramRepository.cpp
    src/db/MassFitRepository.cpp

    # dialogs
    src/dialogs/EditProjectDialog.cpp
    src/dialogs/ExportTableDialog.cpp
    src/dialogs/NewProjectDialog.cpp
    src/dialogs/SettingsDialog.cpp
    src/dialogs/FirstRunDialog.cpp
    src/dialogs/WhatsNewDialog.cpp
    src/dialogs/AddStarDialog.cpp
    src/dialogs/ImportLightcurve.cpp
    src/dialogs/LCFitDialog.cpp
    src/dialogs/LightcurveCredentialPrompts.cpp
    src/dialogs/ReidentifyStarDialog.cpp

    # importWizard
    src/importWizard/GeneralImportPage.cpp
    src/importWizard/ImportStagingArea.cpp
    src/importWizard/LightcurveImportPage.cpp
    src/importWizard/RadialVelocityImportPage.cpp
    src/importWizard/SEDImportPage.cpp
    src/importWizard/SpectraImportPage.cpp
    src/importWizard/SpectralFitImportPage.cpp
    src/importWizard/StarImportWizard.cpp

    # models
    src/models/BarycentricCorrection.cpp
    src/models/ColumnPreset.cpp
    src/models/ElementAbundances.cpp
    src/models/Instrument.cpp
    src/models/InstrumentMode.cpp
    src/models/MassFitPlan.cpp
    src/models/Photometry.cpp
    src/models/RemoteHost.cpp
    src/models/Project.cpp
    src/models/RadialVelocity.cpp
    src/models/Spectrum.cpp
    src/models/Star.cpp
    src/models/Time.cpp
    src/models/PeriodogramRecord.cpp

    # remote (SSH grid streaming + remote fitting)
    src/dialogs/RemoteHostsSettingsPage.cpp
    src/remote/ShellQuote.cpp
    src/remote/AskPass.cpp
    src/db/RemoteFitRepository.cpp
    src/remote/GridDiskCache.cpp
    src/remote/RemoteScheduler.cpp
    src/remote/RemoteFitService.cpp
    src/remote/SshGridProvider.cpp
    src/remote/SshConnection.cpp
    src/remote/SshFileStreamChannel.cpp
    src/remote/RemoteHostRegistry.cpp
    src/remote/RemoteSelfTest.cpp

    # utils
    src/utils/AppSettings.cpp
    src/utils/QuantityFormat.cpp
    src/utils/BackgroundTaskManager.cpp
    src/utils/CatalogQueryWorkers.cpp
    src/utils/CdsTapClient.cpp
    src/utils/CoastlineData.cpp
    src/utils/CrossRefResolver.cpp
    src/utils/DataStore.cpp
    src/utils/ExtractSED.cpp
    src/utils/Logger.cpp
    src/utils/SpectrumReader.cpp
    src/utils/ThemeManager.cpp
    src/utils/WindowSizing.cpp
    src/utils/UiIcons.cpp
    src/utils/UpdateManager.cpp
    src/utils/ObservabilityCalculator.cpp
    src/utils/LightcurveFetcher.cpp
    src/utils/LightcurveFetchService.cpp
    src/utils/PlotPresetStore.cpp
    src/utils/LcqueryEnvironment.cpp
    src/utils/IsisEnvironment.cpp
    src/utils/SedFitEnvironment.cpp
    src/utils/CheckBoxDragger.cpp
    src/utils/CheckStateDragger.cpp
    src/utils/ClaretTables.cpp
    src/utils/LCBinning.cpp
    src/utils/LCFitPhysics.cpp
    src/utils/LCFitRunner.cpp
    src/utils/FilterWavelength.cpp
    src/utils/FilterExpression.cpp
    src/utils/SystematicErrors.cpp
    src/utils/TessSectors.cpp
    src/utils/ClaretFilter.cpp
    src/utils/matchSpectraToInstrument.cpp
    src/utils/SpectrumCoadder.cpp
    src/utils/WheelGuard.cpp
    src/utils/SpectrumFetchService.cpp
    src/utils/MassFitService.cpp
    src/utils/spectrafetch/SpectrumArchiveClient.cpp
    src/utils/spectrafetch/SpectrumArchiveRegistry.cpp
    src/utils/spectrafetch/SpectrumArmJoin.cpp
    src/utils/spectrafetch/SpectrumFrame.cpp
    src/utils/spectrafetch/TapHelpers.cpp
    src/utils/spectrafetch/VoTableReader.cpp
    src/utils/spectrafetch/WavelengthConvert.cpp
    src/utils/spectrafetch/EsoArchiveClient.cpp
    src/utils/spectrafetch/EsoObsCoreIndex.cpp
    src/utils/spectrafetch/LamostArchiveClient.cpp
    src/utils/spectrafetch/SdssOpticalArchiveClient.cpp
    src/utils/spectrafetch/MastArchiveClient.cpp
    src/utils/spectrafetch/ApogeeArchiveClient.cpp

    # views
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
    src/views/tools/ProjectPlotDialog.cpp
    src/views/tools/LightcurveFetchDialog.cpp
    src/views/tools/LightcurveFetchSessionsDialog.cpp
    src/views/tools/LcquerySetupDialog.cpp
    src/views/tools/RVInspectorDialog.cpp
    src/views/tools/SEDFitDialog.cpp
    src/views/tools/SpectraFitDialog.cpp
    src/views/tools/ArchiveFetchWidget.cpp
    src/views/tools/SpectrumFetchSessionsDialog.cpp
    src/views/tools/AddSpectraDialog.cpp
    src/views/tools/FitSetupWidget.cpp
    src/views/tools/CoAddWidget.cpp
    src/views/tools/FitProgressDialog.cpp
    src/views/tools/InteractiveIsisDialog.cpp
    src/views/tools/RVAddFitDialog.cpp
    src/views/tools/RVAddPointDialog.cpp
    src/views/tools/RVImportPointsDialog.cpp
    src/views/tools/RVMCMCResultsDialog.cpp
    src/views/tools/ObservabilityDialog.cpp
    src/views/tools/RVDetectabilityDialog.cpp
    src/views/tools/MassFitPlanDialog.cpp
    src/views/tools/MassFitRuleEditor.cpp
    src/views/tools/MassFitManagerDialog.cpp
    src/views/tools/RemoteFitsDialog.cpp

    # views/panels
    src/views/panels/PanelUtils.cpp
    src/views/panels/DetailPanel.cpp
    src/views/panels/DetailPanelFactory.cpp
    src/views/panels/SummaryPanel.cpp
    src/views/panels/RVPanel.cpp
    src/views/panels/LCPanel.cpp
    src/views/panels/SpectraPanel.cpp
    src/views/panels/PeriodogramPanel.cpp

    # views/widgets
    src/views/widgets/CopyToast.cpp
    src/views/widgets/FlowLayout.cpp
    src/views/widgets/QuantityRenderer.cpp
    src/views/widgets/QuantityLabel.cpp
    src/views/widgets/QuantityDelegate.cpp
    src/views/widgets/FitPreviewOverlay.cpp
    src/views/widgets/FitComponentsWidget.cpp
    src/views/widgets/GridSelectorWidget.cpp
    src/views/widgets/ShimmerWidget.cpp
    src/views/widgets/SquarePlotFrame.cpp
    src/views/widgets/TerminalView.cpp
    src/views/widgets/AnsiTerminalWidget.cpp
    src/views/widgets/LCModelPreview.cpp
    src/views/widgets/IsisMacroPanel.cpp
    src/views/widgets/PlotKeyNavigator.cpp

    # fitting
    src/fitting/FitBackendRegistry.cpp
    src/fitting/FitJobFactory.cpp
    src/fitting/FitTypesJson.cpp
    src/fitting/GaelBackend.cpp
    src/fitting/GaelMapping.cpp
    src/fitting/RemoteGaelBackend.cpp
    src/fitting/IsisBackend.cpp
    src/fitting/FitWorker.cpp
    src/fitting/StdStreamRedirector.cpp
    src/fitting/Periodogram.cpp
    src/fitting/RVErrorMC.cpp
    src/fitting/RVMCMC.cpp
    src/fitting/RVDetectability.cpp

    # io
    src/io/StarPackage.cpp
    src/io/StarShare.cpp

    # kinematics
    src/kinematics/GalacticPotential.cpp
    src/kinematics/GalacticCoordinates.cpp
    src/kinematics/OrbitIntegrator.cpp
    src/kinematics/KinematicsCalculator.cpp
    src/kinematics/StarKinematics.cpp
    src/kinematics/PopulationClassifier.cpp
    src/kinematics/KinematicContours.cpp
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
    src/db/PeriodogramRepository.h
    src/db/MassFitRepository.h

    # dialogs
    src/dialogs/EditProjectDialog.h
    src/dialogs/ExportTableDialog.h
    src/dialogs/NewProjectDialog.h
    src/dialogs/SettingsDialog.h
    src/dialogs/FirstRunDialog.h
    src/dialogs/WhatsNewDialog.h
    src/dialogs/AddStarDialog.h
    src/dialogs/ImportLightcurve.h
    src/dialogs/LCFitDialog.h
    src/dialogs/LightcurveCredentialPrompts.h
    src/dialogs/ReidentifyStarDialog.h
    
    # importWizard
    src/importWizard/GeneralImportPage.h
    src/importWizard/ImportStagingArea.h
    src/importWizard/LightcurveImportPage.h
    src/importWizard/RadialVelocityImportPage.h
    src/importWizard/SEDImportPage.h
    src/importWizard/SpectraImportPage.h
    src/importWizard/SpectralFitImportPage.h
    src/importWizard/StarImportWizard.h

    # models
    src/models/BarycentricCorrection.h
    src/models/ColumnPreset.h
    src/models/ElementAbundances.h
    src/models/Instrument.h
    src/models/InstrumentMode.h
    src/models/MassFitPlan.h
    src/models/Photometry.h
    src/models/RemoteHost.h
    src/models/Project.h
    src/models/RadialVelocity.h
    src/models/Spectrum.h
    src/models/Star.h
    src/models/Time.h
    src/models/PeriodogramRecord.h

    # remote (SSH grid streaming + remote fitting)
    src/dialogs/RemoteHostsSettingsPage.h
    src/remote/ShellQuote.h
    src/remote/AskPass.h
    src/db/RemoteFitRepository.h
    src/remote/GridDiskCache.h
    src/remote/RemoteScheduler.h
    src/remote/RemoteFitService.h
    src/remote/SshGridProvider.h
    src/remote/SshConnection.h
    src/remote/SshFileStreamChannel.h
    src/remote/RemoteHostRegistry.h
    src/remote/RemoteSelfTest.h

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
    src/utils/WindowSizing.h
    src/utils/UiIcons.h
    src/utils/UpdateManager.h
    src/utils/ObservabilityCalculator.h
    src/utils/LightcurveFetcher.h
    src/utils/LightcurveFetchService.h
    src/utils/SpectrumFetchService.h
    src/utils/MassFitService.h
    src/utils/spectrafetch/SpectrumArchiveTypes.h
    src/utils/spectrafetch/SpectrumArchiveClient.h
    src/utils/spectrafetch/SpectrumArchiveRegistry.h
    src/utils/spectrafetch/SpectrumArmJoin.h
    src/utils/spectrafetch/SpectrumFrame.h
    src/utils/spectrafetch/TapHelpers.h
    src/utils/spectrafetch/VoTableReader.h
    src/utils/spectrafetch/WavelengthConvert.h
    src/utils/spectrafetch/EsoArchiveClient.h
    src/utils/spectrafetch/EsoObsCoreIndex.h
    src/utils/spectrafetch/LamostArchiveClient.h
    src/utils/spectrafetch/SdssOpticalArchiveClient.h
    src/utils/spectrafetch/MastArchiveClient.h
    src/utils/spectrafetch/ApogeeArchiveClient.h
    src/utils/PlotPresetStore.h
    src/utils/LcqueryEnvironment.h
    src/utils/IsisEnvironment.h
    src/utils/SedFitEnvironment.h
    src/utils/CheckBoxDragger.h
    src/utils/CheckStateDragger.h
    src/utils/ClaretTables.h
    src/utils/LCFitPhysics.h
    src/utils/LCFitRunner.h
    src/utils/FilterWavelength.h
    src/utils/FilterExpression.h
    src/utils/SystematicErrors.h
    src/utils/TessSectors.h
    src/utils/ClaretFilter.h
    src/utils/matchSpectraToInstrument.h
    src/utils/SpectrumCoadder.h
    src/utils/WheelGuard.h

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
    src/views/tools/RVDetectabilityDialog.h
    src/views/tools/GalacticOrbitDialog.h
    src/views/tools/ProjectPlotDialog.h
    src/views/tools/LightcurveFetchDialog.h
    src/views/tools/LightcurveFetchSessionsDialog.h
    src/views/tools/LcquerySetupDialog.h
    src/views/tools/RVInspectorDialog.h
    src/views/tools/SEDFitDialog.h
    src/views/tools/SpectraFitDialog.h
    src/views/tools/ArchiveFetchWidget.h
    src/views/tools/SpectrumFetchSessionsDialog.h
    src/views/tools/AddSpectraDialog.h
    src/views/tools/FitSetupWidget.h
    src/views/tools/CoAddWidget.h
    src/views/tools/FitProgressDialog.h
    src/views/tools/InteractiveIsisDialog.h
    src/views/tools/RVAddFitDialog.h
    src/views/tools/RVAddPointDialog.h
    src/views/tools/RVMCMCResultsDialog.h
    src/views/tools/ObservabilityDialog.h
    src/views/tools/MassFitPlanDialog.h
    src/views/tools/MassFitRuleEditor.h
    src/views/tools/MassFitManagerDialog.h
    src/views/tools/RemoteFitsDialog.h

    # views/panels
    src/views/panels/PanelUtils.h
    src/views/panels/DetailPanel.h
    src/views/panels/DetailPanelFactory.h
    src/views/panels/SummaryPanel.h
    src/views/panels/RVPanel.h
    src/views/panels/LCPanel.h
    src/views/panels/SpectraPanel.h
    src/views/panels/PeriodogramPanel.h

    # views/widgets
    src/views/widgets/FitPreviewOverlay.h
    src/views/widgets/FitComponentsWidget.h
    src/views/widgets/FlowLayout.h
    src/views/widgets/GridSelectorWidget.h
    src/views/widgets/ShimmerWidget.h
    src/views/widgets/SquarePlotFrame.h
    src/views/widgets/TerminalView.h
    src/views/widgets/AnsiTerminalWidget.h
    src/views/widgets/LCModelPreview.h
    src/views/widgets/IsisMacroPanel.h
    src/views/widgets/PlotKeyNavigator.h
    src/views/widgets/PreciseDoubleSpinBox.h

    #GAEL fitting
    src/fitting/FitTypes.h
    src/fitting/FitJobFactory.h
    src/fitting/FitTypesJson.h
    src/fitting/RVDetectability.h
    src/fitting/IFitBackend.h
    src/fitting/FitBackendRegistry.h
    src/fitting/GaelBackend.h
    src/fitting/GaelMapping.h
    src/fitting/RemoteGaelBackend.h
    src/fitting/IsisBackend.h
    src/fitting/FitWorker.h
    src/fitting/StdStreamRedirector.h
    src/fitting/Periodogram.h
    src/fitting/RVErrorMC.h
    src/fitting/RVMCMC.h

    # io
    src/io/StarPackage.h
    src/io/StarShare.h

    # kinematics
    src/kinematics/GalacticPotential.h
    src/kinematics/GalacticCoordinates.h
    src/kinematics/OrbitIntegrator.h
    src/kinematics/KinematicsCalculator.h
    src/kinematics/StarKinematics.h
    src/kinematics/PopulationClassifier.h
    src/kinematics/KinematicContours.h
)

# Create executable
qt_add_executable(ASTRA
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

# ── Git-derived version header (regenerated every build) ──────────────────────
# ASTRA_VERSION_OVERRIDE stamps a fixed version string (release pipelines only);
# leave it empty and the version is resolved from git on every build.
set(ASTRA_VERSION_OVERRIDE "" CACHE STRING
    "Stamp this exact version string instead of resolving it from git")
set(ASTRA_VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/astra_version.h)
add_custom_target(astra_version ALL
    BYPRODUCTS ${ASTRA_VERSION_HEADER}
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUT=${ASTRA_VERSION_HEADER}
            -DFALLBACK_VERSION=${PROJECT_VERSION}
            -DVERSION_OVERRIDE=${ASTRA_VERSION_OVERRIDE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake
    COMMENT "Resolving ASTRA version from git"
    VERBATIM)
add_dependencies(ASTRA astra_version)
target_include_directories(ASTRA PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_include_directories(ASTRA PRIVATE
    ${ASTRA_GAEL_SOURCE_DIR}/include
)

# Pass to source as a preprocessor define
target_compile_definitions(ASTRA PRIVATE
    ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
    ASTRA_LCQUERY_SCRIPT="${LCQUERY_SCRIPT}"
    ASTRA_LCQUERY_REQS="${LCQUERY_REQS}"
    ASTRA_HAVE_LCQUERY=$<BOOL:${ASTRA_HAVE_LCQUERY}>
    ASTRA_LCQUERY_BUNDLE_RELDIR="${ASTRA_LCQUERY_BUNDLE_RELDIR}"
    ASTRA_LCQUERY_SOURCE_DIR="${LCQUERY_DIR}"
    ASTRA_LCURVE_BUNDLE_RELDIR="${ASTRA_LCURVE_BUNDLE_RELDIR}"
    ASTRA_LCURVE_SOURCE_DIR="${ASTRA_LCURVE_DIR}"
    ASTRA_ISIS_BUNDLE_RELDIR="${ASTRA_ISIS_BUNDLE_RELDIR}"
    ASTRA_ISIS_SOURCE_DIR="${ASTRA_ISIS_DIR}"
    ASTRA_SEDFIT_BUNDLE_RELDIR="${ASTRA_SEDFIT_BUNDLE_RELDIR}"
    ASTRA_SEDFIT_REFDATA_RELDIR="${ASTRA_SEDFIT_REFDATA_RELDIR}"
    ASTRA_SEDFIT_BUILD_DIR="${CMAKE_BINARY_DIR}/external/SEDplusplus"
    ASTRA_SEDFIT_REFDATA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/sedfit/refdata"
)

# Profiling option
option(ENABLE_PROFILING "Build with profiling support (debug symbols + frame pointers)" OFF)


if(ENABLE_PROFILING)
    message(STATUS "Profiling enabled: -g -fno-omit-frame-pointer")
    set(PROFILING_FLAGS -O2 -g -fno-omit-frame-pointer)

    target_compile_options(ASTRA       PRIVATE ${PROFILING_FLAGS})
    target_compile_options(qcustomplot PRIVATE ${PROFILING_FLAGS})

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
    Qt6::Svg
    qcustomplot
    GAEL::core
)

# ── FFTW3 ──────────────────────────────────────────────────────────────────
# Used directly by Periodogram.cpp (GLS via the FFT-based trig sums). ASTRA
# used to pick this up by accident, as a transitive dependency of the RV_MCMC
# submodule; it is a first-class dependency of our own code now.
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3 fftw3)
endif()
if(FFTW3_FOUND)
    target_include_directories(ASTRA PRIVATE ${FFTW3_INCLUDE_DIRS})
    target_link_libraries(ASTRA PRIVATE ${FFTW3_LIBRARIES})
    target_link_directories(ASTRA PRIVATE ${FFTW3_LIBRARY_DIRS})
else()
    find_path(FFTW3_INCLUDE_DIR fftw3.h)
    find_library(FFTW3_LIBRARY NAMES fftw3 libfftw3)
    if(NOT FFTW3_INCLUDE_DIR OR NOT FFTW3_LIBRARY)
        message(FATAL_ERROR "FFTW3 not found - required by src/fitting/Periodogram.cpp")
    endif()
    message(STATUS "Found FFTW3: ${FFTW3_LIBRARY}")
    target_include_directories(ASTRA PRIVATE ${FFTW3_INCLUDE_DIR})
    target_link_libraries(ASTRA PRIVATE ${FFTW3_LIBRARY})
endif()

# ── OpenMP for the RV-MCMC sampler ─────────────────────────────────────────
# Enabled per source file on purpose: several other translation units carry
# `#pragma omp` directives that have never actually been compiled with OpenMP
# on (the flag has never been part of ASTRA's own compile line), so switching
# the whole target over would change their behaviour as a side effect. The
# sampler degrades to a correct single-threaded run when OpenMP is missing.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    # AppleClang reports "-Xclang -fopenmp" here, which has to reach the driver
    # as two arguments, not one.
    separate_arguments(ASTRA_OPENMP_FLAGS NATIVE_COMMAND "${OpenMP_CXX_FLAGS}")
    set_source_files_properties(src/fitting/RVMCMC.cpp PROPERTIES
        COMPILE_OPTIONS "${ASTRA_OPENMP_FLAGS}")
    target_include_directories(ASTRA PRIVATE ${OpenMP_CXX_INCLUDE_DIRS})
    target_link_libraries(ASTRA PRIVATE ${OpenMP_CXX_LIBRARIES})
else()
    message(WARNING "OpenMP not found - the RV-MCMC sampler will be single-threaded")
endif()

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
    # --- App icon --------------------------------------------------------
    # Info.plist.in fills CFBundleIconFile from MACOSX_BUNDLE_ICON_FILE, and
    # Finder falls back to the blank generic app icon whenever that key is
    # empty, even with a perfectly good .icns sitting in Contents/Resources.
    # So the icns gets generated AND named here, in one place.
    set(ASTRA_MACOS_ICON_SRC "")
    foreach(_cand resources/linux/icons/astra.png
                  resources/icons/astra.png
                  resources/linux/icons/astra.svg)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_cand}")
            set(ASTRA_MACOS_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${_cand}")
            break()
        endif()
    endforeach()

    if(ASTRA_MACOS_ICON_SRC)
        set(ASTRA_ICNS "${CMAKE_CURRENT_BINARY_DIR}/astra.icns")
        add_custom_command(
            OUTPUT  "${ASTRA_ICNS}"
            COMMAND ${CMAKE_COMMAND}
                    -DICON_SRC=${ASTRA_MACOS_ICON_SRC}
                    -DOUT_ICNS=${ASTRA_ICNS}
                    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/icon-work
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MakeMacIcns.cmake"
            DEPENDS "${ASTRA_MACOS_ICON_SRC}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MakeMacIcns.cmake"
            COMMENT "Generating astra.icns from ${ASTRA_MACOS_ICON_SRC}"
            VERBATIM)
        # GENERATED so the copy-into-bundle rule does not demand the file at
        # configure time; the custom command above supplies it during the build.
        set_source_files_properties("${ASTRA_ICNS}" PROPERTIES
            GENERATED TRUE
            MACOSX_PACKAGE_LOCATION Resources)
        target_sources(ASTRA PRIVATE "${ASTRA_ICNS}")
    else()
        message(WARNING "No macOS icon source found; the .app will use the "
                        "generic application icon.")
    endif()

    # An empty CFBundleIdentifier makes Launch Services treat the bundle as
    # unregisterable, which is a second, independent way to lose the icon, so
    # every placeholder in Info.plist.in gets a real value.
    # Release pipelines pass the tag (v0.7.0) in ASTRA_VERSION_OVERRIDE. Only a
    # plain dotted number belongs in CFBundleShortVersionString, so anything
    # else (a dev stamp such as git-<hash>) falls back to the project version.
    set(_astra_bundle_version "${PROJECT_VERSION}")
    if(ASTRA_VERSION_OVERRIDE MATCHES "^v?([0-9]+(\\.[0-9]+)*)$")
        set(_astra_bundle_version "${CMAKE_MATCH_1}")
    endif()

    set_target_properties(ASTRA PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "ASTRA"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.fabmat1.astra"
        MACOSX_BUNDLE_BUNDLE_VERSION "${_astra_bundle_version}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${_astra_bundle_version}"
        MACOSX_BUNDLE_COPYRIGHT "Fabian Mattig"
    )
    if(ASTRA_MACOS_ICON_SRC)
        set_target_properties(ASTRA PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "astra.icns")
    endif()
    
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

# ── Bundle lcurve fitting binaries (if provided at configure time) ──────────
# install(PROGRAMS ...) keeps the executable bit. The AppImage build step still
# needs to pull each binary's shared-library deps (Boost, libgomp, ...) into the
# AppDir, e.g. linuxdeploy --executable <AppDir>/.../lcurve/lcurve_levmarq ...
if(_lcurve_found_files)
    install(PROGRAMS ${_lcurve_found_files}
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/astra/lcurve")
endif()

# ── Bundle the sedfit SED-fitting helper + its filter reference data ─────────
if(ASTRA_HAVE_SEDFIT)
    install(TARGETS sedfit
        RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/astra/sedfit")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/sedfit/refdata/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/astra/sedfit/refdata")
endif()

# ── Bundle lightcurvequery Python sources (if the submodule is present) ─────
# Shipped read-only; ASTRA copies them into a writable dir and builds a venv at
# runtime. Excludes VCS metadata, build caches, the submodule's own venv and
# any per-run output folders so the AppImage stays clean and reproducible.
if(ASTRA_HAVE_LCQUERY)
    install(DIRECTORY "${LCQUERY_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/astra/lightcurvequery"
        USE_SOURCE_PERMISSIONS
        PATTERN ".git*"        EXCLUDE
        PATTERN ".venv"        EXCLUDE
        PATTERN ".idea"        EXCLUDE
        PATTERN "__pycache__"  EXCLUDE
        PATTERN "lightcurves"  EXCLUDE
        PATTERN "mastDownload" EXCLUDE
        PATTERN "lcplots"      EXCLUDE
        PATTERN "pgramplots"   EXCLUDE
        PATTERN "rvplots"      EXCLUDE)
endif()

# ── Linux desktop integration ─────────────────────────────────────────────
if(UNIX AND NOT APPLE)
    # --- .desktop file ---------------------------------------------------
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/astra.desktop.in"
        "${CMAKE_CURRENT_BINARY_DIR}/astra.desktop"
        @ONLY
    )
    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/astra.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )

    # --- Icon (scalable SVG preferred; fall back to 256×256 PNG) ---------
    # Place your icon at resources/icons/astra.svg (or astra.png).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/icons/astra.svg")
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/icons/astra.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        )
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/icons/astra.png")
        install(
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/icons/astra.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
        )
    endif()

    # install MIME definition
    install(
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/astra-mime.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages"
    )

    # refresh caches
    find_program(UPDATE_DESKTOP_DB update-desktop-database)
    find_program(UPDATE_ICON_CACHE gtk-update-icon-cache)
    find_program(UPDATE_MIME_DATABASE update-mime-database)

    if(UPDATE_MIME_DATABASE)
        install(CODE "
            execute_process(
                COMMAND \"${UPDATE_MIME_DATABASE}\"
                        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/mime\"
                RESULT_VARIABLE _ret
            )
            if(NOT _ret EQUAL 0)
                message(WARNING \"update-mime-database returned \${_ret}\")
            endif()
        ")
    endif()

    # --- Refresh the icon/desktop caches after install -------------------
    # These are no-ops if the tools are absent (e.g. a headless build host).
    find_program(UPDATE_DESKTOP_DB update-desktop-database)
    find_program(UPDATE_ICON_CACHE  gtk-update-icon-cache)

    if(UPDATE_DESKTOP_DB)
        install(CODE "
            execute_process(
                COMMAND \"${UPDATE_DESKTOP_DB}\"
                        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\"
                RESULT_VARIABLE _ret
            )
            if(NOT _ret EQUAL 0)
                message(WARNING \"update-desktop-database returned \${_ret}\")
            endif()
        ")
    endif()

    if(UPDATE_ICON_CACHE)
        install(CODE "
            execute_process(
                COMMAND \"${UPDATE_ICON_CACHE}\" --quiet --force --ignore-theme-index
                        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor\"
                RESULT_VARIABLE _ret
            )
            if(NOT _ret EQUAL 0)
                message(WARNING \"gtk-update-icon-cache returned \${_ret}\")
            endif()
        ")
    endif()
endif()


# ═════════════════════════════════════════════════════════════════════════════
# Developer tools (not built by default)
# ═════════════════════════════════════════════════════════════════════════════
# Regenerates the kinematic reference contour CSVs bundled in resources.qrc:
#   cmake --build build --target regenerate_kinematic_contours
add_executable(generate_kinematic_contours EXCLUDE_FROM_ALL
    tools/generate_kinematic_contours.cpp
    src/kinematics/PopulationClassifier.cpp
)
target_include_directories(generate_kinematic_contours PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_custom_target(regenerate_kinematic_contours
    COMMAND generate_kinematic_contours
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/data/kinematics"
    DEPENDS generate_kinematic_contours
    COMMENT "Regenerating kinematic contour CSVs..."
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

    # Ensure reference data is generated before the test runs
    if(TARGET generate_reference)
        add_dependencies(test_barycentric generate_reference)
    endif()

    # ── RV-MCMC sampler test ─────────────────────────────────────────────
    # Compiles the sampler (plus the periodogram it seeds from and the logger
    # it reports through) straight into the test binary - no GUI, no database.
    add_executable(test_rvmcmc
        tests/test_rvmcmc.cpp
        src/fitting/RVMCMC.cpp
        src/fitting/Periodogram.cpp
        src/utils/Logger.cpp
    )
    set_target_properties(test_rvmcmc PROPERTIES AUTOMOC ON)
    target_include_directories(test_rvmcmc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_rvmcmc PRIVATE
        ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
    )
    target_link_libraries(test_rvmcmc PRIVATE Qt6::Core)
    # Periodogram.cpp uses Eigen; the app gets it transitively through GAEL.
    find_package(Eigen3 QUIET NO_MODULE)
    if(TARGET Eigen3::Eigen)
        target_link_libraries(test_rvmcmc PRIVATE Eigen3::Eigen)
    elseif(TARGET GAEL::core)
        target_link_libraries(test_rvmcmc PRIVATE GAEL::core)
    endif()
    # EIGEN_USE_BLAS/LAPACKE are set project-wide, so Eigen calls out to them.
    find_package(BLAS QUIET)
    if(BLAS_FOUND)
        target_link_libraries(test_rvmcmc PRIVATE ${BLAS_LIBRARIES})
    endif()
    find_package(LAPACK QUIET)
    if(LAPACK_FOUND)
        target_link_libraries(test_rvmcmc PRIVATE ${LAPACK_LIBRARIES})
    endif()
    if(OpenMP_CXX_FOUND)
        target_include_directories(test_rvmcmc PRIVATE ${OpenMP_CXX_INCLUDE_DIRS})
        target_link_libraries(test_rvmcmc PRIVATE ${OpenMP_CXX_LIBRARIES})
    endif()
    if(FFTW3_FOUND)
        target_include_directories(test_rvmcmc PRIVATE ${FFTW3_INCLUDE_DIRS})
        target_link_directories(test_rvmcmc PRIVATE ${FFTW3_LIBRARY_DIRS})
        target_link_libraries(test_rvmcmc PRIVATE ${FFTW3_LIBRARIES})
    else()
        target_include_directories(test_rvmcmc PRIVATE ${FFTW3_INCLUDE_DIR})
        target_link_libraries(test_rvmcmc PRIVATE ${FFTW3_LIBRARY})
    endif()

    # ── Copy-format test ─────────────────────────────────────────────────
    add_executable(test_quantityformat
        tests/test_quantityformat.cpp
        src/utils/QuantityFormat.cpp
    )
    target_include_directories(test_quantityformat PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_quantityformat PRIVATE Qt6::Core)

    add_test(
        NAME    quantity_format
        COMMAND test_quantityformat
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(quantity_format PROPERTIES
        LABELS "utils;formatting"
        TIMEOUT 30
    )

    # ── Import star-matching helpers ─────────────────────────────────────
    add_executable(test_star_matching
        tests/test_star_matching.cpp
    )
    target_include_directories(test_star_matching PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_star_matching PRIVATE Qt6::Core)

    add_test(
        NAME    star_matching
        COMMAND test_star_matching
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(star_matching PROPERTIES
        LABELS "utils;import"
        TIMEOUT 30
    )

    # ── Spectrum-fetch helper tests ──────────────────────────────────────
    add_executable(test_wavelength_convert
        tests/test_wavelength_convert.cpp
        src/utils/spectrafetch/WavelengthConvert.cpp
    )
    target_include_directories(test_wavelength_convert PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(
        NAME    wavelength_convert
        COMMAND test_wavelength_convert
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(wavelength_convert PROPERTIES
        LABELS "utils;spectrafetch"
        TIMEOUT 30
    )

    # Spectral reference frame: what the FITS headers say, and the shift.
    add_executable(test_spectrum_frame
        tests/test_spectrum_frame.cpp
        src/utils/spectrafetch/SpectrumFrame.cpp
        src/models/BarycentricCorrection.cpp
    )
    target_include_directories(test_spectrum_frame PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(CCFITS_FOUND)
        target_include_directories(test_spectrum_frame PRIVATE
            ${CCFITS_INCLUDE_DIRS})
        target_link_libraries(test_spectrum_frame PRIVATE
            ${CCFITS_LIBRARIES})
    endif()
    target_link_libraries(test_spectrum_frame PRIVATE Qt6::Core)
    add_test(
        NAME    spectrum_frame
        COMMAND test_spectrum_frame
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(spectrum_frame PROPERTIES
        LABELS "utils;spectrafetch"
        TIMEOUT 60
    )

    # Arm joining: grouping one exposure's products and splicing them.
    add_executable(test_arm_join
        tests/test_arm_join.cpp
        src/utils/spectrafetch/SpectrumArmJoin.cpp
    )
    target_include_directories(test_arm_join PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_arm_join PRIVATE Qt6::Core)
    add_test(
        NAME    arm_join
        COMMAND test_arm_join
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(arm_join PROPERTIES
        LABELS "utils;spectrafetch"
        TIMEOUT 30
    )

    # Mass fitting plan: serialisation, rule evaluation, tree walking and
    # validation. Model-only, so no database and no GUI.
    add_executable(test_massfit_plan
        tests/test_massfit_plan.cpp
        src/models/MassFitPlan.cpp
        src/fitting/FitTypesJson.cpp
    )
    target_include_directories(test_massfit_plan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_massfit_plan PRIVATE Qt6::Core)
    add_test(
        NAME    massfit_plan
        COMMAND test_massfit_plan
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(massfit_plan PROPERTIES
        LABELS "models;massfit"
        TIMEOUT 30
    )

    add_executable(test_votable_reader
        tests/test_votable_reader.cpp
        src/utils/spectrafetch/VoTableReader.cpp
    )
    target_include_directories(test_votable_reader PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_votable_reader PRIVATE Qt6::Core)
    add_test(
        NAME    votable_reader
        COMMAND test_votable_reader
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(votable_reader PROPERTIES
        LABELS "utils;spectrafetch"
        TIMEOUT 30
    )

    # Instrument shape-matcher regressions against the shipped defaults.
    add_executable(test_instrument_match
        tests/test_instrument_match.cpp
        src/utils/matchSpectraToInstrument.cpp
        src/models/Instrument.cpp
        src/models/InstrumentMode.cpp
        src/models/BarycentricCorrection.cpp
    )
    target_include_directories(test_instrument_match PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_instrument_match PRIVATE
        ASTRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(test_instrument_match PRIVATE Qt6::Core)
    add_test(
        NAME    instrument_match
        COMMAND test_instrument_match
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(instrument_match PROPERTIES
        LABELS "utils"
        TIMEOUT 30
    )

    # Archive FITS parsers, run against real product files whose paths come
    # from ASTRA_TEST_* environment variables (skips cleanly when unset -
    # the files are too large to commit as fixtures).
    add_executable(test_spectrafetch_parsers
        tests/test_spectrafetch_parsers.cpp
        src/utils/spectrafetch/SpectrumArchiveClient.cpp
        src/utils/spectrafetch/SpectrumFrame.cpp
        src/utils/spectrafetch/EsoArchiveClient.cpp
        src/utils/spectrafetch/EsoObsCoreIndex.cpp
        src/utils/spectrafetch/LamostArchiveClient.cpp
        src/utils/spectrafetch/SdssOpticalArchiveClient.cpp
        src/utils/spectrafetch/ApogeeArchiveClient.cpp
        src/utils/spectrafetch/MastArchiveClient.cpp
        src/utils/spectrafetch/TapHelpers.cpp
        src/utils/spectrafetch/VoTableReader.cpp
        src/utils/spectrafetch/WavelengthConvert.cpp
        src/utils/CdsTapClient.cpp
        src/utils/SpectrumReader.cpp
        src/utils/DataStore.cpp
        src/utils/Logger.cpp
        src/models/Spectrum.cpp
        src/models/Time.cpp
        src/models/Instrument.cpp
        src/models/InstrumentMode.cpp
        src/models/BarycentricCorrection.cpp
    )
    target_include_directories(test_spectrafetch_parsers PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_spectrafetch_parsers PRIVATE
        ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
    )
    if(CCFITS_FOUND)
        target_include_directories(test_spectrafetch_parsers PRIVATE
            ${CCFITS_INCLUDE_DIRS})
        target_link_libraries(test_spectrafetch_parsers PRIVATE
            ${CCFITS_LIBRARIES})
    endif()
    target_link_libraries(test_spectrafetch_parsers PRIVATE
        Qt6::Core Qt6::Network)
    add_test(
        NAME    spectrafetch_parsers
        COMMAND test_spectrafetch_parsers
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(spectrafetch_parsers PROPERTIES
        LABELS "utils;spectrafetch"
        TIMEOUT 120
    )

    # Bulk ESO discovery against the live TAP service. Off unless
    # ASTRA_TEST_ESO_LIVE=1 is set (see the file header); guards the
    # project-sized query that the 120 s /sync budget used to kill.
    add_executable(test_eso_bulk_discovery
        tests/test_eso_bulk_discovery.cpp
        src/utils/spectrafetch/SpectrumArchiveClient.cpp
        src/utils/spectrafetch/SpectrumFrame.cpp
        src/utils/spectrafetch/EsoArchiveClient.cpp
        src/utils/spectrafetch/EsoObsCoreIndex.cpp
        src/utils/spectrafetch/TapHelpers.cpp
        src/utils/spectrafetch/VoTableReader.cpp
        src/utils/CdsTapClient.cpp
        src/utils/SpectrumReader.cpp
        src/utils/DataStore.cpp
        src/utils/Logger.cpp
        src/models/Spectrum.cpp
        src/models/Time.cpp
        src/models/Instrument.cpp
        src/models/InstrumentMode.cpp
        src/models/BarycentricCorrection.cpp
    )
    target_include_directories(test_eso_bulk_discovery PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_eso_bulk_discovery PRIVATE
        ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
    )
    if(CCFITS_FOUND)
        target_include_directories(test_eso_bulk_discovery PRIVATE
            ${CCFITS_INCLUDE_DIRS})
        target_link_libraries(test_eso_bulk_discovery PRIVATE
            ${CCFITS_LIBRARIES})
    endif()
    target_link_libraries(test_eso_bulk_discovery PRIVATE
        Qt6::Core Qt6::Network)
    add_test(
        NAME    eso_bulk_discovery
        COMMAND test_eso_bulk_discovery
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(eso_bulk_discovery PROPERTIES
        LABELS "utils;spectrafetch;network"
        TIMEOUT 900
    )

    # Bulk MAST discovery against the live TAP service. Off unless
    # ASTRA_TEST_MAST_LIVE=1 is set (see the file header); guards the cone
    # query that the CAOM TAP answers with a 504 instead of a plan.
    # Local ESO ObsCore mirror: build, cache round trip, band search against a
    # brute-force scan, and the mirrored discovery path end to end. Live, so it
    # skips unless ASTRA_TEST_ESO_LIVE=1 is set (see the file header). Gets its
    # own ASTRA_DATA_DIR so a run never replaces the user's real mirror with a
    # single-collection one.
    add_executable(test_eso_obscore_index
        tests/test_eso_obscore_index.cpp
        src/utils/spectrafetch/SpectrumArchiveClient.cpp
        src/utils/spectrafetch/SpectrumFrame.cpp
        src/utils/spectrafetch/EsoArchiveClient.cpp
        src/utils/spectrafetch/EsoObsCoreIndex.cpp
        src/utils/spectrafetch/TapHelpers.cpp
        src/utils/spectrafetch/VoTableReader.cpp
        src/utils/CdsTapClient.cpp
        src/utils/SpectrumReader.cpp
        src/utils/DataStore.cpp
        src/utils/Logger.cpp
        src/models/Spectrum.cpp
        src/models/Time.cpp
        src/models/Instrument.cpp
        src/models/InstrumentMode.cpp
        src/models/BarycentricCorrection.cpp
    )
    target_include_directories(test_eso_obscore_index PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_eso_obscore_index PRIVATE
        ASTRA_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/eso_index_test"
    )
    if(CCFITS_FOUND)
        target_include_directories(test_eso_obscore_index PRIVATE
            ${CCFITS_INCLUDE_DIRS})
        target_link_libraries(test_eso_obscore_index PRIVATE
            ${CCFITS_LIBRARIES})
    endif()
    target_link_libraries(test_eso_obscore_index PRIVATE
        Qt6::Core Qt6::Network)
    add_test(
        NAME    eso_obscore_index
        COMMAND test_eso_obscore_index
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(eso_obscore_index PROPERTIES
        LABELS "utils;spectrafetch;network"
        TIMEOUT 1800
    )

    add_executable(test_mast_bulk_discovery
        tests/test_mast_bulk_discovery.cpp
        src/utils/spectrafetch/SpectrumArchiveClient.cpp
        src/utils/spectrafetch/SpectrumFrame.cpp
        src/utils/spectrafetch/MastArchiveClient.cpp
        src/utils/spectrafetch/TapHelpers.cpp
        src/utils/spectrafetch/VoTableReader.cpp
        src/utils/CdsTapClient.cpp
        src/utils/SpectrumReader.cpp
        src/utils/DataStore.cpp
        src/utils/Logger.cpp
        src/models/Spectrum.cpp
        src/models/Time.cpp
        src/models/Instrument.cpp
        src/models/InstrumentMode.cpp
        src/models/BarycentricCorrection.cpp
    )
    target_include_directories(test_mast_bulk_discovery PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_mast_bulk_discovery PRIVATE
        ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
    )
    if(CCFITS_FOUND)
        target_include_directories(test_mast_bulk_discovery PRIVATE
            ${CCFITS_INCLUDE_DIRS})
        target_link_libraries(test_mast_bulk_discovery PRIVATE
            ${CCFITS_LIBRARIES})
    endif()
    target_link_libraries(test_mast_bulk_discovery PRIVATE
        Qt6::Core Qt6::Network)
    add_test(
        NAME    mast_bulk_discovery
        COMMAND test_mast_bulk_discovery
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(mast_bulk_discovery PROPERTIES
        LABELS "utils;spectrafetch;network"
        TIMEOUT 900
    )

    add_test(
        NAME    rv_mcmc_sampler
        COMMAND test_rvmcmc
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_tests_properties(rv_mcmc_sampler PROPERTIES
        LABELS "fitting;statistics"
        TIMEOUT 300
    )

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
