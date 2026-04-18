#pragma once

#include <chrono>
#include <string>

namespace rpi {

struct ThermalEvent {
    enum class Type {
        Reading,            // lecture périodique normale
        ThresholdExceeded,
        ThresholdRecovered,
    };

    Type                                     type;
    float                                    temperature;
    float                                    threshold;
    std::string                              sensor_id;
    std::chrono::system_clock::time_point    timestamp = std::chrono::system_clock::now();
};

} // namespace rpi
