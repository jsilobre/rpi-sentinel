include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rpi_tests
    test_event_bus.cpp
    test_threshold_monitor.cpp
    test_config_loader.cpp
)

target_link_libraries(rpi_tests PRIVATE
    monitoring
    GTest::gtest_main
)

apply_compiler_options(rpi_tests)

include(GoogleTest)
gtest_discover_tests(rpi_tests)
