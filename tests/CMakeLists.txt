# Test harnesses. Each is a standalone executable that prints its own results
# and exits non-zero on failure, so it can be run by hand as well as via ctest.
#
# They link sir-launchalot-lib, so they always test the code the app ships.

# Where the UI harness finds its seeded config.
set(TEST_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

function(add_harness name)
    add_executable(${name} ${name}/main.cpp)
    target_link_libraries(${name} PRIVATE sir-launchalot-lib)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_harness(proton)
add_harness(account)
add_harness(addon)
add_harness(env)

# Drives the real MainWindow, so it needs a config to load. It selects the
# offscreen platform itself, so running it by hand behaves the same as ctest.
add_harness(ui-context-menu)
target_compile_definitions(ui-context-menu PRIVATE
    TEST_FIXTURE_DIR="${TEST_FIXTURE_DIR}")

add_test(NAME prefix-sync
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/prefix-sync.sh)
