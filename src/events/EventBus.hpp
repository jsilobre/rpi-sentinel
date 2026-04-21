#pragma once

#include "SensorEvent.hpp"
#include <memory>
#include <mutex>
#include <vector>

namespace rpi {

class IAlertHandler;

class EventBus {
public:
    void register_handler(std::shared_ptr<IAlertHandler> handler);
    void dispatch(const SensorEvent& event);

private:
    std::vector<std::shared_ptr<IAlertHandler>> handlers_;
    std::mutex                                  mutex_;
};

} // namespace rpi
