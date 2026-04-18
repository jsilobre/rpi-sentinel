add_library(web STATIC
    WebState.cpp
    WebAlert.cpp
    HttpServer.cpp
)

target_include_directories(web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# httplib inclus en SYSTEM pour ne pas propager ses warnings avec -Werror
target_link_libraries(web PUBLIC alerts)
target_link_libraries(web PRIVATE nlohmann_json::nlohmann_json)

# Récupère les includes de httplib et les marque SYSTEM
get_target_property(_httplib_inc httplib::httplib INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(web SYSTEM PRIVATE ${_httplib_inc})

apply_compiler_options(web)
