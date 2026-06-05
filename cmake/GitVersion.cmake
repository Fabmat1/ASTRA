# Run at BUILD time. Writes ${OUT} with ASTRA_VERSION_STRING defined as:
#   • the exact tag, if HEAD is exactly on an annotated/lightweight tag
#   • otherwise the short commit hash (with -dirty suffix if the tree is dirty)
# Falls back to the CMake project version if git is unavailable.

set(_version "${FALLBACK_VERSION}")

find_package(Git QUIET)
if(GIT_FOUND)
    # Exact tag on HEAD? -> use it.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _tag_rc)

    if(_tag_rc EQUAL 0 AND _tag)
        set(_version "${_tag}")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _hash
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _hash_rc)
        if(_hash_rc EQUAL 0 AND _hash)
            # mark a dirty working tree
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" status --porcelain
                WORKING_DIRECTORY "${SRC_DIR}"
                OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_dirty)
                set(_version "${_hash}-dirty")
            else()
                set(_version "${_hash}")
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
