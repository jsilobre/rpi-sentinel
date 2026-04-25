include(FetchContent)
find_package(PkgConfig QUIET)

if(NOT PkgConfig_FOUND)
    message(FATAL_ERROR "pkg-config is required (apt: pkg-config)")
endif()

# SQLite — required for sensor history persistence
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# MQTT support — auto-detected via pkg-config, overridable with -DENABLE_MQTT=ON/OFF
pkg_check_modules(MOSQUITTO QUIET libmosquitto)

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
