add_library(otel STATIC OtlpExporter.cpp)

target_include_directories(otel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(otel PUBLIC alerts monitoring)
apply_compiler_options(otel)

if(ENABLE_OTLP)
    target_compile_definitions(otel PUBLIC ENABLE_OTLP)
    target_link_libraries(otel PUBLIC
        opentelemetry_api
        opentelemetry_resources
        opentelemetry_common
        opentelemetry_metrics
        opentelemetry_logs
        opentelemetry_otlp_recordable
        opentelemetry_exporter_otlp_http_metric
        opentelemetry_exporter_otlp_http_log
        opentelemetry_http_client_curl
    )
    # Re-add the SDK headers as SYSTEM to suppress its internal warnings
    # (otherwise the project's -Wshadow -Werror trips on third-party headers).
    target_include_directories(otel SYSTEM PRIVATE
        ${opentelemetry-cpp_SOURCE_DIR}/api/include
        ${opentelemetry-cpp_SOURCE_DIR}/sdk/include
        ${opentelemetry-cpp_SOURCE_DIR}/exporters/otlp/include
        ${opentelemetry-cpp_SOURCE_DIR}/ext/include
    )
endif()
