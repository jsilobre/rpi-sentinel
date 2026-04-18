#include "ThresholdMonitor.hpp"

#include <print>

namespace rpi {

ThresholdMonitor::ThresholdMonitor(ISensorReader& sensor, EventBus& bus, Config config)
    : sensor_(sensor)
    , bus_(bus)
    , config_(std::move(config))
{}

ThresholdMonitor::~ThresholdMonitor()
{
    stop();
}

void ThresholdMonitor::start()
{
    thread_ = std::jthread([this](std::stop_token st) { run(std::move(st)); });
}

void ThresholdMonitor::stop()
{
    thread_.request_stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ThresholdMonitor::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        auto result = sensor_.read();

        if (!result) {
            std::println("[ThresholdMonitor] read error: {}", static_cast<int>(result.error()));
        } else {
            float temp = result->temperature_celsius;

            // Periodic reading — consumed by the web dashboard
            bus_.dispatch(ThermalEvent{
                .type        = ThermalEvent::Type::Reading,
                .temperature = temp,
                .threshold   = 0.0f,
                .sensor_id   = result->sensor_id,
            });

            // Critical threshold (highest priority)
            if (!crit_active_ && temp >= config_.threshold_crit) {
                crit_active_ = true;
                bus_.dispatch(ThermalEvent{
                    .type        = ThermalEvent::Type::ThresholdExceeded,
                    .temperature = temp,
                    .threshold   = config_.threshold_crit,
                    .sensor_id   = result->sensor_id,
                });
            } else if (crit_active_ && temp < config_.threshold_crit - config_.hysteresis) {
                crit_active_ = false;
                bus_.dispatch(ThermalEvent{
                    .type        = ThermalEvent::Type::ThresholdRecovered,
                    .temperature = temp,
                    .threshold   = config_.threshold_crit,
                    .sensor_id   = result->sensor_id,
                });
            }

            // Warning threshold
            if (!warn_active_ && temp >= config_.threshold_warn && !crit_active_) {
                warn_active_ = true;
                bus_.dispatch(ThermalEvent{
                    .type        = ThermalEvent::Type::ThresholdExceeded,
                    .temperature = temp,
                    .threshold   = config_.threshold_warn,
                    .sensor_id   = result->sensor_id,
                });
            } else if (warn_active_ && temp < config_.threshold_warn - config_.hysteresis) {
                warn_active_ = false;
                bus_.dispatch(ThermalEvent{
                    .type        = ThermalEvent::Type::ThresholdRecovered,
                    .temperature = temp,
                    .threshold   = config_.threshold_warn,
                    .sensor_id   = result->sensor_id,
                });
            }
        }

        std::this_thread::sleep_for(config_.poll_interval);
    }
}

} // namespace rpi
