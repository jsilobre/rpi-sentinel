#include "WebState.hpp"

namespace rpi {

void WebState::push_reading(const SensorEvent& event)
{
    std::lock_guard lock(mutex_);
    auto& state = sensor_states_[event.sensor_id];
    if (!state.has_reading) {
        state.id     = event.sensor_id;
        state.metric = event.metric;
        sensor_order_.push_back(event.sensor_id);
    }
    state.has_reading   = true;
    state.current_value = event.value;
    state.history.push_back({event.value, event.timestamp});
    if (state.history.size() > MAX_HISTORY) {
        state.history.pop_front();
    }
}

void WebState::push_alert(const SensorEvent& event)
{
    std::lock_guard lock(mutex_);
    auto it = sensor_states_.find(event.sensor_id);
    if (it != sensor_states_.end()) {
        it->second.status = (event.type == SensorEvent::Type::ThresholdExceeded)
            ? "alert" : "ok";
    }
    events_.push_front(event);
    if (events_.size() > MAX_EVENTS) {
        events_.pop_back();
    }
}

WebState::Snapshot WebState::snapshot() const
{
    std::lock_guard lock(mutex_);
    Snapshot snap;
    snap.sensors.reserve(sensor_order_.size());
    for (const auto& id : sensor_order_) {
        const auto& s = sensor_states_.at(id);
        snap.sensors.push_back({
            .id            = s.id,
            .metric        = s.metric,
            .has_reading   = s.has_reading,
            .current_value = s.current_value,
            .status        = s.status,
            .history       = {s.history.begin(), s.history.end()},
        });
    }
    snap.recent_events = {events_.begin(), events_.end()};
    return snap;
}

} // namespace rpi
