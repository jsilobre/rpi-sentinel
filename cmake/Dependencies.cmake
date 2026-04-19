include(FetchContent)
find_package(PkgConfig QUIET)

# MQTT support — auto-detected via pkg-config, overridable with -DENABLE_MQTT=ON/OFF
if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO QUIET libmosquitto)
endif()

option(ENABLE_MQTT "Enable MQTT publishing via libmosquitto" ${MOSQUITTO_FOUND})

if(ENABLE_MQTT)
    if(NOT MOSQUITTO_FOUND)
        message(FATAL_ERROR "ENABLE_MQTT=ON but libmosquitto not found. Install libmosquitto-dev.")
    endif()
    message(STATUS "MQTT support: ON (libmosquitto ${MOSQUITTO_VERSION})")
else()
    message(STATUS "MQTT support: OFF")
endif()

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)
