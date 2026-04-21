add_subdirectory(alerts)
add_subdirectory(events)
add_subdirectory(sensors)
add_subdirectory(monitoring)
add_subdirectory(web)

add_executable(rpi-sentinel main.cpp)
target_link_libraries(rpi-sentinel PRIVATE monitoring web)
apply_compiler_options(rpi-sentinel)
