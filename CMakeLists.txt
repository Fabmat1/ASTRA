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
# -std=c++20 (NOT gnu++20) everywhere -- one dialect for host and submodules.
#
# Windows is the exception, and has to be. -std=c++20 defines __STRICT_ANSI__,
# and mingw-w64 hides a large part of its headers behind that: M_PI and the
# other math constants (used in ~26 of our own translation units) and popen /
# pclose (used by SEDplusplus) all vanish. Suppressing them one macro at a time
# means patching submodules we do not own, so Windows gets gnu++20 instead.
# That is still a single dialect for the whole Windows build -- GCC either way,
# same ABI -- which is what this setting is actually protecting.
if(WIN32)
    set(ASTRA_CXX_EXTENSIONS ON)
else()
    set(ASTRA_CXX_EXTENSIONS OFF)
endif()
set(CMAKE_CXX_EXTENSIONS ${ASTRA_CXX_EXTENSIONS})

# Pick a baseline ISA that covers desktop + laptop. Avoid -march=native.
# x86 only: these tokens are rejected outright by Apple Clang on arm64, so the
# guard keeps the macOS build from having to patch (and thereby dirty) the
# working tree — a dirty tree makes GitVersion.cmake stamp "git-<hash>-dirty"
# instead of the release tag, which in turn disables the updater.
# The NOT:RC guard matters only on Windows, where enable_language(RC) is on and
# directory-level compile options land on windres' command line too -- and
# windres treats an unrecognised option as a fatal usage error, so an unguarded
# -march here takes the resource compile down with it.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    add_compile_options(
        "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:-mtune=generic>"
        # AVX2/FMA. Use x86-64-v2 if you support older CPUs.
        "$<$<NOT:$<COMPILE_LANGUAGE:RC>>:-march=x86-64-v3>"
    )
endif()

# MinGW's assembler defaults to the COFF section limit (32767). qcustomplot.cpp
# alone is 35k lines of heavily templated Qt code and blows through it with
# "too many sections"; the flag switches to the big-obj COFF variant. Set here,
# ahead of the add_subdirectory() calls, so GAEL and SEDplusplus inherit it too.
if(MINGW)
    add_compile_options("$<$<NOT:$<COMPILE_LANGUAGE:RC>>:-Wa,-mbig-obj>")
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


# ── GAELcore's CCfits include path ─────────────────────────────────────────
# GAEL resolves CCfits with find_library() alone and never adds an include
# directory for it, which works everywhere the headers sit in a default search
# path (/usr/include on Linux, the Homebrew prefix on macOS). On Windows CCfits
# is a source build in its own prefix, so <CCfits/CCfits> is not found and
# GAELcore fails to compile. Hand it the directory ASTRA already resolved,
# rather than a global include_directories() that would put -I/usr/include
# ahead of /usr/local/include on the Unix builds.
if(TARGET GAELcore AND CCFITS_FOUND)
    target_include_directories(GAELcore PRIVATE ${CCFITS_INCLUDE_DIRS})
endif()

# ── Belt-and-braces: force the final ABI on every linked target ─────────────
# Some submodules unconditionally `set(CMAKE_CXX_STANDARD 17)` in their own
# CMakeLists.txt, which silently overrides our directory-scope setting.
foreach(_t IN ITEMS GAELcore qcustomplot)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS ${ASTRA_CXX_EXTENSIONS})
    endif()
endforeach()

# ── Source lists ─────────────────────────────────────────────────────────────
# Split by layer, not by domain: everything that does not live under a ui/ folder
# is Core-only and goes into astra_core, which the tests link against.  The rule
# is mechanical, so scripts/reorganize_src.py can regenerate these lists and
# verify that no Core file pulls in QtWidgets or QtGui.
set(ASTRA_CORE_SOURCES

    # app
    src/app/AppSettings.cpp
    src/app/DataStore.cpp
    src/app/Logger.cpp

    # catalog
    src/catalog/CatalogQueryWorkers.cpp
    src/catalog/CdsTapClient.cpp
    src/catalog/CrossRefResolver.cpp

    # core
    src/core/BarycentricCorrection.cpp
    src/core/ColumnPreset.cpp
    src/core/DelimitedTable.cpp
    src/core/FilterExpression.cpp
    src/core/Instrument.cpp
    src/core/InstrumentMode.cpp
    src/core/Project.cpp
    src/core/QuantityFormat.cpp
    src/core/Star.cpp
    src/core/Stats.cpp
    src/core/Time.cpp

    # db
    src/db/DBAccess.cpp
    src/db/DatabaseManager.cpp
    src/db/InstrumentRepository.cpp
    src/db/MassFitRepository.cpp
    src/db/PeriodogramRepository.cpp
    src/db/PhotometryRepository.cpp
    src/db/ProjectRepository.cpp
    src/db/RadialVelocityRepository.cpp
    src/db/RemoteFitRepository.cpp
    src/db/SpectrumRepository.cpp
    src/db/StarRepository.cpp

    # fitting
    src/fitting/ElementAbundances.cpp
    src/fitting/FitBackendRegistry.cpp
    src/fitting/FitJobFactory.cpp
    src/fitting/FitTypesJson.cpp
    src/fitting/FitWorker.cpp
    src/fitting/GaelBackend.cpp
    src/fitting/GaelMapping.cpp
    src/fitting/IsisBackend.cpp
    src/fitting/IsisEnvironment.cpp
    src/fitting/RemoteGaelBackend.cpp
    src/fitting/StdStreamRedirector.cpp
    src/fitting/SystematicErrors.cpp

    # io
    src/io/StarPackage.cpp

    # kinematics
    src/kinematics/GalacticCoordinates.cpp
    src/kinematics/GalacticPotential.cpp
    src/kinematics/KinematicContours.cpp
    src/kinematics/KinematicsCalculator.cpp
    src/kinematics/OrbitIntegrator.cpp
    src/kinematics/PopulationClassifier.cpp
    src/kinematics/StarKinematics.cpp

    # lightcurve
    src/lightcurve/ClaretFilter.cpp
    src/lightcurve/ClaretTables.cpp
    src/lightcurve/LCBinning.cpp
    src/lightcurve/LCFitPhysics.cpp
    src/lightcurve/LCFitRunner.cpp
    src/lightcurve/LcqueryEnvironment.cpp
    src/lightcurve/LightcurveFetchService.cpp
    src/lightcurve/LightcurveFetcher.cpp
    src/lightcurve/Photometry.cpp
    src/lightcurve/TessSectors.cpp

    # massfit
    src/massfit/MassFitPlan.cpp
    src/massfit/MassFitService.cpp

    # observing
    src/observing/ObservabilityCalculator.cpp

    # remote
    src/remote/GridDiskCache.cpp
    src/remote/RemoteFitService.cpp
    src/remote/RemoteHost.cpp
    src/remote/RemoteHostRegistry.cpp
    src/remote/RemoteScheduler.cpp
    src/remote/SshConnection.cpp
    src/remote/SshFileStreamChannel.cpp
    src/remote/SshGridProvider.cpp

    # rv
    src/rv/Periodogram.cpp
    src/rv/PeriodogramRecord.cpp
    src/rv/RVDetectability.cpp
    src/rv/RVErrorMC.cpp
    src/rv/RVMCMC.cpp
    src/rv/RadialVelocity.cpp

    # sed
    src/sed/ExtractSED.cpp
    src/sed/FilterWavelength.cpp
    src/sed/SedFitEnvironment.cpp

    # spectra
    src/spectra/Spectrum.cpp
    src/spectra/SpectrumCoadder.cpp
    src/spectra/SpectrumFetchService.cpp
    src/spectra/SpectrumReader.cpp

    # spectra/fetch
    src/spectra/fetch/ApogeeArchiveClient.cpp
    src/spectra/fetch/EsoArchiveClient.cpp
    src/spectra/fetch/ExposureEpoch.cpp
    src/spectra/fetch/EsoObsCoreIndex.cpp
    src/spectra/fetch/LamostArchiveClient.cpp
    src/spectra/fetch/MastArchiveClient.cpp
    src/spectra/fetch/SdssOpticalArchiveClient.cpp
    src/spectra/fetch/SpectrumArchiveClient.cpp
    src/spectra/fetch/SpectrumArchiveRegistry.cpp
    src/spectra/fetch/SpectrumArmJoin.cpp
    src/spectra/fetch/SpectrumFrame.cpp
    src/spectra/fetch/TapHelpers.cpp
    src/spectra/fetch/VoTableReader.cpp
    src/spectra/fetch/WavelengthConvert.cpp

    # spectra
    src/spectra/matchSpectraToInstrument.cpp
)

set(ASTRA_CORE_HEADERS

    # app
    src/app/AppPaths.h
    src/app/AppSettings.h
    src/app/ServiceContext.h
    src/app/DataStore.h
    src/app/Logger.h
    src/app/PerfTimer.h

    # catalog
    src/catalog/CatalogQueryWorkers.h
    src/catalog/CdsTapClient.h
    src/catalog/CrossRefResolver.h
    src/catalog/StarMatching.h
    src/catalog/StarSearchQuery.h

    # core
    src/core/AsymmetricErrors.h
    src/core/BarycentricCorrection.h
    src/core/ColumnPreset.h
    src/core/DelimitedTable.h
    src/core/FilterExpression.h
    src/core/Instrument.h
    src/core/InstrumentMode.h
    src/core/Project.h
    src/core/PhaseUtils.h
    src/core/Quantity.h
    src/core/QuantityFormat.h
    src/core/SkyGeometry.h
    src/core/Star.h
    src/core/Stats.h
    src/core/Time.h

    # db
    src/db/DBAccess.h
    src/db/DatabaseManager.h
    src/db/InstrumentRepository.h
    src/db/MassFitRepository.h
    src/db/PeriodogramRepository.h
    src/db/PhotometryRepository.h
    src/db/ProjectRepository.h
    src/db/RadialVelocityRepository.h
    src/db/RemoteFitRepository.h
    src/db/SpectrumRepository.h
    src/db/SqlValue.h
    src/db/StarRepository.h

    # fitting
    src/fitting/ElementAbundances.h
    src/fitting/FitBackendRegistry.h
    src/fitting/FitJobFactory.h
    src/fitting/FitTypes.h
    src/fitting/FitTypesJson.h
    src/fitting/FitWorker.h
    src/fitting/GaelBackend.h
    src/fitting/GaelMapping.h
    src/fitting/IFitBackend.h
    src/fitting/IsisBackend.h
    src/fitting/IsisEnvironment.h
    src/fitting/RemoteGaelBackend.h
    src/fitting/StdStreamRedirector.h
    src/fitting/SystematicErrors.h

    # io
    src/io/StarPackage.h

    # kinematics
    src/kinematics/GalacticCoordinates.h
    src/kinematics/GalacticPotential.h
    src/kinematics/KinematicContours.h
    src/kinematics/KinematicsCalculator.h
    src/kinematics/OrbitIntegrator.h
    src/kinematics/PopulationClassifier.h
    src/kinematics/StarKinematics.h

    # lightcurve
    src/lightcurve/ClaretFilter.h
    src/lightcurve/ClaretTables.h
    src/lightcurve/LCBinning.h
    src/lightcurve/LCFitPhysics.h
    src/lightcurve/LCFitRunner.h
    src/lightcurve/LcqueryEnvironment.h
    src/lightcurve/LightcurveFetchService.h
    src/lightcurve/LightcurveFetcher.h
    src/lightcurve/Photometry.h
    src/lightcurve/TessSectors.h

    # massfit
    src/massfit/MassFitPlan.h
    src/massfit/MassFitService.h

    # observing
    src/observing/ObservabilityCalculator.h

    # remote
    src/remote/GridDiskCache.h
    src/remote/RemoteFitService.h
    src/remote/RemoteHost.h
    src/remote/RemoteHostRegistry.h
    src/remote/RemoteScheduler.h
    src/remote/SshConnection.h
    src/remote/SshFileStreamChannel.h
    src/remote/SshGridProvider.h

    # rv
    src/rv/Periodogram.h
    src/rv/PeriodogramRecord.h
    src/rv/RVDetectability.h
    src/rv/RVErrorMC.h
    src/rv/RVMCMC.h
    src/rv/RadialVelocity.h

    # sed
    src/sed/ExtractSED.h
    src/sed/FilterWavelength.h
    src/sed/SedFitEnvironment.h

    # spectra
    src/spectra/Spectrum.h
    src/spectra/SpectrumCoadder.h
    src/spectra/SpectrumFetchService.h
    src/spectra/SpectrumReader.h

    # spectra/fetch
    src/spectra/fetch/ApogeeArchiveClient.h
    src/spectra/fetch/EsoArchiveClient.h
    src/spectra/fetch/ExposureEpoch.h
    src/spectra/fetch/EsoObsCoreIndex.h
    src/spectra/fetch/LamostArchiveClient.h
    src/spectra/fetch/MastArchiveClient.h
    src/spectra/fetch/SdssOpticalArchiveClient.h
    src/spectra/fetch/SpectrumArchiveClient.h
    src/spectra/fetch/SpectrumArchiveRegistry.h
    src/spectra/fetch/SpectrumArchiveTypes.h
    src/spectra/fetch/SpectrumArmJoin.h
    src/spectra/fetch/SpectrumFrame.h
    src/spectra/fetch/TapHelpers.h
    src/spectra/fetch/VoTableReader.h
    src/spectra/fetch/WavelengthConvert.h

    # spectra
    src/spectra/matchSpectraToInstrument.h
)

set(ASTRA_UI_SOURCES

    # app/ui
    src/app/ui/ApplicationController.cpp
    src/app/ui/BackgroundTaskManager.cpp
    src/app/ui/ColumnConfigDialog.cpp
    src/app/ui/EditProjectDialog.cpp
    src/app/ui/ExportTableDialog.cpp
    src/app/ui/FirstRunDialog.cpp
    src/app/ui/InstrumentConfigView.cpp
    src/app/ui/LcquerySetupDialog.cpp
    src/app/ui/MainWindow.cpp
    src/app/ui/NewProjectDialog.cpp
    src/app/ui/PlotPresetStore.cpp
    src/app/ui/ProjectPlotDialog.cpp
    src/app/ui/ProjectSelectionView.cpp
    src/app/ui/ProjectView.cpp
    src/app/ui/SettingsDialog.cpp
    src/app/ui/StarDetailView.cpp
    src/app/ui/StarFilterWidget.cpp
    src/app/ui/ThemeManager.cpp
    src/app/ui/UiIcons.cpp
    src/app/ui/UiStyle.cpp
    src/app/ui/UpdateManager.cpp
    src/app/ui/WhatsNewDialog.cpp
    src/app/ui/WindowSizing.cpp

    # app/ui/panels
    src/app/ui/panels/DetailPanel.cpp
    src/app/ui/panels/DetailPanelFactory.cpp
    src/app/ui/panels/PanelUtils.cpp
    src/app/ui/panels/SummaryPanel.cpp

    # catalog/ui
    src/catalog/ui/AddStarDialog.cpp
    src/catalog/ui/CMDDialog.cpp
    src/catalog/ui/ReidentifyStarDialog.cpp

    # fitting/ui
    src/fitting/ui/FitComponentsWidget.cpp
    src/fitting/ui/FitPreviewOverlay.cpp
    src/fitting/ui/FitProgressDialog.cpp
    src/fitting/ui/FitSetupWidget.cpp
    src/fitting/ui/InteractiveIsisDialog.cpp
    src/fitting/ui/IsisMacroPanel.cpp
    src/fitting/ui/SpectraFitDialog.cpp

    # importwizard
    src/importwizard/GeneralImportPage.cpp
    src/importwizard/ImportStagingArea.cpp
    src/importwizard/LightcurveImportPage.cpp
    src/importwizard/RadialVelocityImportPage.cpp
    src/importwizard/SEDImportPage.cpp
    src/importwizard/SpectraImportPage.cpp
    src/importwizard/SpectralFitImportPage.cpp
    src/importwizard/StarImportWizard.cpp

    # io/ui
    src/io/ui/StarShare.cpp

    # kinematics/ui
    src/kinematics/ui/GalacticOrbitDialog.cpp

    # lightcurve/ui
    src/lightcurve/ui/ImportLightcurve.cpp
    src/lightcurve/ui/LCFitDialog.cpp
    src/lightcurve/ui/LCModelPreview.cpp
    src/lightcurve/ui/LCPanel.cpp
    src/lightcurve/ui/LightcurveCredentialPrompts.cpp
    src/lightcurve/ui/LightcurveFetchDialog.cpp
    src/lightcurve/ui/LightcurveFetchSessionsDialog.cpp

    # massfit/ui
    src/massfit/ui/MassFitManagerDialog.cpp
    src/massfit/ui/MassFitPlanDialog.cpp
    src/massfit/ui/MassFitRuleEditor.cpp

    # observing/ui
    src/observing/ui/CoastlineData.cpp
    src/observing/ui/ObservabilityDialog.cpp

    # remote/ui
    src/remote/ui/AnsiTerminalWidget.cpp
    src/remote/ui/AskPass.cpp
    src/remote/ui/RemoteFitsDialog.cpp
    src/remote/ui/RemoteHostsSettingsPage.cpp
    src/remote/ui/RemoteSelfTest.cpp
    src/remote/ui/TerminalView.cpp

    # rv/ui
    src/rv/ui/PeriodogramPanel.cpp
    src/rv/ui/RVAddFitDialog.cpp
    src/rv/ui/RVAddPointDialog.cpp
    src/rv/ui/RVDetectabilityDialog.cpp
    src/rv/ui/RVImportPointsDialog.cpp
    src/rv/ui/RVInspectorDialog.cpp
    src/rv/ui/RVMCMCResultsDialog.cpp
    src/rv/ui/RVPanel.cpp

    # sed/ui
    src/sed/ui/SEDFitDialog.cpp

    # spectra/ui
    src/spectra/ui/AddSpectraDialog.cpp
    src/spectra/ui/ArchiveFetchWidget.cpp
    src/spectra/ui/CoAddWidget.cpp
    src/spectra/ui/SpectraPanel.cpp
    src/spectra/ui/SpectrumFetchSessionsDialog.cpp

    # ui/widgets
    src/ui/widgets/CheckBoxDragger.cpp
    src/ui/widgets/CheckStateDragger.cpp
    src/ui/widgets/CopyToast.cpp
    src/ui/widgets/ElidedLabel.cpp
    src/ui/widgets/FlowLayout.cpp
    src/ui/widgets/GridSelectorWidget.cpp
    src/ui/widgets/PlotKeyNavigator.cpp
    src/ui/widgets/QuantityDelegate.cpp
    src/ui/widgets/QuantityLabel.cpp
    src/ui/widgets/QuantityRenderer.cpp
    src/ui/widgets/ResponsiveGridLayout.cpp
    src/ui/widgets/ShimmerWidget.cpp
    src/ui/widgets/SquarePlotFrame.cpp
    src/ui/widgets/WheelGuard.cpp
    src/ui/widgets/WidgetFactory.cpp
)

set(ASTRA_UI_HEADERS

    # app/ui
    src/app/ui/ApplicationController.h
    src/app/ui/BackgroundTaskManager.h
    src/app/ui/ColumnConfigDialog.h
    src/app/ui/EditProjectDialog.h
    src/app/ui/ExportTableDialog.h
    src/app/ui/FirstRunDialog.h
    src/app/ui/InstrumentConfigView.h
    src/app/ui/LcquerySetupDialog.h
    src/app/ui/MainWindow.h
    src/app/ui/NewProjectDialog.h
    src/app/ui/PlotPresetStore.h
    src/app/ui/ProjectPlotDialog.h
    src/app/ui/ProjectSelectionView.h
    src/app/ui/ProjectView.h
    src/app/ui/SettingsDialog.h
    src/app/ui/StarDetailView.h
    src/app/ui/StarFilterWidget.h
    src/app/ui/ThemeManager.h
    src/app/ui/UiIcons.h
    src/app/ui/UiStyle.h
    src/app/ui/UpdateManager.h
    src/app/ui/WhatsNewDialog.h
    src/app/ui/WindowSizing.h

    # app/ui/panels
    src/app/ui/panels/DetailPanel.h
    src/app/ui/panels/DetailPanelFactory.h
    src/app/ui/panels/PanelUtils.h
    src/app/ui/panels/SummaryPanel.h

    # catalog/ui
    src/catalog/ui/AddStarDialog.h
    src/catalog/ui/CMDDialog.h
    src/catalog/ui/ReidentifyStarDialog.h

    # fitting/ui
    src/fitting/ui/FitComponentsWidget.h
    src/fitting/ui/FitPreviewOverlay.h
    src/fitting/ui/FitProgressDialog.h
    src/fitting/ui/FitSetupWidget.h
    src/fitting/ui/InteractiveIsisDialog.h
    src/fitting/ui/IsisMacroPanel.h
    src/fitting/ui/SpectraFitDialog.h

    # importwizard
    src/importwizard/GeneralImportPage.h
    src/importwizard/ImportStagingArea.h
    src/importwizard/LightcurveImportPage.h
    src/importwizard/RadialVelocityImportPage.h
    src/importwizard/SEDImportPage.h
    src/importwizard/SpectraImportPage.h
    src/importwizard/SpectralFitImportPage.h
    src/importwizard/StarImportWizard.h

    # io/ui
    src/io/ui/StarShare.h

    # kinematics/ui
    src/kinematics/ui/GalacticOrbitDialog.h

    # lightcurve/ui
    src/lightcurve/ui/ImportLightcurve.h
    src/lightcurve/ui/LCFitDialog.h
    src/lightcurve/ui/LCModelPreview.h
    src/lightcurve/ui/LCPanel.h
    src/lightcurve/ui/LightcurveCredentialPrompts.h
    src/lightcurve/ui/LightcurveFetchDialog.h
    src/lightcurve/ui/LightcurveFetchSessionsDialog.h

    # massfit/ui
    src/massfit/ui/MassFitManagerDialog.h
    src/massfit/ui/MassFitPlanDialog.h
    src/massfit/ui/MassFitRuleEditor.h

    # observing/ui
    src/observing/ui/CoastlineData.h
    src/observing/ui/ObservabilityDialog.h

    # remote/ui
    src/remote/ui/AnsiTerminalWidget.h
    src/remote/ui/AskPass.h
    src/remote/ui/RemoteFitsDialog.h
    src/remote/ui/RemoteHostsSettingsPage.h
    src/remote/ui/RemoteSelfTest.h
    src/remote/ui/TerminalView.h

    # rv/ui
    src/rv/ui/PeriodogramPanel.h
    src/rv/ui/RVAddFitDialog.h
    src/rv/ui/RVAddPointDialog.h
    src/rv/ui/RVDetectabilityDialog.h
    src/rv/ui/RVImportPointsDialog.h
    src/rv/ui/RVInspectorDialog.h
    src/rv/ui/RVMCMCResultsDialog.h
    src/rv/ui/RVPanel.h

    # sed/ui
    src/sed/ui/SEDFitDialog.h

    # spectra/ui
    src/spectra/ui/AddSpectraDialog.h
    src/spectra/ui/ArchiveFetchWidget.h
    src/spectra/ui/CoAddWidget.h
    src/spectra/ui/SpectraPanel.h
    src/spectra/ui/SpectrumFetchSessionsDialog.h

    # ui/widgets
    src/ui/widgets/BooleanColumnDelegate.h
    src/ui/widgets/CheckBoxDragger.h
    src/ui/widgets/CheckStateDragger.h
    src/ui/widgets/CopyToast.h
    src/ui/widgets/ElidedLabel.h
    src/ui/widgets/FlowLayout.h
    src/ui/widgets/GridSelectorWidget.h
    src/ui/widgets/PlotKeyNavigator.h
    src/ui/widgets/PreciseDoubleSpinBox.h
    src/ui/widgets/QuantityDelegate.h
    src/ui/widgets/QuantityLabel.h
    src/ui/widgets/QuantityRenderer.h
    src/ui/widgets/ResponsiveGridLayout.h
    src/ui/widgets/ShimmerWidget.h
    src/ui/widgets/SquarePlotFrame.h
    src/ui/widgets/WheelGuard.h
    src/ui/widgets/WidgetFactory.h
)

# ── astra_core: every Core-only translation unit ─────────────────────────────
# The application and all test binaries link this, so a test never has to
# re-list the sources it needs.  Third-party dependencies are attached here
# once (PUBLIC) and inherited by everything downstream.
add_library(astra_core STATIC
    ${ASTRA_CORE_SOURCES}
    ${ASTRA_CORE_HEADERS}
)
target_include_directories(astra_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${ASTRA_GAEL_SOURCE_DIR}/include
)
target_link_libraries(astra_core PUBLIC
    Qt6::Core
    Qt6::Sql
    Qt6::Network
    Qt6::Concurrent
    GAEL::core
)
set_target_properties(astra_core PROPERTIES AUTOMOC ON)

# ── Create executable: main + everything under a ui/ folder ──────────────────
qt_add_executable(ASTRA
    src/main.cpp
    ${ASTRA_UI_SOURCES}
    ${ASTRA_UI_HEADERS}
    resources/resources.qrc
)
target_link_libraries(ASTRA PRIVATE astra_core)

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
add_dependencies(astra_core astra_version)


# Pass to source as preprocessor defines.
#
# ASTRA_DATA_DIR stays PRIVATE on every target: AppPaths::initialize() is
# inline, so each translation unit bakes in the value it was compiled with,
# and test_eso_obscore_index deliberately overrides it with its own directory.
# Exporting it PUBLIC would give that test two conflicting definitions.
set(ASTRA_COMMON_DEFINES
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

target_compile_definitions(astra_core
    PUBLIC  ${ASTRA_COMMON_DEFINES}
    PRIVATE ASTRA_DATA_DIR="${ASTRA_DATA_DIR}"
)
target_compile_definitions(ASTRA PRIVATE ASTRA_DATA_DIR="${ASTRA_DATA_DIR}")

# Profiling option
option(ENABLE_PROFILING "Build with profiling support (debug symbols + frame pointers)" OFF)


if(ENABLE_PROFILING)
    message(STATUS "Profiling enabled: -g -fno-omit-frame-pointer")
    set(PROFILING_FLAGS -O2 -g -fno-omit-frame-pointer)

    target_compile_options(ASTRA       PRIVATE ${PROFILING_FLAGS})
    target_compile_options(astra_core  PRIVATE ${PROFILING_FLAGS})
    target_compile_options(qcustomplot PRIVATE ${PROFILING_FLAGS})

    target_link_options(ASTRA PRIVATE -rdynamic)
endif()


# Link libraries
# Qt6::Core/Sql/Network/Concurrent and GAEL::core arrive through astra_core.
target_link_libraries(ASTRA PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Svg
    qcustomplot
)

# ── FFTW3 ──────────────────────────────────────────────────────────────────
# Used directly by Periodogram.cpp (GLS via the FFT-based trig sums). ASTRA
# used to pick this up by accident, as a transitive dependency of the RV_MCMC
# submodule; it is a first-class dependency of our own code now.
if(PkgConfig_FOUND)
    pkg_check_modules(FFTW3 fftw3)
endif()
if(FFTW3_FOUND)
    target_include_directories(astra_core PUBLIC ${FFTW3_INCLUDE_DIRS})
    target_link_libraries(astra_core PUBLIC ${FFTW3_LIBRARIES})
    target_link_directories(astra_core PUBLIC ${FFTW3_LIBRARY_DIRS})
else()
    find_path(FFTW3_INCLUDE_DIR fftw3.h)
    find_library(FFTW3_LIBRARY NAMES fftw3 libfftw3)
    if(NOT FFTW3_INCLUDE_DIR OR NOT FFTW3_LIBRARY)
        message(FATAL_ERROR "FFTW3 not found - required by src/rv/Periodogram.cpp")
    endif()
    message(STATUS "Found FFTW3: ${FFTW3_LIBRARY}")
    target_include_directories(astra_core PUBLIC ${FFTW3_INCLUDE_DIR})
    target_link_libraries(astra_core PUBLIC ${FFTW3_LIBRARY})
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
    set_source_files_properties(src/rv/RVMCMC.cpp PROPERTIES
        COMPILE_OPTIONS "${ASTRA_OPENMP_FLAGS}")
    target_include_directories(astra_core PUBLIC ${OpenMP_CXX_INCLUDE_DIRS})
    target_link_libraries(astra_core PUBLIC ${OpenMP_CXX_LIBRARIES})
else()
    message(WARNING "OpenMP not found - the RV-MCMC sampler will be single-threaded")
endif()

# Add CCfits support if available
if(CCFITS_FOUND)
    # PUBLIC: HAVE_CCFITS is consumed by UI translation units too.
    target_compile_definitions(astra_core PUBLIC HAVE_CCFITS)
    target_include_directories(astra_core PUBLIC ${CCFITS_INCLUDE_DIRS})
    target_link_libraries(astra_core PUBLIC ${CCFITS_LIBRARIES})
    
    # On some systems, we might need to add additional math libraries
    if(UNIX AND NOT APPLE)
        target_link_libraries(astra_core PUBLIC m)
    endif()
endif()

# Platform-specific settings
if(WIN32)
    # WIN32_EXECUTABLE keeps the GUI subsystem, so launching ASTRA never opens
    # a console window behind the main window.
    set_target_properties(ASTRA PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    # --- App icon + VERSIONINFO -----------------------------------------
    # Compiled into the executable itself. The installer's shortcut icon is a
    # separate thing (build-windows.sh points Inno Setup at the same .ico);
    # without this resource the .exe on disk still shows the generic icon.
    # RC is not in the project()'s language list, so enable it here rather
    # than everywhere -- it exists only on Windows toolchains.
    enable_language(RC)

    # VERSIONINFO wants exactly four comma-separated numbers. Release CI passes
    # the tag (v0.7.0) in ASTRA_VERSION_OVERRIDE; a dev stamp such as
    # git-<hash> is not a version at all, so fall back to the project version
    # rather than emit an .rc the resource compiler rejects.
    set(_astra_win_version "${PROJECT_VERSION}")
    if(ASTRA_VERSION_OVERRIDE MATCHES "^v?([0-9]+(\\.[0-9]+)*)$")
        set(_astra_win_version "${CMAKE_MATCH_1}")
    endif()
    string(REPLACE "." ";" _astra_win_version_parts "${_astra_win_version}")
    list(LENGTH _astra_win_version_parts _astra_win_version_n)
    while(_astra_win_version_n LESS 4)
        list(APPEND _astra_win_version_parts 0)
        math(EXPR _astra_win_version_n "${_astra_win_version_n} + 1")
    endwhile()
    list(SUBLIST _astra_win_version_parts 0 4 _astra_win_version_parts)
    list(JOIN _astra_win_version_parts "," ASTRA_WIN_VERSION_CSV)
    set(ASTRA_WIN_VERSION_DOT "${_astra_win_version}")

    # configure_file writes the .rc next to the generated icon path, so the
    # ICON statement needs an absolute path with forward slashes -- windres
    # treats a backslash in a resource string as an escape.
    set(ASTRA_WINDOWS_ICON
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/astra.ico")
    if(EXISTS "${ASTRA_WINDOWS_ICON}")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/astra.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/astra.rc"
            @ONLY)
        target_sources(ASTRA PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/astra.rc")
    else()
        message(WARNING "resources/windows/astra.ico is missing; ASTRA.exe "
                        "will use the generic application icon.")
    endif()

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

    # ── Test helper ──────────────────────────────────────────────────────
    # Every test binary links astra_core, so a test lists only its own source.
    #
    #   astra_add_test(<ctest name> [TARGET t] [SOURCES ...] [LABELS ...]
    #                  [TIMEOUT s] [DEFINES ...] [ARGS ...] [DATA_DIR dir])
    #
    # TARGET defaults to test_<name> and SOURCES to tests/<target>.cpp.
    # ASTRA_DATA_DIR is set per test so a test can point at its own sandbox.
    function(astra_add_test NAME)
        cmake_parse_arguments(T "" "TARGET;TIMEOUT;DATA_DIR"
                                 "SOURCES;LABELS;DEFINES;LIBS;ARGS" ${ARGN})
        if(NOT T_TARGET)
            set(T_TARGET test_${NAME})
        endif()
        if(NOT T_SOURCES)
            set(T_SOURCES tests/${T_TARGET}.cpp)
        endif()
        if(NOT T_TIMEOUT)
            set(T_TIMEOUT 30)
        endif()
        if(NOT T_DATA_DIR)
            set(T_DATA_DIR "${ASTRA_DATA_DIR}")
        endif()
        add_executable(${T_TARGET} ${T_SOURCES})
        target_link_libraries(${T_TARGET} PRIVATE astra_core ${T_LIBS})
        target_compile_definitions(${T_TARGET} PRIVATE
            ASTRA_DATA_DIR="${T_DATA_DIR}"
            ${T_DEFINES}
        )
        add_test(
            NAME    ${NAME}
            COMMAND ${T_TARGET} ${T_ARGS}
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )
        set_tests_properties(${NAME} PROPERTIES
            LABELS "${T_LABELS}"
            TIMEOUT ${T_TIMEOUT}
        )
        set_property(GLOBAL APPEND PROPERTY ASTRA_TEST_TARGETS ${T_TARGET})
    endfunction()

    # ── Offline tests ────────────────────────────────────────────────────
    astra_add_test(quantity_format  TARGET test_quantityformat LABELS utils formatting)
    astra_add_test(star_search      LABELS utils search)
    astra_add_test(star_matching    LABELS utils import)
    astra_add_test(wavelength_convert LABELS utils spectrafetch)
    astra_add_test(spectrum_frame   LABELS utils spectrafetch TIMEOUT 60)
    astra_add_test(arm_join         LABELS utils spectrafetch)
    astra_add_test(votable_reader   LABELS utils spectrafetch)
    astra_add_test(massfit_plan     LABELS models massfit)
    astra_add_test(phase_lock       LABELS utils lightcurve rv)
    astra_add_test(instrument_match LABELS utils
                   DEFINES ASTRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    astra_add_test(rv_mcmc_sampler  TARGET test_rvmcmc LABELS fitting statistics TIMEOUT 300)

    # Skips itself unless the ASTRA_TEST_* fixture paths point at real FITS files.
    astra_add_test(spectrafetch_parsers LABELS utils spectrafetch TIMEOUT 120)

    # ── Live-archive tests ───────────────────────────────────────────────
    # These hit the real archives; they self-skip unless ASTRA_TEST_*_LIVE is set.
    # Exclude them with `ctest -LE network`.
    astra_add_test(eso_bulk_discovery  LABELS utils spectrafetch network TIMEOUT 900)
    astra_add_test(mast_bulk_discovery LABELS utils spectrafetch network TIMEOUT 900)
    astra_add_test(sdss_discovery      LABELS utils spectrafetch network TIMEOUT 900)
    astra_add_test(eso_obscore_index   LABELS utils spectrafetch network TIMEOUT 1800
                   DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/eso_index_test")

    # ── Barycentric correction, checked against astropy ──────────────────
    astra_add_test(barycentric_correction TARGET test_barycentric
                   LABELS models astronomy
                   ARGS "${REFERENCE_JSON}"
                   DEFINES REFERENCE_BJD_PATH="${REFERENCE_JSON}")
    if(TARGET generate_reference)
        add_dependencies(test_barycentric generate_reference)
    endif()

    # ── doctest unit tests ───────────────────────────────────────────────
    # One binary for all new unit tests: they all link astra_core (plus GAEL and
    # Qt through it), so linking dominates and a binary per domain would cost
    # more than it buys.  Grouping is by TEST_SUITE instead:
    #     ./astra_tests -ts=core        run one domain
    #     ./astra_tests -ltc            list the test cases
    add_library(doctest INTERFACE)
    target_include_directories(doctest INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/doctest)

    astra_add_test(unit
        TARGET  astra_tests
        LABELS  unit doctest
        TIMEOUT 300
        LIBS    doctest
        DEFINES ASTRA_TEST_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/reference"
        SOURCES
            # The Qt resource bundle: several Core units read their reference
            # data from it (the TESS sector table, the Claret limb-darkening
            # grids, the default instruments), so the tests need it too. It is
            # listed on the executable rather than on astra_core because static
            # libraries need explicit resource initialisation.
            resources/resources.qrc
            tests/doctest_main.cpp
            tests/unit/core/time.cpp
            tests/unit/core/stats.cpp
            tests/unit/core/delimited_table.cpp
            tests/unit/core/asymmetric_errors.cpp
            tests/unit/core/filter_expression.cpp
            tests/unit/core/barycentric_parts.cpp
            tests/unit/spectra/exposure_epoch.cpp
            tests/unit/rv/rvfit.cpp
            tests/unit/lightcurve/lcfit_physics.cpp
            tests/unit/kinematics/galactic.cpp
            tests/unit/observing/observability.cpp
            tests/unit/fitting/fit_types_json.cpp
            tests/unit/rv/periodogram.cpp
            tests/unit/io/star_package.cpp
            tests/unit/rv/rv_error_mc.cpp
            tests/unit/db/delete_star.cpp
    )

    # ── Convenience target ───────────────────────────────────────────────
    # `cmake --build build --target check` builds and runs all tests
    get_property(ASTRA_TEST_TARGETS GLOBAL PROPERTY ASTRA_TEST_TARGETS)
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS ${ASTRA_TEST_TARGETS}
        COMMENT "Running all tests..."
    )
endif()
