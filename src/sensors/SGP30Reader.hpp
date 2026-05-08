#pragma once

#include "ISensorReader.hpp"
#include <string>

namespace rpi {

// Reads eCO2 or TVOC from an SGP30 via the Linux IIO kernel driver.
// device_path: IIO device directory, e.g. /sys/bus/iio/devices/iio:device0
// metric: "eco2" (in_concentration_co2_input, ppm) or "tvoc" (in_concentration_voc_input, ppb)
class SGP30Reader final : public ISensorReader {
public:
    SGP30Reader(std::string device_path, std::string id, std::string metric);

    auto read()      -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

private:
    std::string device_path_;
    std::string sensor_id_;
    std::string metric_;
};

} // namespace rpi
