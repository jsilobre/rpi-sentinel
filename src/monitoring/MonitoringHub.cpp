#include "MonitoringHub.hpp"

#include "../sensors/DS18B20Reader.hpp"
#include "../sensors/SimulatedSensor.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <print>

namespace rpi {

static auto make_sensor(const SensorConfig& sc) -> std::unique_ptr<ISensorReader>
{
    switch (sc.type) {
        case SensorType::DS18B20:
            return std::make_unique<DS18B20Reader>(sc.device_path, sc.id, sc.metric);
        case SensorType::Simulated:
            return std::make_unique<SimulatedSensor>(sc.id, sc.metric,
                SimulatedSensor::make_for_metric(sc.metric));
    }
    return std::make_unique<SimulatedSensor>(sc.id, sc.metric);
}

MonitoringHub::MonitoringHub(EventBus& bus, Config config)
    : config_(std::move(config))
{
    for (const auto& sc : config_.sensors) {
        sensors_.push_back(make_sensor(sc));
        monitors_.push_back(std::make_unique<ThresholdMonitor>(
            *sensors_.back(),
            bus,
            MonitorConfig{
                .threshold_warn = sc.threshold_warn,
                .threshold_crit = sc.threshold_crit,
                .hysteresis     = config_.hysteresis,
                .poll_interval  = config_.poll_interval,
            }
        ));
        monitor_map_[sc.id] = monitors_.back().get();
    }
    std::println("[MonitoringHub] {} monitor(s) configured.", monitors_.size());
}

void MonitoringHub::update_thresholds(const std::string& sensor_id, float warn, float crit)
{
    auto it = monitor_map_.find(sensor_id);
    if (it == monitor_map_.end()) return;
    it->second->update_thresholds(warn, crit);

    std::lock_guard lock(config_mutex_);
    for (auto& sc : config_.sensors) {
        if (sc.id == sensor_id) {
            sc.threshold_warn = warn;
            sc.threshold_crit = crit;
            break;
        }
    }
}

Config MonitoringHub::get_config_snapshot() const
{
    std::lock_guard lock(config_mutex_);
    return config_;
}

std::string MonitoringHub::build_config_json() const
{
    std::lock_guard lock(config_mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& sc : config_.sensors) {
        arr.push_back({
            {"id",             sc.id},
            {"metric",         sc.metric},
            {"threshold_warn", sc.threshold_warn},
            {"threshold_crit", sc.threshold_crit},
        });
    }
    return nlohmann::json{{"sensors", arr}}.dump();
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
