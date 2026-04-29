#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace rpi {

enum class SensorType {
    DS18B20,
    Simulated,
    DHT11,
    CpuTemp,
};

struct SensorConfig {
    std::string id;
    SensorType  type           = SensorType::Simulated;
    std::string device_path;
    std::string metric         = "temperature";
    float       threshold_warn = 60.0f;
    float       threshold_crit = 80.0f;
};

struct MqttConfig {
    bool        enabled      = false;
    std::string broker_url;               // e.g. "ssl://xxx.hivemq.cloud:8883"
    std::string username;
    std::string password;
    std::string topic_prefix = "rpi";
};

struct HistoryConfig {
    bool        enabled               = true;
    std::string db_path               = "data/history.db";
    int         retention_days        = 7;
    int         max_points_per_sensor = 50000;
};

struct OtlpConfig {
    bool        enabled              = false;
    std::string endpoint;                            // e.g. https://otlp-gateway-prod-eu-west-2.grafana.net/otlp
    std::string auth_header;                         // literal "Basic <base64(...)>"; dev only
    std::string auth_header_env      = "GRAFANA_CLOUD_OTLP_AUTH"; // env var name (preferred over auth_header)
    std::string service_instance_id  = "rpi-sentinel-1";
    int         export_interval_ms   = 5000;
};

struct Config {
    std::vector<SensorConfig> sensors;
    float                     hysteresis    = 2.0f;
    std::chrono::milliseconds poll_interval {5000};
    bool                      web_enabled   = true;
    uint16_t                  web_port      = 8080;
    MqttConfig                mqtt;
    HistoryConfig             history;
    OtlpConfig                otlp;
};

} // namespace rpi
