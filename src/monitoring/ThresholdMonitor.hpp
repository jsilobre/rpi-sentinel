#pragma once

#include "../events/EventBus.hpp"
#include "../sensors/ISensorReader.hpp"
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

private:
    void run(std::stop_token stop);

    ISensorReader& sensor_;
    EventBus&      bus_;
    MonitorConfig  config_;
    std::jthread   thread_;

    // Per-threshold state to manage hysteresis
    bool warn_active_ = false;
    bool crit_active_ = false;
};

} // namespace rpi
