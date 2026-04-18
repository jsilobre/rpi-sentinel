#include "monitoring/Config.hpp"
#include "monitoring/ThresholdMonitor.hpp"
#include "sensors/DS18B20Reader.hpp"
#include "sensors/SimulatedSensor.hpp"
#include "events/EventBus.hpp"
#include "alerts/LogAlert.hpp"

#include <csignal>
#include <memory>
#include <print>

namespace {
    volatile std::sig_atomic_t g_running = 1;
}

static void signal_handler(int) { g_running = 0; }

static auto make_sensor(const rpi::Config& cfg) -> std::unique_ptr<rpi::ISensorReader>
{
    switch (cfg.sensor_type) {
        case rpi::SensorType::DS18B20:
            return std::make_unique<rpi::DS18B20Reader>(cfg.device_path);
        case rpi::SensorType::Simulated:
            return std::make_unique<rpi::SimulatedSensor>("sim-0");
    }
    return std::make_unique<rpi::SimulatedSensor>("sim-0");
}

int main()
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    rpi::Config config{
        .sensor_type    = rpi::SensorType::Simulated,
        .threshold_warn = 50.0f,
        .threshold_crit = 65.0f,
        .hysteresis     = 2.0f,
        .poll_interval  = std::chrono::milliseconds{2000},
    };

    auto sensor = make_sensor(config);

    rpi::EventBus bus;
    bus.register_handler(std::make_shared<rpi::LogAlert>());

    rpi::ThresholdMonitor monitor(*sensor, bus, config);
    monitor.start();

    std::println("[main] Monitor started. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    std::println("[main] Shutting down...");
    monitor.stop();
    return 0;
}
