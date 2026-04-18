#pragma once

#include "ISensorReader.hpp"
#include <functional>
#include <string>

namespace rpi {

// Sensor driven by a user-supplied generator function.
// Default: sinusoidal variation around a base temperature.
class SimulatedSensor final : public ISensorReader {
public:
    using Generator = std::function<float()>;

    explicit SimulatedSensor(std::string id, Generator generator = {});

    auto read() -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

    static auto make_sinusoidal(float base, float amplitude, float period_seconds) -> Generator;

private:
    std::string sensor_id_;
    Generator   generator_;
};

} // namespace rpi
