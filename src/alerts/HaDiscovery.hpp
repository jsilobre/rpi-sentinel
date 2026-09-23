#pragma once

#include "../monitoring/Config.hpp"

#include <string>
#include <vector>

namespace rpi {

struct DiscoveryMessage {
    std::string topic;
    std::string payload;   // empty = delete a stale entity (retained tombstone)
};

// Home Assistant MQTT discovery configs for the daemon's existing topics.
// Published retained on every connect, so HA creates one "RPi Sentinel"
// device whose entities read <prefix>/<id>/reading and follow <prefix>/status
// for availability; the MQTT contract itself is unchanged.
//
// Per sensor: a measurement entity (sensor, or binary_sensor for "motion")
// and a "<id> level" enum entity (ok/warn/crit). Plus a "Refresh readings"
// button on <prefix>/cmd/refresh.
std::vector<DiscoveryMessage> build_ha_discovery(const MqttConfig&                mqtt,
                                                 const std::vector<SensorConfig>& sensors);

} // namespace rpi
