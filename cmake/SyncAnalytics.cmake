# Copies the analytics scripts and their config templates into the build tree.
#
# Run in script mode by the sync_analytics target in CMakeLists.txt:
#   cmake -DSRC=<source analytics dir> -DDST=<exe dir>/analytics -P SyncAnalytics.cmake
#
# This is an allowlist on purpose. The source analytics/ folder also collects
# runtime output whenever a script or test is run from it -- vortex.sqlite3 and
# its -wal/-shm files, recommendations*.json, model/*.pkl, .igdb_token.json,
# feedback_events.log -- and the copy beside the exe holds the user's real
# versions of those. Copying the whole folder would overwrite them. .env is
# excluded for the same reason: the first-run wizard writes it beside the exe.
#
# Globbing here, at build time, means a newly added script is picked up without
# re-running the configure step. configure_file(COPYONLY) only rewrites a file
# whose contents changed, so an unchanged script keeps its timestamp.

if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "SyncAnalytics.cmake needs -DSRC=... and -DDST=...")
endif()

file(GLOB_RECURSE _vortex_analytics_files
    RELATIVE "${SRC}"
    "${SRC}/*.py"
    "${SRC}/*.sql"
)
list(APPEND _vortex_analytics_files
    .env.example
    requirements.txt
)

foreach(_rel IN LISTS _vortex_analytics_files)
    if(EXISTS "${SRC}/${_rel}")
        configure_file("${SRC}/${_rel}" "${DST}/${_rel}" COPYONLY)
    endif()
endforeach()
