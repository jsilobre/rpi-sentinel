#include "WebState.hpp"

namespace rpi {

void WebState::push_reading(float temperature, std::chrono::system_clock::time_point timestamp)
{
    std::lock_guard lock(mutex_);
    current_temperature_ = temperature;
    has_reading_         = true;
    history_.push_back({temperature, timestamp});
    if (history_.size() > MAX_HISTORY) {
        history_.pop_front();
    }
}

void WebState::push_alert(const ThermalEvent& event)
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
        .current_temperature = current_temperature_,
        .has_reading         = has_reading_,
        .history             = {history_.begin(), history_.end()},
        .recent_events       = {events_.begin(), events_.end()},
    };
}

} // namespace rpi
