#include <gtest/gtest.h>
#include "../src/monitoring/ConfigLoader.hpp"

#include <filesystem>
#include <fstream>

// Écrit un fichier JSON temporaire et retourne son chemin.
static std::filesystem::path write_tmp(const std::string& content)
{
    auto path = std::filesystem::temp_directory_path() / "rpi_test_config.json";
    std::ofstream(path) << content;
    return path;
}

TEST(ConfigLoader, LoadsValidConfig)
{
    auto path = write_tmp(R"({
        "sensor_type":      "simulated",
        "threshold_warn":   45.0,
        "threshold_crit":   70.0,
        "hysteresis":       3.0,
        "poll_interval_ms": 1000
    })");

    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->sensor_type,   rpi::SensorType::Simulated);
    EXPECT_FLOAT_EQ(result->threshold_warn, 45.0f);
    EXPECT_FLOAT_EQ(result->threshold_crit, 70.0f);
    EXPECT_FLOAT_EQ(result->hysteresis,      3.0f);
    EXPECT_EQ(result->poll_interval, std::chrono::milliseconds{1000});
}

TEST(ConfigLoader, DefaultsForMissingFields)
{
    // JSON minimal — tous les champs absents → valeurs par défaut de Config
    auto path = write_tmp(R"({ "sensor_type": "simulated" })");

    rpi::Config defaults;
    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_FLOAT_EQ(result->threshold_warn, defaults.threshold_warn);
    EXPECT_FLOAT_EQ(result->threshold_crit, defaults.threshold_crit);
    EXPECT_FLOAT_EQ(result->hysteresis,     defaults.hysteresis);
    EXPECT_EQ(result->poll_interval,        defaults.poll_interval);
}

TEST(ConfigLoader, SensorTypeDS18B20)
{
    auto path = write_tmp(R"({
        "sensor_type": "ds18b20",
        "device_path": "/sys/bus/w1/devices/28-abc/temperature"
    })");

    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->sensor_type, rpi::SensorType::DS18B20);
    EXPECT_EQ(result->device_path, "/sys/bus/w1/devices/28-abc/temperature");
}

TEST(ConfigLoader, ErrorOnUnknownSensorType)
{
    auto path = write_tmp(R"({ "sensor_type": "bluetooth" })");
    auto result = rpi::load_config(path);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Unknown sensor_type"), std::string::npos);
}

TEST(ConfigLoader, ErrorWhenWarnGeqCrit)
{
    auto path = write_tmp(R"({
        "sensor_type":    "simulated",
        "threshold_warn": 80.0,
        "threshold_crit": 60.0
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
    // nlohmann/json supporte les commentaires // si ignore_comments=true
    auto path = write_tmp(R"({
        // commentaire de configuration
        "sensor_type": "simulated",
        "threshold_warn": 50.0,
        "threshold_crit": 75.0
    })");
    auto result = rpi::load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error();
}
