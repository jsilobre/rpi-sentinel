#pragma once

#include "../events/ThermalEvent.hpp"

namespace rpi {

class IAlertHandler {
public:
    virtual ~IAlertHandler() = default;
    virtual void on_event(const ThermalEvent& event) = 0;
};

} // namespace rpi
