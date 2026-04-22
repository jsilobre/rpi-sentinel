#include <gtest/gtest.h>
#include "../src/monitoring/ConfigLoader.hpp"

#include <filesystem>
#include <fstream>

// Writes a temporary JSON file named after the current test and returns its path.
static std::filesystem::path write_tmp(const std::string& content)
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "unknown";
    auto path = std::filesystem::temp_directory_path() / ("rpi_test_" + name + ".json");
    std::ofstream(path) << content;
    return path;
}

TEST(ConfigLoader, LoadsValidConfig)
{
    auto path = write_tmp(R"({
        "sensors": [{
            "id": "cpu",
            "type": "simulated",
            "metric": "temperature",
            "threshold_warn": 45.0,
            "threshold_crit": 70.0
        }],
        "hysteresis": 3.0,
        "poll_interval_ms": 1000
    })");

    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();

    ASSERT_EQ(result->sensors.size(), 1u);
    EXPECT_EQ(result->sensors[0].id,     "cpu");
    EXPECT_EQ(result->sensors[0].type,   rpi::SensorType::Simulated);
    EXPECT_EQ(result->sensors[0].metric, "temperature");
    EXPECT_FLOAT_EQ(result->sensors[0].threshold_warn, 45.0f);
    EXPECT_FLOAT_EQ(result->sensors[0].threshold_crit, 70.0f);
    EXPECT_FLOAT_EQ(result->hysteresis,  3.0f);
    EXPECT_EQ(result->poll_interval, std::chrono::milliseconds{1000});
}

TEST(ConfigLoader, DefaultsForMissingFields)
{
    // Minimal: one sensor, missing optional global fields -> use Config defaults
    auto path = write_tmp(R"({
        "sensors": [{"id": "s0", "type": "simulated", "threshold_warn": 50.0, "threshold_crit": 80.0}]
    })");

    rpi::Config defaults;
    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_FLOAT_EQ(result->hysteresis,  defaults.hysteresis);
    EXPECT_EQ(result->poll_interval,     defaults.poll_interval);
}

TEST(ConfigLoader, MultipleSensors)
{
    auto path = write_tmp(R"({
        "sensors": [
            {"id": "temp", "type": "simulated", "metric": "temperature", "threshold_warn": 60.0, "threshold_crit": 80.0},
            {"id": "pres", "type": "simulated", "metric": "pressure",    "threshold_warn": 1010.0, "threshold_crit": 1050.0}
        ]
    })");

    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->sensors.size(), 2u);
    EXPECT_EQ(result->sensors[0].metric, "temperature");
    EXPECT_EQ(result->sensors[1].metric, "pressure");
}

TEST(ConfigLoader, SensorTypeDS18B20)
{
    auto path = write_tmp(R"({
        "sensors": [{
            "id": "ext",
            "type": "ds18b20",
            "device_path": "/sys/bus/w1/devices/28-abc/temperature",
            "threshold_warn": 60.0,
            "threshold_crit": 80.0
        }]
    })");

    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->sensors[0].type,        rpi::SensorType::DS18B20);
    EXPECT_EQ(result->sensors[0].device_path, "/sys/bus/w1/devices/28-abc/temperature");
}

TEST(ConfigLoader, ErrorOnUnknownSensorType)
{
    auto path = write_tmp(R"({"sensors": [{"id": "s", "type": "bluetooth", "threshold_warn": 50.0, "threshold_crit": 80.0}]})");
    auto result = rpi::load_config(path);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigLoader, ErrorWhenWarnGeqCrit)
{
    auto path = write_tmp(R"({
        "sensors": [{"id": "s", "type": "simulated", "threshold_warn": 80.0, "threshold_crit": 60.0}]
    })");
    auto result = rpi::load_config(path);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigLoader, ErrorOnMalformedJSON)
{
    auto path = write_tmp("{ invalid json !!!");
    auto result = rpi::load_config(path);
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigLoader, ErrorOnMissingFile)
{
    auto result = rpi::load_config("/tmp/does_not_exist_xyz.json");
    EXPECT_FALSE(result.has_value());
}

TEST(ConfigLoader, CommentsAreIgnored)
{
    // nlohmann/json supports // comments when ignore_comments=true
    auto path = write_tmp(R"({
        // config comment
        "sensors": [{"id": "s0", "type": "simulated", "threshold_warn": 50.0, "threshold_crit": 75.0}]
    })");
    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();
}

TEST(ConfigLoader, ErrorOnEmptySensors)
{
    auto path = write_tmp(R"({"sensors": []})");
    auto result = rpi::load_config(path);
    EXPECT_FALSE(result.has_value());
}
