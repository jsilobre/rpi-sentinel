#pragma once

#include "WebState.hpp"
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace rpi {

class HistoryStore; // forward declaration — full type in HttpServer.cpp

class HttpServer {
public:
    using ConfigGetter  = std::function<std::string()>;
    using ConfigUpdater = std::function<std::expected<void, std::string>(
                              const std::string& sensor_id, float warn, float crit)>;

    // history_store may be nullptr when persistence is disabled.
    HttpServer(uint16_t port, const WebState& state,
               std::shared_ptr<const HistoryStore> history_store,
               ConfigGetter  config_getter,
               ConfigUpdater config_updater);
    ~HttpServer();

    void start();
    void stop();

private:
    std::string build_state_json(const WebState::Snapshot& snap) const;

    const WebState&                      state_;
    uint16_t                             port_;
    std::shared_ptr<const HistoryStore>  history_store_;
    ConfigGetter                         config_getter_;
    ConfigUpdater                        config_updater_;
    std::unique_ptr<httplib::Server>     server_;
    std::thread                          thread_;
};

} // namespace rpi
