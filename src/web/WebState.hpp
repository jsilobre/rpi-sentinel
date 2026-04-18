#pragma once

#include "../events/SensorEvent.hpp"
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace rpi {

struct HistoryPoint {
    std::string                           sensor_id;
    std::string                           metric;
    float                                 value;
    std::chrono::system_clock::time_point timestamp;
};

class WebState {
public:
    static constexpr size_t MAX_HISTORY = 120;
    static constexpr size_t MAX_EVENTS  = 30;

    void push_reading(const SensorEvent& event);
    void push_alert(const SensorEvent& event);

    struct Snapshot {
        bool                       has_reading       = false;
        float                      current_value     = 0.0f;
        std::string                current_sensor_id;
        std::string                current_metric;
        std::vector<HistoryPoint>  history;
        std::vector<SensorEvent>   recent_events;
    };

    Snapshot snapshot() const;

private:
    mutable std::mutex        mutex_;
    std::deque<HistoryPoint>  history_;
    std::deque<SensorEvent>   events_;
    float                     current_value_     = 0.0f;
    std::string               current_sensor_id_;
    std::string               current_metric_;
    bool                      has_reading_       = false;
};

} // namespace rpi
