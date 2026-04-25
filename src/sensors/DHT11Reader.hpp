#pragma once

#include "ISensorReader.hpp"
#include <string>

namespace rpi {

// Reads temperature or humidity from a DHT11 via the Linux IIO kernel driver.
// device_path: IIO device directory, e.g. /sys/bus/iio/devices/iio:device0
// metric: "temperature" (in_temp_input) or "humidity" (in_humidityrelative_input)
class DHT11Reader final : public ISensorReader {
public:
    DHT11Reader(std::string device_path, std::string id, std::string metric);

    auto read()      -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

private:
    std::string device_path_;
    std::string sensor_id_;
    std::string metric_;
};

} // namespace rpi
