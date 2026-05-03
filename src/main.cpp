#include "monitoring/Config.hpp"
#include "monitoring/ConfigLoader.hpp"
#include "monitoring/MonitoringHub.hpp"
#include "events/EventBus.hpp"
#include "alerts/GpioAlert.hpp"
#include "alerts/LogAlert.hpp"
#include "persistence/HistoryStore.hpp"
#include "persistence/SqliteHistoryHandler.hpp"
#include "web/WebAlert.hpp"
#include "web/WebState.hpp"
#include "web/HttpServer.hpp"

#ifdef ENABLE_MQTT
#include "alerts/MqttPublisher.hpp"
#endif

#ifdef ENABLE_OTLP
#include "otel/OtlpExporter.hpp"
#endif

#include <csignal>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <thread>

namespace {
    volatile std::sig_atomic_t g_running = 1;
}

static void signal_handler(int) { g_running = 0; }

int main(int argc, char* argv[])
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    const std::filesystem::path config_path = (argc > 1) ? argv[1] : "config.json";

    auto result = rpi::load_config(config_path);
    if (!result) {
        std::println(stderr, "[main] Config error: {}", result.error());
        std::println(stderr, "[main] Usage: {} [config.json]", argv[0]);
        return 1;
    }

    std::println("[main] Config loaded from '{}'", config_path.string());

    rpi::WebState web_state;
    rpi::EventBus bus;
    bus.register_handler(std::make_shared<rpi::LogAlert>());
    bus.register_handler(std::make_shared<rpi::WebAlert>(web_state));

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

    if (history_store) {
        for (const auto& s : result->sensors) {
            auto pts = history_store->recent(s.id, rpi::WebState::MAX_HISTORY);
            std::vector<rpi::HistoryPoint> hp;
            hp.reserve(pts.size());
            for (const auto& p : pts) {
                hp.push_back({p.value,
                    std::chrono::system_clock::time_point{
                        std::chrono::milliseconds{p.ts_ms}}});
            }
            if (!hp.empty()) {
                web_state.prime_history(s.id, s.metric, std::move(hp));
            }
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
        mqtt_pub->publish_config(hub.build_config_json());
    }
#endif

    hub.start();

    std::unique_ptr<rpi::HttpServer> http_server;
    if (hub.get_config_snapshot().web_enabled) {
        std::string web_token;
        const auto& auth_cfg = hub.get_config_snapshot().web_auth;
        if (auth_cfg.enabled) {
            if (!auth_cfg.token_env.empty()) {
                if (const char* v = std::getenv(auth_cfg.token_env.c_str()); v && *v) {
                    web_token = v;
                }
            }
            if (web_token.empty()) web_token = auth_cfg.token;

            if (web_token.empty()) {
                std::println(stderr,
                    "[main] web_auth.enabled=true but no token resolved "
                    "(env '{}' empty and no literal token) — refusing to start HTTP server",
                    auth_cfg.token_env);
            } else {
                std::println("[main] Web auth enabled (bearer token required for POST routes).");
            }
        }

        const bool start_http = !auth_cfg.enabled || !web_token.empty();
        if (start_http) {
            http_server = std::make_unique<rpi::HttpServer>(
                hub.get_config_snapshot().web_port,
                web_state,
                history_store,
                [&hub]() { return hub.build_config_json(); },
                on_config_change,
                [&hub]() { hub.force_poll_all(); },
                std::move(web_token));
            http_server->start();
        }
    }

    std::println("[main] Monitors started. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    std::println("[main] Shutting down...");
#ifdef ENABLE_MQTT
    if (mqtt_pub) mqtt_pub->disconnect();
#endif
    hub.stop();
    if (http_server) http_server->stop();
#ifdef ENABLE_OTLP
    // Reset after monitors have stopped so any final dispatches are
    // queued, then destruction flushes the SDK providers.
    otlp_exporter.reset();
#endif
    return 0;
}
