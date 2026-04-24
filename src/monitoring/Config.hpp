#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace rpi {

enum class SensorType {
    DS18B20,
    Simulated,
    DHT11,
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

struct Config {
    std::vector<SensorConfig> sensors;
    float                     hysteresis    = 2.0f;
    std::chrono::milliseconds poll_interval {5000};
    bool                      web_enabled   = true;
    uint16_t                  web_port      = 8080;
    MqttConfig                mqtt;
};

} // namespace rpi
