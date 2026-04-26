#include "ThresholdMonitor.hpp"

#include <condition_variable>
#include <mutex>
#include <print>

namespace rpi {

ThresholdMonitor::ThresholdMonitor(ISensorReader& sensor, EventBus& bus, MonitorConfig config)
    : sensor_(sensor)
    , bus_(bus)
    , config_(config)
    , threshold_warn_(config.threshold_warn)
    , threshold_crit_(config.threshold_crit)
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

void ThresholdMonitor::update_thresholds(float warn, float crit)
{
    threshold_warn_.store(warn);
    threshold_crit_.store(crit);
}

void ThresholdMonitor::run(std::stop_token stop)
{
    std::condition_variable_any cv;
    std::mutex sleep_mtx;

    while (!stop.stop_requested()) {
        auto result = sensor_.read();

        if (!result) {
            std::println("[ThresholdMonitor] read error: {}", static_cast<int>(result.error()));
        } else {
            const float temp      = result->value;
            const float thr_warn  = threshold_warn_.load();
            const float thr_crit  = threshold_crit_.load();

            bus_.dispatch(SensorEvent{
                .type      = SensorEvent::Type::Reading,
                .metric    = result->metric,
                .value     = temp,
                .threshold = 0.0f,
                .sensor_id = result->sensor_id,
            });

            // Critical threshold (highest priority)
            if (!crit_active_ && temp >= thr_crit) {
                crit_active_ = true;
                bus_.dispatch(SensorEvent{
                    .type      = SensorEvent::Type::ThresholdExceeded,
                    .metric    = result->metric,
                    .value     = temp,
                    .threshold = thr_crit,
                    .sensor_id = result->sensor_id,
                });
            } else if (crit_active_ && temp < thr_crit - config_.hysteresis) {
                crit_active_ = false;
                bus_.dispatch(SensorEvent{
                    .type      = SensorEvent::Type::ThresholdRecovered,
                    .metric    = result->metric,
                    .value     = temp,
                    .threshold = thr_crit,
                    .sensor_id = result->sensor_id,
                });
            }

            // Warning threshold
            if (!warn_active_ && temp >= thr_warn && !crit_active_) {
                warn_active_ = true;
                bus_.dispatch(SensorEvent{
                    .type      = SensorEvent::Type::ThresholdExceeded,
                    .metric    = result->metric,
                    .value     = temp,
                    .threshold = thr_warn,
                    .sensor_id = result->sensor_id,
                });
            } else if (warn_active_ && temp < thr_warn - config_.hysteresis) {
                warn_active_ = false;
                bus_.dispatch(SensorEvent{
                    .type      = SensorEvent::Type::ThresholdRecovered,
                    .metric    = result->metric,
                    .value     = temp,
                    .threshold = thr_warn,
                    .sensor_id = result->sensor_id,
                });
            }
        }

        std::unique_lock lock(sleep_mtx);
        cv.wait_for(lock, stop, config_.poll_interval, [] { return false; });
    }
}

} // namespace rpi
