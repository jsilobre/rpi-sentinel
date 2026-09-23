#include <gtest/gtest.h>

#include "../src/alerts/HaDiscovery.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

using namespace rpi;
using nlohmann::json;

namespace {

MqttConfig make_mqtt()
{
    MqttConfig cfg;
    cfg.topic_prefix                   = "rpi";
    cfg.homeassistant.enabled          = true;
    cfg.homeassistant.discovery_prefix = "homeassistant";
    return cfg;
}

SensorConfig sensor(std::string id, std::string metric)
{
    SensorConfig sc;
    sc.id     = std::move(id);
    sc.metric = std::move(metric);
    return sc;
}

const DiscoveryMessage* find(const std::vector<DiscoveryMessage>& msgs, const std::string& topic)
{
    auto it = std::find_if(msgs.begin(), msgs.end(),
                           [&](const DiscoveryMessage& m) { return m.topic == topic; });
    return it == msgs.end() ? nullptr : &*it;
}

} // namespace

TEST(HaDiscovery, TemperatureSensorMapsDeviceClassAndStateTopic)
{
    const auto msgs = build_ha_discovery(make_mqtt(), {sensor("bme280-temp", "temperature")});

    const auto* m = find(msgs, "homeassistant/sensor/rpi-sentinel/bme280-temp/config");
    ASSERT_NE(m, nullptr);
    const auto j = json::parse(m->payload);
    EXPECT_EQ(j["state_topic"], "rpi/bme280-temp/reading");
    EXPECT_EQ(j["value_template"], "{{ value_json.value }}");
    EXPECT_EQ(j["device_class"], "temperature");
    EXPECT_EQ(j["unit_of_measurement"], "°C");
    EXPECT_EQ(j["state_class"], "measurement");
    EXPECT_EQ(j["unique_id"], "rpi-sentinel_bme280-temp");
    EXPECT_EQ(j["availability_topic"], "rpi/status");
    EXPECT_EQ(j["device"]["identifiers"][0], "rpi-sentinel");
    EXPECT_EQ(j["device"]["name"], "RPi Sentinel");
}

TEST(HaDiscovery, MetricUnits)
{
    const auto msgs = build_ha_discovery(make_mqtt(), {
        sensor("h", "humidity"), sensor("p", "pressure"),
        sensor("c", "eco2"),     sensor("v", "tvoc"),
    });
    auto unit = [&](const std::string& id) {
        const auto* m = find(msgs, "homeassistant/sensor/rpi-sentinel/" + id + "/config");
        return m ? json::parse(m->payload)["unit_of_measurement"].get<std::string>() : "";
    };
    EXPECT_EQ(unit("h"), "%");
    EXPECT_EQ(unit("p"), "hPa");
    EXPECT_EQ(unit("c"), "ppm");
    EXPECT_EQ(unit("v"), "ppb");
}

TEST(HaDiscovery, UnknownMetricHasNoDeviceClassOrUnit)
{
    const auto msgs = build_ha_discovery(make_mqtt(), {sensor("x", "lux")});
    const auto* m = find(msgs, "homeassistant/sensor/rpi-sentinel/x/config");
    ASSERT_NE(m, nullptr);
    const auto j = json::parse(m->payload);
    EXPECT_FALSE(j.contains("device_class"));
    EXPECT_FALSE(j.contains("unit_of_measurement"));
    EXPECT_EQ(j["state_class"], "measurement");
}

TEST(HaDiscovery, MotionIsBinarySensorAndTombstonesSensorComponent)
{
    const auto msgs = build_ha_discovery(make_mqtt(), {sensor("pir", "motion")});

    const auto* bin = find(msgs, "homeassistant/binary_sensor/rpi-sentinel/pir/config");
    ASSERT_NE(bin, nullptr);
    const auto j = json::parse(bin->payload);
    EXPECT_EQ(j["device_class"], "motion");
    EXPECT_FALSE(j.contains("state_class"));

    const auto* stale = find(msgs, "homeassistant/sensor/rpi-sentinel/pir/config");
    ASSERT_NE(stale, nullptr);
    EXPECT_TRUE(stale->payload.empty());
}

TEST(HaDiscovery, LevelEntityPerSensor)
{
    const auto msgs = build_ha_discovery(make_mqtt(), {sensor("t1", "temperature")});
    const auto* m = find(msgs, "homeassistant/sensor/rpi-sentinel/t1_level/config");
    ASSERT_NE(m, nullptr);
    const auto j = json::parse(m->payload);
    EXPECT_EQ(j["value_template"], "{{ value_json.level }}");
    EXPECT_EQ(j["device_class"], "enum");
    EXPECT_EQ(j["options"], json({"ok", "warn", "crit"}));
    EXPECT_EQ(j["unique_id"], "rpi-sentinel_t1_level");
}

TEST(HaDiscovery, RefreshButtonButNoClearButton)
{
    const auto msgs = build_ha_discovery(make_mqtt(), {sensor("t1", "temperature")});
    const auto* m = find(msgs, "homeassistant/button/rpi-sentinel/refresh/config");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(json::parse(m->payload)["command_topic"], "rpi/cmd/refresh");

    for (const auto& msg : msgs)
        EXPECT_EQ(msg.payload.find("cmd/clear"), std::string::npos) << msg.topic;
}

TEST(HaDiscovery, CustomPrefixesAndSanitizedIds)
{
    auto cfg = make_mqtt();
    cfg.topic_prefix                   = "garage";
    cfg.homeassistant.discovery_prefix = "ha";
    const auto msgs = build_ha_discovery(cfg, {sensor("temp.1 a", "temperature")});

    const auto* m = find(msgs, "ha/sensor/garage-sentinel/temp_1_a/config");
    ASSERT_NE(m, nullptr);
    const auto j = json::parse(m->payload);
    // The state topic keeps the raw id: it is what the daemon publishes on.
    EXPECT_EQ(j["state_topic"], "garage/temp.1 a/reading");
    EXPECT_EQ(j["availability_topic"], "garage/status");
    EXPECT_EQ(j["device"]["name"], "RPi Sentinel (garage)");
}
