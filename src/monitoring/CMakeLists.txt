add_library(monitoring STATIC
    ThresholdMonitor.cpp
    ConfigLoader.cpp
    MonitoringHub.cpp
)

target_include_directories(monitoring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(monitoring PUBLIC sensors events nlohmann_json::nlohmann_json)
apply_compiler_options(monitoring)
