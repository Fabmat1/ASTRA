# Build a macOS .icns from a PNG or SVG source using tools that ship with the
# OS (sips, iconutil). Run in script mode from a custom command:
#
#   cmake -DICON_SRC=<png|svg> -DOUT_ICNS=<out.icns> -DWORK_DIR=<scratch dir>
#         -P cmake/MakeMacIcns.cmake
#
# Kept out of CMakeLists.txt because the iconset has to be built as a real
# directory of files before iconutil will look at it.

if(NOT ICON_SRC OR NOT OUT_ICNS OR NOT WORK_DIR)
    message(FATAL_ERROR "MakeMacIcns.cmake needs ICON_SRC, OUT_ICNS and WORK_DIR")
endif()

set(_iconset "${WORK_DIR}/astra.iconset")
set(_base    "${WORK_DIR}/astra_1024.png")
file(REMOVE_RECURSE "${_iconset}")
file(MAKE_DIRECTORY "${_iconset}")

# Rasterise to one big square PNG first: sips reads PNG but not SVG.
if(ICON_SRC MATCHES "\\.svg$")
    find_program(RSVG_CONVERT rsvg-convert)
    if(RSVG_CONVERT)
        execute_process(COMMAND "${RSVG_CONVERT}" -w 1024 -h 1024 "${ICON_SRC}"
                                -o "${_base}"
                        RESULT_VARIABLE _rc)
    else()
        # qlmanage ships with macOS; it names its output <source>.png.
        execute_process(COMMAND qlmanage -t -s 1024 -o "${WORK_DIR}" "${ICON_SRC}"
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE _rc)
        get_filename_component(_srcname "${ICON_SRC}" NAME)
        if(EXISTS "${WORK_DIR}/${_srcname}.png")
            file(RENAME "${WORK_DIR}/${_srcname}.png" "${_base}")
        else()
            set(_rc 1)
        endif()
    endif()
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Could not rasterise ${ICON_SRC}; install librsvg "
                            "or ship a PNG icon source instead.")
    endif()
else()
    execute_process(COMMAND sips -s format png -z 1024 1024 "${ICON_SRC}"
                            --out "${_base}"
                    OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "sips failed to read ${ICON_SRC}")
    endif()
endif()

# iconutil rejects the whole set if it finds a file outside the canonical name
# list, so no icon_64x64.png here: 64px is covered by icon_32x32@2x.png.
foreach(_sz 16 32 128 256 512)
    math(EXPR _sz2 "${_sz} * 2")
    execute_process(COMMAND sips -z ${_sz} ${_sz} "${_base}"
                            --out "${_iconset}/icon_${_sz}x${_sz}.png"
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND sips -z ${_sz2} ${_sz2} "${_base}"
                            --out "${_iconset}/icon_${_sz}x${_sz}@2x.png"
                    OUTPUT_QUIET ERROR_QUIET)
endforeach()

execute_process(COMMAND iconutil -c icns "${_iconset}" -o "${OUT_ICNS}"
                RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${OUT_ICNS}")
    message(FATAL_ERROR "iconutil failed to produce ${OUT_ICNS}")
endif()
