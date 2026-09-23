#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace rpi {

struct SensorEvent {
    enum class Type {
        Reading,            // periodic sensor reading (for dashboard)
        ThresholdExceeded,
        ThresholdRecovered,
    };

    // Reading: alert level the monitor holds for this sensor after evaluating
    // the reading (hysteresis applied), so late subscribers learn the current
    // state without having seen the one-shot transitions.
    // ThresholdExceeded/Recovered: which threshold crossed (Warn or Crit).
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

// Wire/storage spellings shared by the MQTT payloads, the SQLite alert log
// and the console log.
constexpr std::string_view to_string(SensorEvent::Level level)
{
    switch (level) {
        case SensorEvent::Level::Crit: return "crit";
        case SensorEvent::Level::Warn: return "warn";
        case SensorEvent::Level::Ok:   break;
    }
    return "ok";
}

// Alert transitions only; Reading has no alert type.
constexpr std::string_view alert_type_string(SensorEvent::Type type)
{
    return type == SensorEvent::Type::ThresholdExceeded ? "EXCEEDED" : "RECOVERED";
}

} // namespace rpi
