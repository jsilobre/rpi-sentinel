#pragma once

#include "ISensorReader.hpp"
#include "Sgp30Device.hpp"

#include <memory>
#include <string>

namespace rpi {

// Reads eCO2 or TVOC from an SGP30. Two backends, chosen by device_path:
//
// - I2C bus (device_path starting with /dev/, e.g. /dev/i2c-1): the chip is
//   driven from user space by a shared Sgp30Device, which persists the IAQ
//   baseline across restarts and applies humidity compensation. The sgp30
//   kernel module must not be bound to the chip.
// - IIO kernel driver (legacy, e.g. /sys/bus/iio/devices/iio:device0): reads
//   in_concentration_co2_input / in_concentration_voc_input. The driver keeps
//   no baseline, so every reboot restarts the 12 h early-operation phase.
//
// metric: "eco2" (ppm) or "tvoc" (ppb)
class SGP30Reader final : public ISensorReader {
public:
    SGP30Reader(std::string device_path, std::string id, std::string metric,
                Sgp30Options options = {});
    // I2C backend on an explicit device (tests).
    SGP30Reader(std::shared_ptr<Sgp30Device> device, std::string id, std::string metric);

    auto read()      -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

private:
    auto read_iio() -> std::expected<SensorReading, SensorError>;
    auto read_i2c() -> std::expected<SensorReading, SensorError>;

    std::string device_path_;
    std::string sensor_id_;
    std::string metric_;
    std::shared_ptr<Sgp30Device> device_;   // null for the IIO backend
};

} // namespace rpi
