# Faceit_RoundEndStats — Plugify port of the Metamod plugin cs2-faceit_roundendstats (ABKAM):
# per-round damage statistics printed to every player at round end.
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no guard library, no hudmenu. The map
# gate, the client/controller checks, the event hooks and the chat output live in src/plugin.cpp;
# the engine-free parts (src/faceit_stats_core.*, src/kv1.*, src/map_gate.hpp, src/ent_handle.hpp,
# src/chat_text.hpp) are what the tests compile in.
#
#   plugify-Faceit_RoundEndStats  the plugin shared library (entry "bin/Faceit_RoundEndStats")
#   faceit_stats_core             the engine-independent damage bookkeeping + KV1/phrase reader
#   faceit_stats_core_tests       the damage/kill bookkeeping unit tests
#   faceit_stats_engine_tests     the KV1/phrases/language/map-gate/colour/handle-rule tests
#   faceit_stats_plugin_tests     the real event callbacks against fake s2sdk bindings

set(FACEIT_ROUNDENDSTATS_VERSION "1.1.0")

# --- s2sdk bindings ------------------------------------------------------------
# The __s2sdk_* binding globals have to be compiled into every plugin; the root CMakeLists builds
# them as the object library `s2sdk_bindings`. This is the standalone fallback for a configure that
# only includes this directory - named differently on purpose, so it can never collide with the
# root target when the whole tree is configured.
if(TARGET s2sdk_bindings)
    set(FACEIT_S2SDK_BINDINGS "$<TARGET_OBJECTS:s2sdk_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    add_library(faceit_roundendstats_s2sdk_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    target_include_directories(faceit_roundendstats_s2sdk_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(faceit_roundendstats_s2sdk_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(FACEIT_S2SDK_BINDINGS "$<TARGET_OBJECTS:faceit_roundendstats_s2sdk_bindings>")
else()
    message(FATAL_ERROR
            "Faceit_RoundEndStats needs the generated s2sdk bindings (sdk/include/s2sdk/s2sdk.cpp)")
endif()

# --- engine-independent core --------------------------------------------------
add_library(faceit_stats_core STATIC
        src/faceit_stats_core.cpp
        src/kv1.cpp
)
target_include_directories(faceit_stats_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(faceit_stats_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(faceit_stats_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-Faceit_RoundEndStats SHARED
        src/plugin.cpp
        ${FACEIT_S2SDK_BINDINGS}
)
target_include_directories(plugify-Faceit_RoundEndStats PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated plg/s2sdk headers must not fail -Werror on our sources.
target_include_directories(plugify-Faceit_RoundEndStats SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-Faceit_RoundEndStats PRIVATE
        plugify_sdk
        faceit_stats_core
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
# Engine-free bookkeeping tests (the damage/kill table of the original).
add_executable(faceit_stats_core_tests tests/faceit_stats_core_tests.cpp)
target_link_libraries(faceit_stats_core_tests PRIVATE faceit_stats_core -static-libstdc++ -static-libgcc)
target_compile_options(faceit_stats_core_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(faceit_stats_core_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(faceit_stats_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Engine-free tests of the local KV1 reader and phrase resolution (the production phrases file from
# tests/data), the language normalization, every map-gate transition, the entity-handle rule, the
# colour table and the round-end formatting. Nothing here links s2sdk, pisexcompat or the host.
add_executable(faceit_stats_engine_tests tests/faceit_stats_engine_tests.cpp)
target_link_libraries(faceit_stats_engine_tests PRIVATE
        faceit_stats_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(faceit_stats_engine_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(faceit_stats_engine_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        FACEIT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(faceit_stats_engine_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Engine test of the real event callbacks: src/plugin.cpp is compiled into the test together with
# the generated binding objects, whose __s2sdk_* globals are null until the test assigns counting
# fakes. It drives the plugin through its own Plugify_* entry points and fires round_start/
# player_hurt/player_death/round_end/player_connect_full, proving the accessor split, slots 0/-1,
# the map teardown, the ru/en language chain, the handle checks and the symmetric teardown.
add_executable(faceit_stats_plugin_tests
        tests/faceit_stats_plugin_tests.cpp
        src/plugin.cpp
        ${FACEIT_S2SDK_BINDINGS}
)
target_include_directories(faceit_stats_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated plg/s2sdk headers must not fail -Werror on our sources
# (src/plugin.cpp is one of them).
target_include_directories(faceit_stats_plugin_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(faceit_stats_plugin_tests PRIVATE
        plugify_sdk
        faceit_stats_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(faceit_stats_plugin_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(faceit_stats_plugin_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="Faceit_RoundEndStats"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        FACEIT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(faceit_stats_plugin_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
