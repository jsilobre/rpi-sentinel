#pragma once

#include "IAlertHandler.hpp"
#include <filesystem>

namespace rpi {

// Drives a GPIO output pin via the Linux sysfs interface.
// HIGH on ThresholdExceeded, LOW on ThresholdRecovered.
// sysfs_root defaults to /sys/class/gpio; override in tests.
class GpioAlert final : public IAlertHandler {
public:
    explicit GpioAlert(int pin, std::filesystem::path sysfs_root = "/sys/class/gpio");
    ~GpioAlert() override;

    GpioAlert(const GpioAlert&)            = delete;
    GpioAlert& operator=(const GpioAlert&) = delete;

    void on_event(const SensorEvent& event) override;

private:
    void set_pin(bool active);

    int                   pin_;
    std::filesystem::path sysfs_root_;
    bool                  ready_{false};
};

} // namespace rpi
