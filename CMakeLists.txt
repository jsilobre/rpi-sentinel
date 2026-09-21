cmake_minimum_required(VERSION 3.28)
project(rpi-sentinel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

include(GNUInstallDirs)

set(RPI_SENTINEL_INSTALL_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config.example.json"
    CACHE FILEPATH
    "Config file installed as <prefix>/${CMAKE_INSTALL_SYSCONFDIR}/rpi-sentinel/config.json. Set to an empty string to skip installing any config.")

if(RPI_SENTINEL_INSTALL_CONFIG)
    install(FILES "${RPI_SENTINEL_INSTALL_CONFIG}"
            DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/rpi-sentinel"
            RENAME config.json)
endif()

option(RPI_SENTINEL_INSTALL_SYSTEMD_UNIT
    "Install the rpi-sentinel.service systemd unit." ON)
set(RPI_SENTINEL_SYSTEMD_UNITDIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
    CACHE PATH
    "Directory where rpi-sentinel.service is installed (override for distro packaging, e.g. /lib/systemd/system).")

if(RPI_SENTINEL_INSTALL_SYSTEMD_UNIT)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/rpi-sentinel.service.in"
        "${CMAKE_CURRENT_BINARY_DIR}/rpi-sentinel.service"
        @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rpi-sentinel.service"
            DESTINATION "${RPI_SENTINEL_SYSTEMD_UNITDIR}")
endif()

option(BUILD_TESTING "Build unit tests" ON)

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
