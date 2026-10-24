#include <gtest/gtest.h>
#include "../src/sensors/SGP30Reader.hpp"
#include "../src/sensors/Sgp30Device.hpp"
#include "../src/sensors/Sgp30Protocol.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace rpi;
using namespace std::chrono_literals;

namespace {

// Answers SGP30 commands from canned values and records what was written.
class FakeSgp30Bus final : public II2cBus {
public:
    std::vector<std::vector<std::uint8_t>> writes;
    std::uint16_t eco2 = 450, tvoc = 12;
    std::uint16_t baseline_eco2 = 0x8a3c, baseline_tvoc = 0x8f12;
    bool fail = false;

    auto write(std::span<const std::uint8_t> data) -> bool override
    {
        if (fail) return false;
        writes.emplace_back(data.begin(), data.end());
        return true;
    }

    auto read(std::span<std::uint8_t> data) -> bool override
    {
        if (fail || writes.empty()) return false;
        const auto cmd = last_command();
        std::array<std::uint16_t, 2> words{};
        if (cmd == sgp30::kCmdMeasureIaq)          words = {eco2, tvoc};
        else if (cmd == sgp30::kCmdGetIaqBaseline) words = {baseline_eco2, baseline_tvoc};
        else return false;
        for (std::size_t i = 0; i < data.size() / 3; ++i) {
            const std::array<std::uint8_t, 2> b{
                static_cast<std::uint8_t>(words[i] >> 8),
                static_cast<std::uint8_t>(words[i] & 0xFF),
            };
            data[i * 3]     = b[0];
            data[i * 3 + 1] = b[1];
            data[i * 3 + 2] = sgp30::crc8(b);
        }
        return true;
    }

    void wait(std::chrono::milliseconds) override {}

    auto last_command() const -> std::uint16_t { return command_of(writes.back()); }

    static auto command_of(const std::vector<std::uint8_t>& w) -> std::uint16_t
    {
        return static_cast<std::uint16_t>((w[0] << 8) | w[1]);
    }

    auto count(std::uint16_t cmd) const -> int
    {
        int n = 0;
        for (const auto& w : writes) n += command_of(w) == cmd;
        return n;
    }

    auto find(std::uint16_t cmd) const -> const std::vector<std::uint8_t>*
    {
        for (const auto& w : writes)
            if (command_of(w) == cmd) return &w;
        return nullptr;
    }
};

auto temp_dir(const std::string& name) -> std::filesystem::path
{
    auto dir = std::filesystem::temp_directory_path() / ("rpi_sgp30dev_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

auto now_seconds() -> std::int64_t
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Harness {
    FakeSgp30Bus* bus;
    std::shared_ptr<Sgp30Device> device;
};

auto make_device(Sgp30Options options) -> Harness
{
    auto bus = std::make_unique<FakeSgp30Bus>();
    auto* raw = bus.get();
    return {raw, std::make_shared<Sgp30Device>(std::move(bus), std::move(options))};
}

} // namespace

// ── Protocol helpers ────────────────────────────────────────────────────────

TEST(Sgp30Protocol, Crc8MatchesDatasheetExample)
{
    const std::array<std::uint8_t, 2> data{0xBE, 0xEF};
    EXPECT_EQ(sgp30::crc8(data), 0x92);
}

TEST(Sgp30Protocol, AbsoluteHumidityAt25C50Percent)
{
    EXPECT_NEAR(sgp30::absolute_humidity(25.0f, 50.0f), 11.5f, 0.1f);
    // 8.8 fixed point
    EXPECT_NEAR(sgp30::absolute_humidity_fixed(25.0f, 50.0f), 11.5f * 256.0f, 30.0f);
}

TEST(Sgp30Protocol, AbsoluteHumidityFixedNeverZero)
{
    EXPECT_EQ(sgp30::absolute_humidity_fixed(-40.0f, 0.0f), 1);
}

TEST(Sgp30Protocol, BaselineRoundTrip)
{
    auto path = temp_dir("RoundTrip") / "sub" / "baseline.txt";
    const sgp30::Baseline b{.eco2 = 0x8a3c, .tvoc = 0x8f12, .saved_at = now_seconds()};
    ASSERT_TRUE(sgp30::save_baseline(path, b));

    auto loaded = sgp30::load_baseline(path, std::chrono::system_clock::now());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->eco2, b.eco2);
    EXPECT_EQ(loaded->tvoc, b.tvoc);
}

TEST(Sgp30Protocol, BaselineOlderThanSevenDaysIsIgnored)
{
    auto path = temp_dir("TooOld") / "baseline.txt";
    const auto eight_days = std::chrono::duration_cast<std::chrono::seconds>(24h * 8).count();
    ASSERT_TRUE(sgp30::save_baseline(path, {.eco2 = 1, .tvoc = 1, .saved_at = now_seconds() - eight_days}));
    EXPECT_FALSE(sgp30::load_baseline(path, std::chrono::system_clock::now()).has_value());
}

TEST(Sgp30Protocol, MissingOrGarbageBaselineIsIgnored)
{
    auto dir = temp_dir("Garbage");
    EXPECT_FALSE(sgp30::load_baseline(dir / "none.txt", std::chrono::system_clock::now()));
    std::ofstream(dir / "bad.txt") << "not a baseline\n";
    EXPECT_FALSE(sgp30::load_baseline(dir / "bad.txt", std::chrono::system_clock::now()));
}

// ── Device ──────────────────────────────────────────────────────────────────

TEST(Sgp30Device, NoSampleDuringWarmup)
{
    auto [bus, device] = make_device({});
    ASSERT_TRUE(device->init());
    EXPECT_EQ(bus->count(sgp30::kCmdIaqInit), 1);

    for (int i = 0; i < sgp30::kWarmupMeasurements; ++i) device->step();
    EXPECT_FALSE(device->latest().has_value());

    device->step();
    ASSERT_TRUE(device->latest().has_value());
    EXPECT_EQ(device->latest()->eco2_ppm, 450);
    EXPECT_EQ(device->latest()->tvoc_ppb, 12);
}

TEST(Sgp30Device, RestoresBaselineTvocFirst)
{
    auto path = temp_dir("Restore") / "baseline.txt";
    ASSERT_TRUE(sgp30::save_baseline(path, {.eco2 = 0x1111, .tvoc = 0x2222, .saved_at = now_seconds()}));

    auto [bus, device] = make_device({.baseline_path = path.string()});
    ASSERT_TRUE(device->init());

    const auto* set = bus->find(sgp30::kCmdSetIaqBaseline);
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->size(), 8u);
    EXPECT_EQ((*set)[2], 0x22);   // TVOC word first
    EXPECT_EQ((*set)[5], 0x11);   // then eCO2
}

TEST(Sgp30Device, SavesRestoredBaselineHourly)
{
    auto path = temp_dir("SaveHourly") / "baseline.txt";
    ASSERT_TRUE(sgp30::save_baseline(path, {.eco2 = 1, .tvoc = 1, .saved_at = now_seconds()}));

    auto [bus, device] = make_device({.baseline_path = path.string()});
    ASSERT_TRUE(device->init());
    for (int i = 0; i < 3600; ++i) device->step();

    EXPECT_EQ(bus->count(sgp30::kCmdGetIaqBaseline), 1);
    auto saved = sgp30::load_baseline(path, std::chrono::system_clock::now());
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->eco2, 0x8a3c);
    EXPECT_EQ(saved->tvoc, 0x8f12);
}

TEST(Sgp30Device, FreshBaselineNotSavedBefore12Hours)
{
    auto path = temp_dir("Fresh") / "baseline.txt";
    auto [bus, device] = make_device({.baseline_path = path.string()});
    ASSERT_TRUE(device->init());

    for (int i = 0; i < 12 * 3600 - 1; ++i) device->step();
    EXPECT_EQ(bus->count(sgp30::kCmdGetIaqBaseline), 0);
    EXPECT_FALSE(std::filesystem::exists(path));

    device->step();
    EXPECT_EQ(bus->count(sgp30::kCmdGetIaqBaseline), 1);
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(Sgp30Device, SendsAbsoluteHumidityFromDht11)
{
    auto dir = temp_dir("Humidity");
    std::ofstream(dir / "in_temp_input") << "25000\n";
    std::ofstream(dir / "in_humidityrelative_input") << "50000\n";

    auto [bus, device] = make_device({.humidity_device_path = dir.string()});
    ASSERT_TRUE(device->init());

    const auto* set = bus->find(sgp30::kCmdSetAbsoluteHumidity);
    ASSERT_NE(set, nullptr);
    const auto value = static_cast<std::uint16_t>(((*set)[2] << 8) | (*set)[3]);
    EXPECT_EQ(value, sgp30::absolute_humidity_fixed(25.0f, 50.0f));
}

TEST(Sgp30Device, NoHumidityCommandWithoutSource)
{
    auto [bus, device] = make_device({});
    ASSERT_TRUE(device->init());
    for (int i = 0; i < 120; ++i) device->step();
    EXPECT_EQ(bus->count(sgp30::kCmdSetAbsoluteHumidity), 0);
}

TEST(Sgp30Device, RepeatedFailuresDropSampleAndReinit)
{
    auto [bus, device] = make_device({});
    ASSERT_TRUE(device->init());
    for (int i = 0; i <= sgp30::kWarmupMeasurements; ++i) device->step();
    ASSERT_TRUE(device->latest().has_value());

    bus->fail = true;
    for (int i = 0; i < 10; ++i) device->step();
    EXPECT_FALSE(device->latest().has_value());

    bus->fail = false;
    ASSERT_TRUE(device->init());
    EXPECT_EQ(bus->count(sgp30::kCmdIaqInit), 2);
}

TEST(Sgp30Device, ReaderMapsMetricsAndReportsWarmup)
{
    auto [bus, device] = make_device({});
    SGP30Reader eco2(device, "sgp30-eco2", "eco2");
    SGP30Reader tvoc(device, "sgp30-tvoc", "tvoc");
    ASSERT_TRUE(device->init());

    EXPECT_EQ(eco2.read().error(), SensorError::ReadFailure);

    for (int i = 0; i <= sgp30::kWarmupMeasurements; ++i) device->step();
    ASSERT_TRUE(eco2.read().has_value());
    EXPECT_FLOAT_EQ(eco2.read()->value, 450.0f);
    EXPECT_FLOAT_EQ(tvoc.read()->value, 12.0f);
    EXPECT_EQ(tvoc.read()->metric, "tvoc");
}
