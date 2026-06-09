#include "EventBus.hpp"
#include "../alerts/IAlertHandler.hpp"

namespace rpi {

void EventBus::register_handler(std::shared_ptr<IAlertHandler> handler)
{
    std::lock_guard lock(handlers_mutex_);
    handlers_.push_back(std::move(handler));
}

void EventBus::clear_handlers()
{
    std::lock_guard lock(handlers_mutex_);
    handlers_.clear();
}

void EventBus::dispatch(const SensorEvent& event)
{
    std::lock_guard dispatch_lock(dispatch_mutex_);

    // Snapshot under the handlers lock, then invoke without it, so a handler
    // may register_handler()/clear_handlers() from within on_event().
    std::vector<std::shared_ptr<IAlertHandler>> snapshot;
    {
        std::lock_guard lock(handlers_mutex_);
        snapshot = handlers_;
    }
    for (auto& handler : snapshot) {
        handler->on_event(event);
    }
}

} // namespace rpi
