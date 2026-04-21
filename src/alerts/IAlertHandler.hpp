#pragma once

#include "../events/SensorEvent.hpp"

namespace rpi {

class IAlertHandler {
public:
    virtual ~IAlertHandler() = default;
    virtual void on_event(const SensorEvent& event) = 0;
};

} // namespace rpi
