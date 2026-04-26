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

# OpenTelemetry C++ SDK — opt-in because the SDK build is expensive
# (~5–15 min depending on machine). Default OFF so CI and local iteration
# stay fast. Enable on actual deployments with -DENABLE_OTLP=ON.
option(ENABLE_OTLP "Enable OTLP/HTTP export to Grafana Cloud" OFF)

if(ENABLE_OTLP)
    find_package(Protobuf REQUIRED)
    find_package(CURL REQUIRED)

    set(WITH_OTLP_HTTP            ON  CACHE BOOL "" FORCE)
    set(WITH_OTLP_GRPC            OFF CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES             OFF CACHE BOOL "" FORCE)
    set(WITH_FUNC_TESTS           OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK            OFF CACHE BOOL "" FORCE)
    set(OPENTELEMETRY_INSTALL     OFF CACHE BOOL "" FORCE)
    set(WITH_ABSEIL               OFF CACHE BOOL "" FORCE)
    set(BUILD_W3CTRACECONTEXT_TEST OFF CACHE BOOL "" FORCE)

    # opentelemetry-cpp keys its own test build on BUILD_TESTING. We must
    # disable it during their FetchContent (otherwise they try find_package(GTest))
    # then restore the user's value so our tests/ subdir still builds.
    set(_RPI_SAVED_BUILD_TESTING ${BUILD_TESTING})
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        opentelemetry-cpp
        GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
        GIT_TAG        v1.18.0
    )
    FetchContent_MakeAvailable(opentelemetry-cpp)

    set(BUILD_TESTING ${_RPI_SAVED_BUILD_TESTING} CACHE BOOL "" FORCE)
    message(STATUS "OTLP support: ON (opentelemetry-cpp v1.18.0, HTTP/Protobuf only)")
else()
    message(STATUS "OTLP support: OFF")
endif()
