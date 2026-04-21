#include "LogAlert.hpp"

#include <chrono>
#include <format>
#include <print>

namespace rpi {

void LogAlert::on_event(const SensorEvent& event)
{
    if (event.type == SensorEvent::Type::Reading) return;

    auto time_t = std::chrono::system_clock::to_time_t(event.timestamp);
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));

    std::string_view label = (event.type == SensorEvent::Type::ThresholdExceeded)
        ? "EXCEEDED"
        : "RECOVERED";

    std::println("[{}] [{}] sensor={} {}={:.1f} threshold={:.1f}",
        time_buf, label, event.sensor_id, event.metric, event.value, event.threshold);
}

} // namespace rpi
