#pragma once

#include "IAlertHandler.hpp"

namespace rpi {

class LogAlert final : public IAlertHandler {
public:
    void on_event(const SensorEvent& event) override;
};

} // namespace rpi
