# Run at BUILD time. Writes ${OUT} with ASTRA_VERSION_STRING defined as:
#   • ${VERSION_OVERRIDE}           -> when explicitly stamped (release CI)
#   • the exact tag                 -> when HEAD is *exactly* on a tag and the
#                                      working tree is clean (a real release)
#   • git-<shorthash>[-dirty]       -> any other build from a git checkout
#   • ${FALLBACK_VERSION}           -> when git is unavailable (e.g. a tarball)
#
# The dirty check deliberately ignores submodule pointer changes and untracked
# files, so a clean superproject is reported as clean even when vendored
# submodules (external/*) have local modifications.

set(_version "${FALLBACK_VERSION}")

# An explicit stamp wins over anything git says. Only release pipelines set it
# (build-macos.sh on a tag build, via ASTRA_VERSION_OVERRIDE) — a normal build
# must keep reporting what git actually sees.
if(VERSION_OVERRIDE)
    set(_content "#pragma once\n#define ASTRA_VERSION_STRING \"${VERSION_OVERRIDE}\"\n")
    if(EXISTS "${OUT}")
        file(READ "${OUT}" _old)
    else()
        set(_old "")
    endif()
    if(NOT _old STREQUAL _content)
        file(WRITE "${OUT}" "${_content}")
    endif()
    return()
endif()

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _hash OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _hash_rc)

    if(_hash_rc EQUAL 0 AND _hash)
        # Is HEAD exactly on a tag?
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _tag_rc)

        # Is the superproject dirty? (ignore submodules + untracked files)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
                    --untracked-files=no --ignore-submodules=all
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

        if(_tag_rc EQUAL 0 AND _tag AND NOT _dirty)
            set(_version "${_tag}")                 # clean release → the tag
        else()
            set(_version "git-${_hash}")            # dev build → git-<hash>
            if(_dirty)
                set(_version "${_version}-dirty")
            endif()
        endif()
    endif()
endif()

set(_content "#pragma once\n#define ASTRA_VERSION_STRING \"${_version}\"\n")

# Only rewrite if changed, to avoid needless recompiles.
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
else()
    set(_old "")
endif()
if(NOT _old STREQUAL _content)
    file(WRITE "${OUT}" "${_content}")
endif()
