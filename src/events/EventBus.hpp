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

    // Drops all handler references. Call after monitors are stopped so
    // handlers owned elsewhere can actually be destroyed (the bus holds
    // shared_ptr copies).
    void clear_handlers();

    void dispatch(const SensorEvent& event);

private:
    std::vector<std::shared_ptr<IAlertHandler>> handlers_;
    std::mutex                                  handlers_mutex_;
    // Serializes dispatches across monitor threads. Recursive so a handler
    // that dispatches from on_event() doesn't deadlock.
    std::recursive_mutex                        dispatch_mutex_;
};

} // namespace rpi
