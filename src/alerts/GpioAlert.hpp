#pragma once

#include "IAlertHandler.hpp"
#include <filesystem>
#include <string>
#include <unordered_map>

namespace rpi {

// Drives a GPIO output pin via the Linux sysfs interface.
// HIGH while at least one sensor has an unrecovered ThresholdExceeded,
// LOW once every alert has recovered.
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
    // Unrecovered alerts per sensor (warn and crit each count once).
    // No mutex needed: EventBus serializes all on_event() calls.
    std::unordered_map<std::string, int> active_alerts_;
};

} // namespace rpi
