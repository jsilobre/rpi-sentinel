#include "HaDiscovery.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <string_view>

namespace rpi {

namespace {

struct MetricInfo {
    std::string_view device_class;   // empty = generic numeric sensor
    std::string_view unit;
};

// HA device classes/units for the metrics the daemon knows. SGP30 TVOC is
// reported in ppb (SGP30Reader scales the IIO fraction by 1e9).
MetricInfo metric_info(std::string_view metric)
{
    if (metric == "temperature")                return {"temperature", "°C"};
    if (metric == "humidity")                   return {"humidity", "%"};
    if (metric == "pressure")                   return {"atmospheric_pressure", "hPa"};
    if (metric == "eco2" || metric == "co2")    return {"carbon_dioxide", "ppm"};
    if (metric == "tvoc")                       return {"volatile_organic_compounds_parts", "ppb"};
    return {};
}

// Discovery topics only allow [a-zA-Z0-9_-] in the node and object ids.
std::string object_id(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    return out;
}

} // namespace

std::vector<DiscoveryMessage> build_ha_discovery(const MqttConfig&                mqtt,
                                                 const std::vector<SensorConfig>& sensors)
{
    const std::string& prefix = mqtt.topic_prefix;
    const std::string& dp     = mqtt.homeassistant.discovery_prefix;
    const std::string  node   = object_id(prefix + "-sentinel");

    const nlohmann::json device = {
        {"identifiers",  {node}},
        {"name",         prefix == "rpi" ? std::string{"RPi Sentinel"}
                                         : std::format("RPi Sentinel ({})", prefix)},
        {"manufacturer", "rpi-sentinel"},
        {"model",        "Raspberry Pi"},
    };

    // Shared by every entity: device grouping + availability from the
    // daemon's retained status (LWT publishes {"status":"offline"}).
    auto base = [&](std::string name, std::string unique_id) {
        return nlohmann::json{
            {"name",                  std::move(name)},
            {"unique_id",             std::move(unique_id)},
            {"device",                device},
            {"availability_topic",    prefix + "/status"},
            {"availability_template", "{{ value_json.status }}"},
            {"payload_available",     "online"},
            {"payload_not_available", "offline"},
        };
    };
    auto topic = [&](std::string_view component, std::string_view oid) {
        return std::format("{}/{}/{}/{}/config", dp, component, node, oid);
    };

    std::vector<DiscoveryMessage> out;
    for (const auto& sc : sensors) {
        const std::string oid         = object_id(sc.id);
        const std::string state_topic = std::format("{}/{}/reading", prefix, sc.id);
        const bool        is_motion   = sc.metric == "motion";

        auto value = base(sc.id, std::format("{}_{}", node, oid));
        value["state_topic"] = state_topic;
        if (is_motion) {
            value["device_class"]   = "motion";
            value["value_template"] =
                "{{ 'ON' if value_json.value | float(0) >= 0.5 else 'OFF' }}";
        } else {
            value["value_template"]              = "{{ value_json.value }}";
            value["state_class"]                 = "measurement";
            value["suggested_display_precision"] = 1;
            if (const auto mi = metric_info(sc.metric); !mi.device_class.empty()) {
                value["device_class"]        = mi.device_class;
                value["unit_of_measurement"] = mi.unit;
            }
        }
        out.push_back({topic(is_motion ? "binary_sensor" : "sensor", oid), value.dump()});
        // Tombstone the other component, in case this sensor's metric changed
        // to/from "motion" since the last run.
        out.push_back({topic(is_motion ? "sensor" : "binary_sensor", oid), ""});

        auto level = base(sc.id + " level", std::format("{}_{}_level", node, oid));
        level["state_topic"]    = state_topic;
        level["value_template"] = "{{ value_json.level }}";
        level["device_class"]   = "enum";
        level["options"]        = {"ok", "warn", "crit"};
        level["icon"]           = "mdi:alert-circle-outline";
        out.push_back({topic("sensor", oid + "_level"), level.dump()});
    }

    // <prefix>/cmd/clear is deliberately not exposed: it wipes the history
    // database, too easy to hit by accident from an HA dashboard.
    auto refresh = base("Refresh readings", node + "_refresh");
    refresh["command_topic"] = prefix + "/cmd/refresh";
    refresh["payload_press"] = "{}";
    refresh["icon"]          = "mdi:refresh";
    out.push_back({topic("button", "refresh"), refresh.dump()});

    return out;
}

} // namespace rpi
