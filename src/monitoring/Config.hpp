#pragma once

#include <chrono>
#include <string>

namespace rpi {

enum class SensorType {
    DS18B20,
    Simulated,
};

struct Config {
    SensorType             sensor_type     = SensorType::Simulated;
    std::string            device_path     = "/sys/bus/w1/devices/28-000000000000/temperature";
    float                  threshold_warn  = 60.0f;
    float                  threshold_crit  = 80.0f;
    float                  hysteresis      = 2.0f;
    std::chrono::milliseconds poll_interval {5000};
    bool                       web_enabled  = true;
    uint16_t                   web_port     = 8080;
};

} // namespace rpi
