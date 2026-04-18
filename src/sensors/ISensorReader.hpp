#pragma once

#include <expected>
#include <string>

namespace rpi {

enum class SensorError {
    ReadFailure,
    DeviceNotFound,
    ParseError,
};

struct SensorReading {
    float       temperature_celsius;
    std::string sensor_id;
};

class ISensorReader {
public:
    virtual ~ISensorReader() = default;

    virtual auto read() -> std::expected<SensorReading, SensorError> = 0;
    virtual auto sensor_id() const -> std::string = 0;
};

} // namespace rpi
