add_library(alerts STATIC
    LogAlert.cpp
)

target_include_directories(alerts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
apply_compiler_options(alerts)
