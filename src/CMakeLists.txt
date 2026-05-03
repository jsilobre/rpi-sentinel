add_subdirectory(alerts)
add_subdirectory(events)
add_subdirectory(sensors)
add_subdirectory(monitoring)
add_subdirectory(web)
add_subdirectory(persistence)
add_subdirectory(otel)

add_executable(rpi-sentinel main.cpp)
target_link_libraries(rpi-sentinel PRIVATE monitoring web persistence otel)
apply_compiler_options(rpi-sentinel)

install(TARGETS rpi-sentinel
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
