#pragma once

#include "WebState.hpp"
#include <cstdint>
#include <memory>
#include <thread>

namespace httplib { class Server; }

namespace rpi {

class HttpServer {
public:
    HttpServer(uint16_t port, const WebState& state);
    ~HttpServer();

    void start();
    void stop();

private:
    std::string build_state_json(const WebState::Snapshot& snap) const;
    std::string compute_status(const WebState::Snapshot& snap) const;

    const WebState&                  state_;
    uint16_t                         port_;
    std::unique_ptr<httplib::Server> server_;
    std::thread                      thread_;
};

} // namespace rpi
