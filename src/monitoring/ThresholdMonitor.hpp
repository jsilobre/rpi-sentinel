#pragma once

#include "../events/EventBus.hpp"
#include "../sensors/ISensorReader.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace rpi {

struct MonitorConfig {
    float                     threshold_warn;
    float                     threshold_crit;
    float                     hysteresis;
    std::chrono::milliseconds poll_interval;
};

class ThresholdMonitor {
public:
    ThresholdMonitor(ISensorReader& sensor, EventBus& bus, MonitorConfig config);
    ~ThresholdMonitor();

    void start();
    void stop();

    void  update_thresholds(float warn, float crit);
    float get_threshold_warn() const { return threshold_warn_.load(); }
    float get_threshold_crit() const { return threshold_crit_.load(); }

private:
    void run(std::stop_token stop);

    ISensorReader&      sensor_;
    EventBus&           bus_;
    MonitorConfig       config_;
    std::atomic<float>  threshold_warn_;
    std::atomic<float>  threshold_crit_;
    std::jthread        thread_;

    bool warn_active_ = false;
    bool crit_active_ = false;
};

} // namespace rpi
