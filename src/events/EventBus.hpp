#pragma once

#include "ThermalEvent.hpp"
#include <memory>
#include <mutex>
#include <vector>

namespace rpi {

class IAlertHandler;

class EventBus {
public:
    void register_handler(std::shared_ptr<IAlertHandler> handler);
    void dispatch(const ThermalEvent& event);

private:
    std::vector<std::shared_ptr<IAlertHandler>> handlers_;
    std::mutex                                  mutex_;
};

} // namespace rpi
