add_library(sensors STATIC
    CpuTempReader.cpp
    DS18B20Reader.cpp
    DHT11Reader.cpp
    SGP30Reader.cpp
    SimulatedSensor.cpp
)

target_include_directories(sensors PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
apply_compiler_options(sensors)
