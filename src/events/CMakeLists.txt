add_library(events STATIC
    EventBus.cpp
)

target_include_directories(events PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(events PUBLIC alerts)
apply_compiler_options(events)
