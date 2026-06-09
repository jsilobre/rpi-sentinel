#include "monitoring/Config.hpp"
#include "monitoring/ConfigLoader.hpp"
#include "monitoring/MonitoringHub.hpp"
#include "events/EventBus.hpp"
#include "alerts/GpioAlert.hpp"
#include "alerts/LogAlert.hpp"
#include "persistence/HistoryStore.hpp"
#include "persistence/SqliteHistoryHandler.hpp"
#ifdef ENABLE_MQTT
#include "alerts/MqttPublisher.hpp"
#endif

#ifdef ENABLE_OTLP
#include "otel/OtlpExporter.hpp"
#endif

#ifdef ENABLE_CLOUD_STORAGE
#include "alerts/CloudStorageHandler.hpp"
#include <curl/curl.h>
#endif

#include <csignal>
#include <chrono>
#include <filesystem>
#include <memory>
#include <print>
#include <thread>

namespace {
    volatile std::sig_atomic_t g_running = 1;
}

static void signal_handler(int) { g_running = 0; }

int main(int argc, char* argv[])
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

#ifdef ENABLE_CLOUD_STORAGE
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

    const std::filesystem::path config_path = (argc > 1) ? argv[1] : "config.json";

    auto result = rpi::load_config(config_path);
    if (!result) {
        std::println(stderr, "[main] Config error: {}", result.error());
        std::println(stderr, "[main] Usage: {} [config.json]", argv[0]);
        return 1;
    }

    std::println("[main] Config loaded from '{}'", config_path.string());

    rpi::EventBus bus;
    bus.register_handler(std::make_shared<rpi::LogAlert>());

    if (result->gpio_alert.enabled) {
        bus.register_handler(std::make_shared<rpi::GpioAlert>(result->gpio_alert.pin));
        std::println("[main] GPIO alert enabled on pin {}.", result->gpio_alert.pin);
    }

    std::shared_ptr<rpi::HistoryStore> history_store;
    if (result->history.enabled) {
        try {
            history_store = std::make_shared<rpi::HistoryStore>(
                result->history.db_path,
                result->history.retention_days,
                result->history.max_points_per_sensor);
            bus.register_handler(std::make_shared<rpi::SqliteHistoryHandler>(history_store));
        } catch (const std::exception& e) {
            std::println(stderr, "[main] History store error: {}", e.what());
            history_store.reset();
        }
    }

#ifdef ENABLE_MQTT
    std::shared_ptr<rpi::MqttPublisher> mqtt_pub;
    if (result->mqtt.enabled) {
        try {
            mqtt_pub = std::make_shared<rpi::MqttPublisher>(result->mqtt);
            if (history_store) mqtt_pub->set_history_store(history_store);
            mqtt_pub->connect();
            bus.register_handler(mqtt_pub);
        } catch (const std::exception& e) {
            std::println(stderr, "[main] MQTT error: {}", e.what());
        }
    }
#endif

#ifdef ENABLE_OTLP
    std::shared_ptr<rpi::OtlpExporter> otlp_exporter;
    if (result->otlp.enabled) {
        try {
            otlp_exporter = std::make_shared<rpi::OtlpExporter>(result->otlp, result->sensors);
            bus.register_handler(otlp_exporter);
        } catch (const std::exception& e) {
            std::println(stderr, "[main] OTLP exporter init failed: {}", e.what());
            otlp_exporter.reset();
        }
    }
#else
    if (result->otlp.enabled) {
        std::println(stderr, "[main] otlp.enabled=true but binary built without ENABLE_OTLP — ignored");
    }
#endif

#ifdef ENABLE_CLOUD_STORAGE
    std::shared_ptr<rpi::CloudStorageHandler> cloud_handler;
    if (result->cloud_storage.enabled) {
        try {
            cloud_handler = std::make_shared<rpi::CloudStorageHandler>(result->cloud_storage);
            bus.register_handler(cloud_handler);
        } catch (const std::exception& e) {
            std::println(stderr, "[main] Cloud storage error: {}", e.what());
        }
    }
#else
    if (result->cloud_storage.enabled) {
        std::println(stderr, "[main] cloud_storage.enabled=true but binary built without ENABLE_CLOUD_STORAGE — ignored");
    }
#endif

    rpi::MonitoringHub hub(bus, std::move(*result));

    auto on_config_change = [&hub, &config_path
#ifdef ENABLE_MQTT
        , &mqtt_pub
#endif
    ](const std::string& sensor_id, float warn, float crit)
        -> std::expected<void, std::string>
    {
        hub.update_thresholds(sensor_id, warn, crit);
        if (auto r = rpi::save_config(config_path, hub.get_config_snapshot()); !r) {
            std::println(stderr, "[main] save_config failed: {}", r.error());
            return r;
        }
#ifdef ENABLE_MQTT
        if (mqtt_pub) mqtt_pub->publish_config(hub.build_config_json());
#endif
        return {};
    };

#ifdef ENABLE_MQTT
    if (mqtt_pub) {
        mqtt_pub->set_threshold_callback(on_config_change);
        mqtt_pub->set_force_poller([&hub]() { hub.force_poll_all(); });
        mqtt_pub->set_data_clearer([&history_store]() {
            if (history_store) history_store->clear_all();
            std::println("[MqttPublisher] History cleared by user request.");
        });
        mqtt_pub->publish_config(hub.build_config_json());
    }
#endif

    hub.start();

    std::println("[main] Monitors started. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    std::println("[main] Shutting down...");
#ifdef ENABLE_MQTT
    if (mqtt_pub) mqtt_pub->disconnect();
#endif
    hub.stop();
    // The bus holds shared_ptr copies of every handler; drop them so the
    // resets below are the last reference and actually destroy the handlers.
    bus.clear_handlers();
#ifdef ENABLE_OTLP
    // Reset after monitors have stopped so any final dispatches are
    // queued, then destruction flushes the SDK providers.
    otlp_exporter.reset();
#endif
#ifdef ENABLE_CLOUD_STORAGE
    // Destroy handler (joins sender thread + flushes) before curl cleanup;
    // curl_global_cleanup() while a transfer is in flight is UB.
    cloud_handler.reset();
    curl_global_cleanup();
#endif
    return 0;
}
