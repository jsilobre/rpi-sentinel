include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rpi_tests
    test_cpu_temp_reader.cpp
    test_event_bus.cpp
    test_threshold_monitor.cpp
    test_config_loader.cpp
    test_history_store.cpp
    test_web_state_hydration.cpp
)

target_link_libraries(rpi_tests PRIVATE
    monitoring
    persistence
    web
    GTest::gtest_main
)

if(ENABLE_MQTT)
    target_sources(rpi_tests PRIVATE test_mqtt_history_responder.cpp)
    target_link_libraries(rpi_tests PRIVATE alerts)
endif()

if(ENABLE_OTLP)
    target_sources(rpi_tests PRIVATE test_otlp_exporter.cpp)
    target_link_libraries(rpi_tests PRIVATE otel)
endif()

apply_compiler_options(rpi_tests)

include(GoogleTest)
gtest_discover_tests(rpi_tests)
