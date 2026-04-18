#pragma once

#include "WebState.hpp"
#include "../monitoring/Config.hpp"
#include <thread>

namespace httplib { class Server; }

namespace rpi {

class HttpServer {
public:
    HttpServer(const Config& config, const WebState& state);
    ~HttpServer();

    void start();
    void stop();

private:
    std::string build_state_json(const WebState::Snapshot& snap) const;
    std::string compute_status(float temp) const;

    const WebState& state_;
    Config          config_;
    std::unique_ptr<httplib::Server> server_;
    std::thread     thread_;
};

} // namespace rpi
