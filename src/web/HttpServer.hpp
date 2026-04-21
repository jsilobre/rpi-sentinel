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

class HttpServer {
public:
    using ConfigGetter  = std::function<std::string()>;
    using ConfigUpdater = std::function<std::expected<void, std::string>(
                              const std::string& sensor_id, float warn, float crit)>;

    HttpServer(uint16_t port, const WebState& state,
               ConfigGetter  config_getter,
               ConfigUpdater config_updater);
    ~HttpServer();

    void start();
    void stop();

private:
    std::string build_state_json(const WebState::Snapshot& snap) const;

    const WebState&                  state_;
    uint16_t                         port_;
    ConfigGetter                     config_getter_;
    ConfigUpdater                    config_updater_;
    std::unique_ptr<httplib::Server> server_;
    std::thread                      thread_;
};

} // namespace rpi
