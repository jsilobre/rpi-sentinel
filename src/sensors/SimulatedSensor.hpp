#pragma once

#include "ISensorReader.hpp"
#include <functional>
#include <string>

namespace rpi {

// Sensor driven by a user-supplied generator function.
// Default: sinusoidal variation around a base value.
class SimulatedSensor final : public ISensorReader {
public:
    using Generator = std::function<float()>;

    explicit SimulatedSensor(std::string id,
                             std::string metric    = "temperature",
                             Generator   generator = {});

    auto read() -> std::expected<SensorReading, SensorError> override;
    auto sensor_id() const -> std::string override;

    static auto make_sinusoidal(float base, float amplitude, float period_seconds) -> Generator;
    static auto make_motion(float period_seconds = 25.0f, float active_seconds = 5.0f) -> Generator;
    static auto make_for_metric(std::string_view metric) -> Generator;

private:
    std::string sensor_id_;
    std::string metric_;
    Generator   generator_;
};

} // namespace rpi
