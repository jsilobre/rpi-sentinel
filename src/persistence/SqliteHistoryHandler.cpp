#include "SqliteHistoryHandler.hpp"
#include "HistoryStore.hpp"

#include <chrono>
#include <string>

namespace rpi {

SqliteHistoryHandler::SqliteHistoryHandler(std::shared_ptr<HistoryStore> store)
    : store_(std::move(store))
{}

void SqliteHistoryHandler::on_event(const SensorEvent& event)
{
    if (!store_) return;
    if (event.type == SensorEvent::Type::Reading) {
        store_->insert(event.sensor_id, event.metric, event.value, event.timestamp);
        return;
    }
    store_->insert_alert({
        .ts_ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                         event.timestamp.time_since_epoch()).count(),
        .sensor_id = event.sensor_id,
        .metric    = event.metric,
        .type      = std::string(alert_type_string(event.type)),
        .level     = std::string(to_string(event.level)),
        .value     = event.value,
        .threshold = event.threshold,
    });
}

} // namespace rpi
