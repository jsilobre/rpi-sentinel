#include "MonitoringHub.hpp"

#include "../sensors/DS18B20Reader.hpp"
#include "../sensors/SimulatedSensor.hpp"

#include <print>

namespace rpi {

static auto make_sensor(const SensorConfig& sc) -> std::unique_ptr<ISensorReader>
{
    switch (sc.type) {
        case SensorType::DS18B20:
            return std::make_unique<DS18B20Reader>(sc.device_path, sc.id, sc.metric);
        case SensorType::Simulated:
            return std::make_unique<SimulatedSensor>(sc.id, sc.metric);
    }
    return std::make_unique<SimulatedSensor>(sc.id, sc.metric);
}

MonitoringHub::MonitoringHub(EventBus& bus, const Config& config)
{
    for (const auto& sc : config.sensors) {
        sensors_.push_back(make_sensor(sc));
        monitors_.push_back(std::make_unique<ThresholdMonitor>(
            *sensors_.back(),
            bus,
            MonitorConfig{
                .threshold_warn = sc.threshold_warn,
                .threshold_crit = sc.threshold_crit,
                .hysteresis     = config.hysteresis,
                .poll_interval  = config.poll_interval,
            }
        ));
    }
    std::println("[MonitoringHub] {} monitor(s) configured.", monitors_.size());
}

void MonitoringHub::start()
{
    for (auto& m : monitors_) m->start();
}

void MonitoringHub::stop()
{
    for (auto& m : monitors_) m->stop();
}

} // namespace rpi
