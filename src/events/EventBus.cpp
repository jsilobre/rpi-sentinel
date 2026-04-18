#include "EventBus.hpp"
#include "../alerts/IAlertHandler.hpp"

namespace rpi {

void EventBus::register_handler(std::shared_ptr<IAlertHandler> handler)
{
    std::lock_guard lock(mutex_);
    handlers_.push_back(std::move(handler));
}

void EventBus::dispatch(const ThermalEvent& event)
{
    std::lock_guard lock(mutex_);
    for (auto& handler : handlers_) {
        handler->on_event(event);
    }
}

} // namespace rpi
