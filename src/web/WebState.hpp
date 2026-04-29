#pragma once

#include "../events/SensorEvent.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
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

    // SSE: returns the current change-counter.
    uint64_t version() const noexcept;
    // SSE: blocks until version != last_version or timeout elapses.
    void wait_for_change(uint64_t last_version, std::chrono::milliseconds timeout) const;

    // Replaces (or initialises) a sensor's history with `points` (assumed ASC chronological).
    // Sets has_reading + current_value from the last point if any.
    void prime_history(const std::string& sensor_id, const std::string& metric,
                       std::vector<HistoryPoint> points);

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
    mutable std::condition_variable              cv_;
    std::atomic<uint64_t>                        version_{0};
    std::vector<std::string>                     sensor_order_;  // preserves insertion order
    std::unordered_map<std::string, SensorState> sensor_states_;
    std::deque<SensorEvent>                      events_;
};

} // namespace rpi
