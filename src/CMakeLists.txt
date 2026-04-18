add_subdirectory(alerts)
add_subdirectory(events)
add_subdirectory(sensors)
add_subdirectory(monitoring)
add_subdirectory(web)

add_executable(rpi-temp-monitor main.cpp)
target_link_libraries(rpi-temp-monitor PRIVATE monitoring web)
apply_compiler_options(rpi-temp-monitor)
