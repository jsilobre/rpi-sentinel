#include "WebAlert.hpp"

namespace rpi {

WebAlert::WebAlert(WebState& state) : state_(state) {}

void WebAlert::on_event(const ThermalEvent& event)
{
    if (event.type == ThermalEvent::Type::Reading) {
        state_.push_reading(event.temperature, event.timestamp);
    } else {
        state_.push_alert(event);
    }
}

} // namespace rpi
