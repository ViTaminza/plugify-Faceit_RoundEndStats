# Faceit_RoundEndStats — Plugify port of the Metamod plugin cs2-faceit_roundendstats (ABKAM):
# per-round damage statistics printed to every player at round end.
#
#   plugify-Faceit_RoundEndStats  the plugin shared library (entry "bin/Faceit_RoundEndStats")
#   faceit_stats_core             the engine-independent damage bookkeeping (+ its tests)

set(FACEIT_ROUNDENDSTATS_VERSION "1.0.1")

# --- engine-independent core --------------------------------------------------
add_library(faceit_stats_core STATIC src/faceit_stats_core.cpp)
target_include_directories(faceit_stats_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(faceit_stats_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(faceit_stats_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-Faceit_RoundEndStats SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-Faceit_RoundEndStats PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated pisexcompat/plg/s2sdk headers must not fail -Werror on us.
target_include_directories(plugify-Faceit_RoundEndStats SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-Faceit_RoundEndStats PRIVATE
        faceit_stats_core
        pisexcompat
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-Faceit_RoundEndStats PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-Faceit_RoundEndStats PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="Faceit_RoundEndStats"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-Faceit_RoundEndStats PROPERTIES OUTPUT_NAME Faceit_RoundEndStats)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-Faceit_RoundEndStats PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/Faceit_RoundEndStats.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Faceit_RoundEndStats.pplugin"
)

# --- tests --------------------------------------------------------------------
add_executable(faceit_stats_core_tests
        tests/faceit_stats_core_tests.cpp
        src/faceit_stats_core.cpp
)
target_include_directories(faceit_stats_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(faceit_stats_core_tests PRIVATE -Wall -Wextra -Werror)
set_target_properties(faceit_stats_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
