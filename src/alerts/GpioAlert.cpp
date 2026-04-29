#include "GpioAlert.hpp"

#include <format>
#include <fstream>
#include <print>

namespace rpi {

GpioAlert::GpioAlert(int pin, std::filesystem::path sysfs_root)
    : pin_(pin)
    , sysfs_root_(std::move(sysfs_root))
{
    // Request export — may already be exported after an unclean shutdown, so
    // we silently continue even if this write fails.
    if (std::ofstream f(sysfs_root_ / "export"); f.is_open()) {
        f << pin_;
    }

    auto direction_path = sysfs_root_ / std::format("gpio{}", pin_) / "direction";
    if (std::ofstream f(direction_path); f.is_open()) {
        f << "out";
        ready_ = true;
        std::println("[GpioAlert] GPIO pin {} configured as output.", pin_);
    } else {
        std::println(stderr, "[GpioAlert] GPIO pin {} not available — running without hardware output.", pin_);
    }
}

GpioAlert::~GpioAlert()
{
    if (!ready_) return;
    set_pin(false);
    if (std::ofstream f(sysfs_root_ / "unexport"); f.is_open()) {
        f << pin_;
    }
}

void GpioAlert::on_event(const SensorEvent& event)
{
    if (event.type == SensorEvent::Type::Reading) return;
    set_pin(event.type == SensorEvent::Type::ThresholdExceeded);
}

void GpioAlert::set_pin(bool active)
{
    auto value_path = sysfs_root_ / std::format("gpio{}", pin_) / "value";
    if (std::ofstream f(value_path); f.is_open()) {
        f << (active ? '1' : '0');
    }
}

} // namespace rpi
