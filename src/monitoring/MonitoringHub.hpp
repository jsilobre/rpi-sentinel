#pragma once

#include "Config.hpp"
#include "ThresholdMonitor.hpp"
#include "../events/EventBus.hpp"
#include "../sensors/ISensorReader.hpp"
#include <memory>
#include <vector>

namespace rpi {

class MonitoringHub {
public:
    MonitoringHub(EventBus& bus, const Config& config);
    void start();
    void stop();

private:
    std::vector<std::unique_ptr<ISensorReader>>    sensors_;
    std::vector<std::unique_ptr<ThresholdMonitor>> monitors_;
};

} // namespace rpi
