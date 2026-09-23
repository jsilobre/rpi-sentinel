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

    // Alert level the monitor holds for this sensor after evaluating the
    // reading (hysteresis applied). Only meaningful on Reading events: it lets
    // late subscribers learn the current state without having seen the
    // one-shot ThresholdExceeded/Recovered transitions.
    enum class Level {
        Ok,
        Warn,
        Crit,
    };

    Type        type;
    std::string metric;     // e.g. "temperature", "pressure", "motion"
    float       value;
    float       threshold;
    std::string sensor_id;
    Level       level = Level::Ok;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

} // namespace rpi
