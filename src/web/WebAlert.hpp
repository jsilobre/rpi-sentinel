#pragma once

#include "../alerts/IAlertHandler.hpp"
#include "WebState.hpp"

namespace rpi {

class WebAlert final : public IAlertHandler {
public:
    explicit WebAlert(WebState& state);
    void on_event(const SensorEvent& event) override;

private:
    WebState& state_;
};

} // namespace rpi
