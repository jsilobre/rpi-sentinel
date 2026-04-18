#pragma once

#include "../events/ThermalEvent.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace rpi {

struct TemperatureReading {
    float                                  temperature;
    std::chrono::system_clock::time_point  timestamp;
};

class WebState {
public:
    static constexpr size_t MAX_HISTORY = 120;
    static constexpr size_t MAX_EVENTS  = 30;

    void push_reading(float temperature, std::chrono::system_clock::time_point timestamp);
    void push_alert(const ThermalEvent& event);

    struct Snapshot {
        float                         current_temperature = 0.0f;
        bool                          has_reading         = false;
        std::vector<TemperatureReading> history;
        std::vector<ThermalEvent>       recent_events;
    };

    Snapshot snapshot() const;

private:
    mutable std::mutex            mutex_;
    std::deque<TemperatureReading> history_;
    std::deque<ThermalEvent>       events_;
    float                          current_temperature_ = 0.0f;
    bool                           has_reading_         = false;
};

} // namespace rpi
