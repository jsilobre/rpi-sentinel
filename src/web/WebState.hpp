#pragma once

#include "../events/SensorEvent.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rpi {

struct HistoryPoint {
    float                                 value;
    std::chrono::system_clock::time_point timestamp;
};

class WebState {
public:
    static constexpr size_t MAX_HISTORY = 120;
    static constexpr size_t MAX_EVENTS  = 30;

    void push_reading(const SensorEvent& event);
    void push_alert(const SensorEvent& event);

    struct SensorSnapshot {
        std::string               id;
        std::string               metric;
        bool                      has_reading   = false;
        float                     current_value = 0.0f;
        std::string               status        = "ok";
        std::vector<HistoryPoint> history;
    };

    struct Snapshot {
        std::vector<SensorSnapshot> sensors;
        std::vector<SensorEvent>    recent_events;
    };

    Snapshot snapshot() const;

private:
    struct SensorState {
        std::string              id;
        std::string              metric;
        bool                     has_reading   = false;
        float                    current_value = 0.0f;
        std::string              status        = "ok";
        std::deque<HistoryPoint> history;
    };

    mutable std::mutex                           mutex_;
    std::vector<std::string>                     sensor_order_;  // preserves insertion order
    std::unordered_map<std::string, SensorState> sensor_states_;
    std::deque<SensorEvent>                      events_;
};

} // namespace rpi
