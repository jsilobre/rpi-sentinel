add_library(monitoring STATIC
    ThresholdMonitor.cpp
)

target_include_directories(monitoring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(monitoring PUBLIC sensors events)
apply_compiler_options(monitoring)
