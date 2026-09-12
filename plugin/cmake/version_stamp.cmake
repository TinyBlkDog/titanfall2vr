# Regenerate version.h AT BUILD TIME, so the commit in the startup banner is the
# commit the DLL was built from.
#
# WHY THIS EXISTS. The sha used to be captured during `cmake` configure, and
# configure only re-runs when something it depends on changes -- so every build
# between two CMakeLists edits stamped the same old commit. The 2026-09-11 smoke
# test proved it: a DLL built at 21:36 announced itself as `ccaf2e3`, a commit
# from several hours and a dozen changes earlier. An identity line that names
# the wrong build is worse than no identity line, because a bug report carrying
# it sends the reader to the wrong source.
#
# Writes to a temp file and then copy_if_different, so a build that did NOT
# change the commit does not touch version.h and does not force everything that
# includes it to recompile.
#
# Inputs, all -D'd by the caller: TF2VR_VERSION_TAG, TF2VR_VERSION,
# TF2VR_PRODUCT_NAME, TF2VR_AUTHOR, TF2VR_SOURCE_DIR, TF2VR_IN, TF2VR_OUT.

set(TF2VR_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${TF2VR_SOURCE_DIR}"
        OUTPUT_VARIABLE _sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_sha)
        set(TF2VR_GIT_SHA "${_sha}")
        # A dirty tree is not the commit it claims to be. Saying so costs one
        # character and stops a report being traced to source that never built.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${TF2VR_SOURCE_DIR}"
            OUTPUT_VARIABLE _dirty
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_dirty)
            set(TF2VR_GIT_SHA "${_sha}+")
        endif()
    endif()
endif()

configure_file("${TF2VR_IN}" "${TF2VR_OUT}.tmp" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${TF2VR_OUT}.tmp" "${TF2VR_OUT}")
