#include "monitoring/Config.hpp"
#include "monitoring/ConfigLoader.hpp"
#include "monitoring/MonitoringHub.hpp"
#include "events/EventBus.hpp"
#include "alerts/LogAlert.hpp"
#include "web/WebAlert.hpp"
#include "web/WebState.hpp"
#include "web/HttpServer.hpp"

#include <csignal>
#include <chrono>
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

    const std::string config_path = (argc > 1) ? argv[1] : "config.json";

    auto result = rpi::load_config(config_path);
    if (!result) {
        std::println(stderr, "[main] Config error: {}", result.error());
        std::println(stderr, "[main] Usage: {} [config.json]", argv[0]);
        return 1;
    }

    const rpi::Config config = *result;
    std::println("[main] Config loaded from '{}'", config_path);

    rpi::WebState web_state;
    rpi::EventBus bus;
    bus.register_handler(std::make_shared<rpi::LogAlert>());
    bus.register_handler(std::make_shared<rpi::WebAlert>(web_state));

    rpi::MonitoringHub hub(bus, config);
    hub.start();

    std::unique_ptr<rpi::HttpServer> http_server;
    if (config.web_enabled) {
        http_server = std::make_unique<rpi::HttpServer>(config.web_port, web_state);
        http_server->start();
    }

    std::println("[main] Monitors started. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    std::println("[main] Shutting down...");
    hub.stop();
    if (http_server) http_server->stop();
    return 0;
}
