#pragma once

#include "Config.hpp"
#include "../events/EventBus.hpp"
#include "../sensors/ISensorReader.hpp"
#include <thread>

namespace rpi {

class ThresholdMonitor {
public:
    ThresholdMonitor(ISensorReader& sensor, EventBus& bus, Config config);
    ~ThresholdMonitor();

    void start();
    void stop();

private:
    void run(std::stop_token stop);

    ISensorReader& sensor_;
    EventBus&      bus_;
    Config         config_;
    std::jthread   thread_;

    // Per-threshold state to manage hysteresis
    bool warn_active_ = false;
    bool crit_active_ = false;
};

} // namespace rpi
