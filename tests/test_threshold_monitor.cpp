#include <gtest/gtest.h>
#include "../src/monitoring/ThresholdMonitor.hpp"
#include "../src/sensors/SimulatedSensor.hpp"
#include "../src/events/EventBus.hpp"
#include "../src/alerts/IAlertHandler.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace rpi;

// Captures only alert events (ignores Reading)
class CapturingHandler final : public IAlertHandler {
public:
    void on_event(const SensorEvent& ev) override {
        if (ev.type == SensorEvent::Type::Reading) return;
        std::lock_guard lock(mutex_);
        events.push_back(ev);
    }
    std::vector<SensorEvent> events;
    std::mutex mutex_;
};

TEST(ThresholdMonitor, ExceedanceIsReported)
{
    // Sensor always returns 75 — above warn (50) and crit (65)
    SimulatedSensor sensor("test", "temperature", []() { return 75.0f; });

    EventBus bus;
    auto handler = std::make_shared<CapturingHandler>();
    bus.register_handler(handler);

    MonitorConfig cfg{
        .threshold_warn = 50.0f,
        .threshold_crit = 65.0f,
        .hysteresis     = 2.0f,
        .poll_interval  = std::chrono::milliseconds{50},
    };

    ThresholdMonitor monitor(sensor, bus, cfg);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    monitor.stop();

    std::lock_guard lock(handler->mutex_);
    ASSERT_FALSE(handler->events.empty());
    EXPECT_EQ(handler->events[0].type, SensorEvent::Type::ThresholdExceeded);
}

TEST(ThresholdMonitor, BelowThresholdNoEvent)
{
    // Sensor returns 20 — below all thresholds
    SimulatedSensor sensor("test", "temperature", []() { return 20.0f; });

    EventBus bus;
    auto handler = std::make_shared<CapturingHandler>();
    bus.register_handler(handler);

    MonitorConfig cfg{
        .threshold_warn = 50.0f,
        .threshold_crit = 65.0f,
        .hysteresis     = 2.0f,
        .poll_interval  = std::chrono::milliseconds{50},
    };

    ThresholdMonitor monitor(sensor, bus, cfg);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    monitor.stop();

    std::lock_guard lock(handler->mutex_);
    EXPECT_TRUE(handler->events.empty());
}

// Captures only Reading events, to inspect the level they carry
class ReadingCapture final : public IAlertHandler {
public:
    void on_event(const SensorEvent& ev) override {
        if (ev.type != SensorEvent::Type::Reading) return;
        std::lock_guard lock(mutex_);
        readings.push_back(ev);
    }
    std::vector<SensorEvent> readings;
    std::mutex mutex_;
};

namespace {

std::vector<SensorEvent> readings_for(float value)
{
    SimulatedSensor sensor("test", "tvoc", [value]() { return value; });

    EventBus bus;
    auto handler = std::make_shared<ReadingCapture>();
    bus.register_handler(handler);

    MonitorConfig cfg{
        .threshold_warn = 50.0f,
        .threshold_crit = 65.0f,
        .hysteresis     = 2.0f,
        .poll_interval  = std::chrono::milliseconds{50},
    };

    ThresholdMonitor monitor(sensor, bus, cfg);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    monitor.stop();

    std::lock_guard lock(handler->mutex_);
    return handler->readings;
}

} // namespace

TEST(ThresholdMonitor, ReadingCarriesCurrentLevel)
{
    // Every reading reports the level, not just the one that crossed the
    // threshold — so a late subscriber learns the state from any reading.
    const auto crit = readings_for(75.0f);
    ASSERT_GE(crit.size(), 2u);
    for (const auto& r : crit) EXPECT_EQ(r.level, SensorEvent::Level::Crit);

    const auto warn = readings_for(55.0f);
    ASSERT_GE(warn.size(), 2u);
    for (const auto& r : warn) EXPECT_EQ(r.level, SensorEvent::Level::Warn);

    const auto ok = readings_for(20.0f);
    ASSERT_FALSE(ok.empty());
    for (const auto& r : ok) EXPECT_EQ(r.level, SensorEvent::Level::Ok);
}

TEST(ThresholdMonitor, TransitionCarriesCrossedThresholdLevel)
{
    // The alert event says which threshold was crossed, so the dashboard's
    // timeline can tell a warning from a critical alert.
    auto first_transition = [](float value) {
        SimulatedSensor sensor("test", "tvoc", [value]() { return value; });

        EventBus bus;
        auto handler = std::make_shared<CapturingHandler>();
        bus.register_handler(handler);

        MonitorConfig cfg{
            .threshold_warn = 50.0f,
            .threshold_crit = 65.0f,
            .hysteresis     = 2.0f,
            .poll_interval  = std::chrono::milliseconds{50},
        };

        ThresholdMonitor monitor(sensor, bus, cfg);
        monitor.start();
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
        monitor.stop();

        std::lock_guard lock(handler->mutex_);
        EXPECT_EQ(handler->events.size(), 1u);  // one-shot, not repeated each poll
        return handler->events.empty() ? SensorEvent{} : handler->events[0];
    };

    const auto crit = first_transition(75.0f);
    EXPECT_EQ(crit.type, SensorEvent::Type::ThresholdExceeded);
    EXPECT_EQ(crit.level, SensorEvent::Level::Crit);
    EXPECT_FLOAT_EQ(crit.threshold, 65.0f);

    const auto warn = first_transition(55.0f);
    EXPECT_EQ(warn.type, SensorEvent::Type::ThresholdExceeded);
    EXPECT_EQ(warn.level, SensorEvent::Level::Warn);
    EXPECT_FLOAT_EQ(warn.threshold, 50.0f);
}

TEST(ThresholdMonitor, SetPollIntervalCutsLongSleepShort)
{
    // Starts with a one-hour interval: without the change, only the initial
    // read would ever happen during this test.
    std::atomic<int> reads{0};
    SimulatedSensor sensor("test", "temperature", [&reads]() { ++reads; return 20.0f; });

    EventBus bus;
    MonitorConfig cfg{
        .threshold_warn = 50.0f,
        .threshold_crit = 65.0f,
        .hysteresis     = 2.0f,
        .poll_interval  = std::chrono::hours{1},
    };

    ThresholdMonitor monitor(sensor, bus, cfg);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_EQ(reads.load(), 1);

    monitor.set_poll_interval(std::chrono::milliseconds{20});
    EXPECT_EQ(monitor.get_poll_interval(), std::chrono::milliseconds{20});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    monitor.stop();

    EXPECT_GE(reads.load(), 4);
}
