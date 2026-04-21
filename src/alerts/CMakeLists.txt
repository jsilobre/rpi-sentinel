add_library(alerts STATIC
    LogAlert.cpp
)

target_include_directories(alerts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
apply_compiler_options(alerts)

if(ENABLE_MQTT)
    target_sources(alerts PRIVATE MqttPublisher.cpp)
    target_compile_definitions(alerts PUBLIC ENABLE_MQTT)
    target_include_directories(alerts PRIVATE ${MOSQUITTO_INCLUDE_DIRS})
    target_link_libraries(alerts PUBLIC ${MOSQUITTO_LIBRARIES} nlohmann_json::nlohmann_json)
    target_link_directories(alerts PUBLIC ${MOSQUITTO_LIBRARY_DIRS})
endif()
