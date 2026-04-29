#include <gtest/gtest.h>
#include "../src/alerts/GpioAlert.hpp"
#include "../src/events/SensorEvent.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Creates a mock sysfs GPIO tree under a unique temp directory and returns its path.
// Layout:
//   <root>/export
//   <root>/unexport
//   <root>/gpio<pin>/direction
//   <root>/gpio<pin>/value
static fs::path make_mock_sysfs(int pin)
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? std::string(info->test_suite_name()) + "_" + info->name() : "unknown";
    auto root = fs::temp_directory_path() / ("rpi_gpio_" + name);
    fs::remove_all(root);
    fs::create_directories(root);

    std::ofstream(root / "export").close();
    std::ofstream(root / "unexport").close();

    auto gpio_dir = root / std::format("gpio{}", pin);
    fs::create_directories(gpio_dir);
    std::ofstream(gpio_dir / "direction").close();
    std::ofstream(gpio_dir / "value").close();

    return root;
}

static std::string read_file(const fs::path& p)
{
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

// ─── Construction ────────────────────────────────────────────────────────────

TEST(GpioAlert, ConstructorSetsDirectionOut)
{
    auto root = make_mock_sysfs(17);
    { rpi::GpioAlert alert(17, root); }
    EXPECT_EQ(read_file(root / "gpio17" / "direction"), "out");
}

TEST(GpioAlert, ConstructorWritesPinToExport)
{
    auto root = make_mock_sysfs(27);
    { rpi::GpioAlert alert(27, root); }
    EXPECT_EQ(read_file(root / "export"), "27");
}

// ─── ThresholdExceeded → pin HIGH ────────────────────────────────────────────

TEST(GpioAlert, ThresholdExceededSetsHigh)
{
    auto root = make_mock_sysfs(17);
    rpi::GpioAlert alert(17, root);

    rpi::SensorEvent ev;
    ev.type      = rpi::SensorEvent::Type::ThresholdExceeded;
    ev.sensor_id = "temp1";
    ev.metric    = "temperature";
    ev.value     = 75.0f;
    ev.threshold = 70.0f;

    alert.on_event(ev);
    EXPECT_EQ(read_file(root / "gpio17" / "value"), "1");
}

// ─── ThresholdRecovered → pin LOW ────────────────────────────────────────────

TEST(GpioAlert, ThresholdRecoveredSetsLow)
{
    auto root = make_mock_sysfs(17);
    rpi::GpioAlert alert(17, root);

    rpi::SensorEvent exceeded;
    exceeded.type      = rpi::SensorEvent::Type::ThresholdExceeded;
    exceeded.sensor_id = "temp1";
    exceeded.metric    = "temperature";
    exceeded.value     = 75.0f;
    exceeded.threshold = 70.0f;
    alert.on_event(exceeded);

    rpi::SensorEvent recovered;
    recovered.type      = rpi::SensorEvent::Type::ThresholdRecovered;
    recovered.sensor_id = "temp1";
    recovered.metric    = "temperature";
    recovered.value     = 65.0f;
    recovered.threshold = 70.0f;
    alert.on_event(recovered);

    EXPECT_EQ(read_file(root / "gpio17" / "value"), "0");
}

// ─── Reading events are ignored ──────────────────────────────────────────────

TEST(GpioAlert, ReadingEventDoesNotChangePin)
{
    auto root = make_mock_sysfs(17);
    rpi::GpioAlert alert(17, root);

    rpi::SensorEvent ev;
    ev.type      = rpi::SensorEvent::Type::Reading;
    ev.sensor_id = "temp1";
    ev.metric    = "temperature";
    ev.value     = 42.0f;
    ev.threshold = 0.0f;
    alert.on_event(ev);

    // value file should still be empty (constructor never set it)
    EXPECT_EQ(read_file(root / "gpio17" / "value"), "");
}

// ─── Destructor resets pin and unexports ─────────────────────────────────────

TEST(GpioAlert, DestructorSetsLowAndUnexports)
{
    auto root = make_mock_sysfs(22);
    {
        rpi::GpioAlert alert(22, root);
        rpi::SensorEvent ev;
        ev.type      = rpi::SensorEvent::Type::ThresholdExceeded;
        ev.sensor_id = "s";
        ev.metric    = "temperature";
        ev.value     = 90.0f;
        ev.threshold = 80.0f;
        alert.on_event(ev);
        EXPECT_EQ(read_file(root / "gpio22" / "value"), "1");
    }
    EXPECT_EQ(read_file(root / "gpio22" / "value"), "0");
    EXPECT_EQ(read_file(root / "unexport"), "22");
}

// ─── Graceful degradation on missing sysfs ───────────────────────────────────

TEST(GpioAlert, MissingSysfsDoesNotCrash)
{
    // Provide a root that exists but has no gpio directory pre-created.
    auto root = fs::temp_directory_path() / "rpi_gpio_no_pin";
    fs::remove_all(root);
    fs::create_directories(root);
    std::ofstream(root / "export").close();
    std::ofstream(root / "unexport").close();
    // gpio17/ deliberately NOT created → direction write will fail

    EXPECT_NO_THROW({
        rpi::GpioAlert alert(17, root);
        rpi::SensorEvent ev;
        ev.type      = rpi::SensorEvent::Type::ThresholdExceeded;
        ev.sensor_id = "s";
        ev.metric    = "temperature";
        ev.value     = 90.0f;
        ev.threshold = 80.0f;
        alert.on_event(ev);
    });
}

// ─── Config loader round-trip ────────────────────────────────────────────────

#include "../src/monitoring/ConfigLoader.hpp"
#include <sstream>

static fs::path write_json(const std::string& content)
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = info ? info->name() : "unknown";
    auto path = fs::temp_directory_path() / ("rpi_gpio_cfg_" + name + ".json");
    std::ofstream(path) << content;
    return path;
}

TEST(GpioAlert, ConfigLoaderParsesGpioBlock)
{
    auto path = write_json(R"({
        "sensors": [{"id": "s0", "type": "simulated", "threshold_warn": 50.0, "threshold_crit": 80.0}],
        "gpio_alert": {"enabled": true, "pin": 22}
    })");
    auto cfg = rpi::load_config(path);
    ASSERT_TRUE(cfg.has_value()) << cfg.error();
    EXPECT_TRUE(cfg->gpio_alert.enabled);
    EXPECT_EQ(cfg->gpio_alert.pin, 22);
}

TEST(GpioAlert, ConfigLoaderDefaultsGpioDisabled)
{
    auto path = write_json(R"({
        "sensors": [{"id": "s0", "type": "simulated", "threshold_warn": 50.0, "threshold_crit": 80.0}]
    })");
    auto cfg = rpi::load_config(path);
    ASSERT_TRUE(cfg.has_value()) << cfg.error();
    EXPECT_FALSE(cfg->gpio_alert.enabled);
    EXPECT_EQ(cfg->gpio_alert.pin, 17);
}

TEST(GpioAlert, ConfigLoaderRejectsNegativePin)
{
    auto path = write_json(R"({
        "sensors": [{"id": "s0", "type": "simulated", "threshold_warn": 50.0, "threshold_crit": 80.0}],
        "gpio_alert": {"pin": -1}
    })");
    auto cfg = rpi::load_config(path);
    EXPECT_FALSE(cfg.has_value());
}
