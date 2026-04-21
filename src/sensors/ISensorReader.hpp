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
    std::string sensor_id;
    std::string metric;   // e.g. "temperature", "pressure"
    float       value;
};

class ISensorReader {
public:
    virtual ~ISensorReader() = default;

    virtual auto read() -> std::expected<SensorReading, SensorError> = 0;
    virtual auto sensor_id() const -> std::string = 0;
};

} // namespace rpi
