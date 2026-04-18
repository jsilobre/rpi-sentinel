#include "LogAlert.hpp"

#include <chrono>
#include <format>
#include <print>

namespace rpi {

void LogAlert::on_event(const ThermalEvent& event)
{
    auto time_t = std::chrono::system_clock::to_time_t(event.timestamp);
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));

    std::string_view label = (event.type == ThermalEvent::Type::ThresholdExceeded)
        ? "EXCEEDED"
        : "RECOVERED";

    std::println("[{}] [{}] sensor={} temp={:.1f}°C threshold={:.1f}°C",
        time_buf, label, event.sensor_id, event.temperature, event.threshold);
}

} // namespace rpi
