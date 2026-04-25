#include "SqliteHistoryHandler.hpp"
#include "HistoryStore.hpp"

namespace rpi {

SqliteHistoryHandler::SqliteHistoryHandler(std::shared_ptr<HistoryStore> store)
    : store_(std::move(store))
{}

void SqliteHistoryHandler::on_event(const SensorEvent& event)
{
    if (event.type != SensorEvent::Type::Reading) return;
    if (!store_) return;
    store_->insert(event.sensor_id, event.metric, event.value, event.timestamp);
}

} // namespace rpi
