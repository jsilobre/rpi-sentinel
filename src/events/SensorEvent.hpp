#pragma once

#include <chrono>
#include <string>

namespace rpi {

struct SensorEvent {
    enum class Type {
        Reading,            // periodic sensor reading (for dashboard)
        ThresholdExceeded,
        ThresholdRecovered,
    };

    Type        type;
    std::string metric;     // e.g. "temperature", "pressure", "motion"
    float       value;
    float       threshold;
    std::string sensor_id;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

} // namespace rpi
