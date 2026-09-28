#pragma once

#include "Config.hpp"
#include "ThresholdMonitor.hpp"
#include "../events/EventBus.hpp"
#include "../sensors/ISensorReader.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rpi {

class MonitoringHub {
public:
    MonitoringHub(EventBus& bus, Config config);
    void start();
    void stop();

    void        update_thresholds(const std::string& sensor_id, float warn, float crit);
    void        update_poll_interval(std::chrono::milliseconds interval);
    // Returns false if no sensor has this id.
    bool        set_sensor_enabled(const std::string& sensor_id, bool enabled);
    void        force_poll_all();
    Config      get_config_snapshot() const;
    std::string build_config_json() const;

private:
    mutable std::mutex                                config_mutex_;
    Config                                            config_;
    std::vector<std::unique_ptr<ISensorReader>>       sensors_;
    std::vector<std::unique_ptr<ThresholdMonitor>>    monitors_;
    std::unordered_map<std::string, ThresholdMonitor*> monitor_map_;
};

} // namespace rpi
