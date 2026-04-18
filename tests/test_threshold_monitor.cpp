#include <gtest/gtest.h>
#include "../src/monitoring/ThresholdMonitor.hpp"
#include "../src/sensors/SimulatedSensor.hpp"
#include "../src/events/EventBus.hpp"
#include "../src/alerts/IAlertHandler.hpp"

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
