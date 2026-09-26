#include "ThresholdMonitor.hpp"

#include <print>
#include <vector>

namespace rpi {

ThresholdMonitor::ThresholdMonitor(ISensorReader& sensor, EventBus& bus, MonitorConfig config)
    : sensor_(sensor)
    , bus_(bus)
    , config_(config)
    , threshold_warn_(config.threshold_warn)
    , threshold_crit_(config.threshold_crit)
    , poll_interval_ms_(config.poll_interval.count())
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

void ThresholdMonitor::set_poll_interval(std::chrono::milliseconds interval)
{
    poll_interval_ms_.store(interval.count());
    // Cut the current (possibly long) sleep short so the new interval applies now.
    force_poll();
}

void ThresholdMonitor::force_poll()
{
    // Set the flag under the sleep mutex so it can't be set between the
    // waiter's predicate check and its reset (which would lose the wakeup).
    {
        std::lock_guard lock(sleep_mtx_);
        force_poll_flag_.store(true);
    }
    sleep_cv_.notify_one();
}

void ThresholdMonitor::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        auto result = sensor_.read();

        if (!result) {
            std::println("[ThresholdMonitor] read error: {}", static_cast<int>(result.error()));
        } else {
            const float temp      = result->value;
            const float thr_warn  = threshold_warn_.load();
            const float thr_crit  = threshold_crit_.load();

            // Evaluate thresholds first so the Reading event can carry the
            // resulting level; transitions are dispatched after the reading.
            std::vector<SensorEvent> transitions;
            auto transition = [&](SensorEvent::Type type, SensorEvent::Level level,
                                  float threshold) {
                transitions.push_back(SensorEvent{
                    .type      = type,
                    .metric    = result->metric,
                    .value     = temp,
                    .threshold = threshold,
                    .sensor_id = result->sensor_id,
                    .level     = level,
                });
            };

            // Critical threshold (highest priority)
            if (!crit_active_ && temp >= thr_crit) {
                crit_active_ = true;
                transition(SensorEvent::Type::ThresholdExceeded, SensorEvent::Level::Crit, thr_crit);
            } else if (crit_active_ && temp < thr_crit - config_.hysteresis) {
                crit_active_ = false;
                transition(SensorEvent::Type::ThresholdRecovered, SensorEvent::Level::Crit, thr_crit);
            }

            // Warning threshold
            if (!warn_active_ && temp >= thr_warn && !crit_active_) {
                warn_active_ = true;
                transition(SensorEvent::Type::ThresholdExceeded, SensorEvent::Level::Warn, thr_warn);
            } else if (warn_active_ && temp < thr_warn - config_.hysteresis) {
                warn_active_ = false;
                transition(SensorEvent::Type::ThresholdRecovered, SensorEvent::Level::Warn, thr_warn);
            }

            bus_.dispatch(SensorEvent{
                .type      = SensorEvent::Type::Reading,
                .metric    = result->metric,
                .value     = temp,
                .threshold = 0.0f,
                .sensor_id = result->sensor_id,
                .level     = crit_active_ ? SensorEvent::Level::Crit
                           : warn_active_ ? SensorEvent::Level::Warn
                                          : SensorEvent::Level::Ok,
            });
            for (const auto& ev : transitions) bus_.dispatch(ev);
        }

        std::unique_lock lock(sleep_mtx_);
        sleep_cv_.wait_for(lock, stop, get_poll_interval(),
            [this] { return force_poll_flag_.load(); });
        force_poll_flag_.store(false);
    }
}

} // namespace rpi
