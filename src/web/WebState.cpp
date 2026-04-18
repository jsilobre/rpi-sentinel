#include "WebState.hpp"

namespace rpi {

void WebState::push_reading(const SensorEvent& event)
{
    std::lock_guard lock(mutex_);
    current_value_     = event.value;
    current_sensor_id_ = event.sensor_id;
    current_metric_    = event.metric;
    has_reading_       = true;
    history_.push_back({event.sensor_id, event.metric, event.value, event.timestamp});
    if (history_.size() > MAX_HISTORY) {
        history_.pop_front();
    }
}

void WebState::push_alert(const SensorEvent& event)
{
    std::lock_guard lock(mutex_);
    events_.push_front(event);
    if (events_.size() > MAX_EVENTS) {
        events_.pop_back();
    }
}

WebState::Snapshot WebState::snapshot() const
{
    std::lock_guard lock(mutex_);
    return Snapshot{
        .has_reading       = has_reading_,
        .current_value     = current_value_,
        .current_sensor_id = current_sensor_id_,
        .current_metric    = current_metric_,
        .history           = {history_.begin(), history_.end()},
        .recent_events     = {events_.begin(), events_.end()},
    };
}

} // namespace rpi
