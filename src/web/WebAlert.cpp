#include "WebAlert.hpp"

namespace rpi {

WebAlert::WebAlert(WebState& state) : state_(state) {}

void WebAlert::on_event(const SensorEvent& event)
{
    if (event.type == SensorEvent::Type::Reading) {
        state_.push_reading(event);
    } else {
        state_.push_alert(event);
    }
}

} // namespace rpi
